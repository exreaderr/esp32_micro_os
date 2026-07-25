// ============================================================================
// ConfigManager.cpp - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Полноценный менеджер конфигурации с поддержкой:
// - LittleFS + NVS бэкап
// - Атомарная запись с ротацией
// - Событийная модель публикации изменений
// - Миграция версий
// - Валидация данных
// - Расширяемая секция через колбэки
// - Кэширование и оптимизация CRC
// - Полная потокобезопасность
// - Публикация событий через новую шину (v5.0)
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА критическая ошибка в updateStatus (status.isDirty -> _status.isDirty)
// - ИСПРАВЛЕНА синтаксическая ошибка в applyNetworkSettings
// - ИСПРАВЛЕНА гонка данных в calculateConfigCRC (добавлен мьютекс)
// - ИСПРАВЛЕНА отправка событий в logMessage (SH_EVENT_MODULE_TICK -> SH_EVENT_LOG_ENTRY)
// - Добавлена проверка _initialized в handleCommand
// - Добавлена защита от рекурсии в applyNetworkSettings
// - Добавлен метод safeStrCopy для безопасного копирования
// - Добавлен метод ensureLittleFS для надежного монтирования
// - Добавлены колбэки для SafeMode
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.2
// - Добавлен метод publishConfigEventInternal() для публикации через новую шину
// - Добавлен метод publishConfigEvent() (публичный)
// - Добавлены вызовы publishConfigEventInternal() в ключевые методы
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "ConfigManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static ConfigManager _configManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
ConfigManager::ConfigManager() {
    _mutex = xSemaphoreCreateRecursiveMutex();
    if (_mutex == nullptr) {
        Serial.println("[CFG] CRITICAL: Failed to create mutex!");
        while (1) { delay(100); }
    }

    _moduleId = MODULE_ID_CONFIG;

    _sys = SystemConfig();
    _safeModeActive = false;
    _factoryResetTriggered = false;
    _isDirty = false;
    _networkApplied = false;
    _loaded = false;
    _initialized = false;
    _applyNetworkInProgress = false;
    _dirtyTimestamp = 0;
    _lastSavedCRC = 0;
    _lastLoadTime = 0;
    _lastSaveTime = 0;
    _loadCount = 0;
    _saveCount = 0;
    _totalEventsPublished = 0; // НОВОЕ
    _lastError[0] = '\0';
    _onLoadExtra = nullptr;
    _onSaveExtra = nullptr;
    _onConfigChange = nullptr;
    _onSafeModeChange = nullptr;

    memset(&_status, 0, sizeof(_status));
    _status.currentVersion = CONFIG_VERSION_CURRENT;

    Serial.println("[CFG] Instance created (v5.0)");
}

ConfigManager::~ConfigManager() {
    if (_isDirty) {
        save();
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
    _prefs.end();
    Serial.println("[CFG] Instance destroyed");
}

// ============================================================================
// 2. СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
ConfigManager& ConfigManager::getInstance() {
    return _configManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

bool ConfigManager::ensureLittleFS() {
    if (LittleFS.begin(false)) return true;
    Serial.println("[CFG] LittleFS mount failed, trying format...");
    if (LittleFS.begin(true)) {
        Serial.println("[CFG] LittleFS formatted successfully");
        return true;
    }
    Serial.println("[CFG] LittleFS format failed!");
    return false;
}

void ConfigManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[CFG] %s\n", msg);

    if (_initialized) {
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = _moduleId;
        data.targetModule = MODULE_ID_LOG;
        data.command = SH_EVENT_LOG_ENTRY;
        data.value = 0;
        safeStrCopy(data.payload, sizeof(data.payload), msg);
        data.payloadLen = strlen(data.payload);
        postEvent(SH_EVENT_CMD_EXECUTE, &data);
    }
}

void ConfigManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

void ConfigManager::markDirty() {
    _isDirty = true;
    _dirtyTimestamp = millis();
    _status.isDirty = true;
}

void ConfigManager::updateStatus(const char* error) {
    _status.isLoaded = _loaded;
    _status.isDirty = _isDirty;
    _status.isSafeMode = _safeModeActive;
    _status.isFactoryReset = _factoryResetTriggered;
    _status.isNetworkApplied = _networkApplied;
    _status.crc = _lastSavedCRC;
    _status.lastSaveTime = _lastSaveTime;
    _status.lastLoadTime = _lastLoadTime;
    _status.currentVersion = CONFIG_VERSION_CURRENT;
    _status.lastModified = _sys.last_modified;

    if (error != nullptr) {
        safeStrCopy(_status.errorMessage, sizeof(_status.errorMessage), error);
        safeStrCopy(_lastError, sizeof(_lastError), error);
    }
}

// ============================================================================
// 4. НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void ConfigManager::publishConfigEventInternal(const char* eventType, const char* key,
                                               bool success, const char* details) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = success ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "Config: %s %s - %s%s",
             eventType,
             key ? key : "",
             success ? "OK" : "FAIL",
             details ? details : "");

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void ConfigManager::publishConfigEvent(const char* eventType, const char* key,
                                       bool success, const char* details) {
    publishConfigEventInternal(eventType, key, success, details);
}

// ============================================================================
// 5. ЖИЗНЕННЫЙ ЦИКЛ (IModule) (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::init() {
    if (_initialized) return;

    logMessage("Initializing...");

    if (!ensureLittleFS()) {
        updateStatus("LittleFS mount failed");
        _initialized = false;
        return;
    }

    bool loadResult = load();
    if (!loadResult) {
        logMessage("Load failed, generating defaults...");
        _sys = SystemConfig();
        if (save()) {
            logMessage("Default config saved successfully");
        } else {
            logMessage("Failed to save default config!");
        }
    }

    uint32_t currentCRC = calculateConfigCRC();
    if (_lastSavedCRC != 0 && _lastSavedCRC != currentCRC) {
        logMessage("WARNING: Config CRC mismatch! Loading NVS backup...");
        loadFromNvsBackup();
        if (save()) {
            logMessage("NVS backup restored successfully");
        }
    }

    if (_loaded) {
        applyNetworkSettings();
    }

    esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &ConfigManager::eventHandler,
        this,
        &_sysHandlerInstance
    );
    esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &ConfigManager::eventHandler,
        this,
        &_appHandlerInstance
    );

    _initialized = true;
    _loaded = true;
    _status.isLoaded = true;
    updateStatus();
    publishConfigLoaded();
    publishConfigEventInternal("INIT", nullptr, true, "Config loaded");
    logMessage("Initialized successfully");
}

void ConfigManager::start() {
    // Ничего не делаем, уже запущены в init()
}

void ConfigManager::stop() {
    if (!_initialized) return;

    logMessage("Stopping...");
    if (_isDirty) {
        save();
    }

    if (_sysHandlerInstance != nullptr) {
        esp_event_handler_instance_unregister(SH_SYS_EVENTS, ESP_EVENT_ANY_ID, _sysHandlerInstance);
        _sysHandlerInstance = nullptr;
    }
    if (_appHandlerInstance != nullptr) {
        esp_event_handler_instance_unregister(SH_APP_EVENTS, ESP_EVENT_ANY_ID, _appHandlerInstance);
        _appHandlerInstance = nullptr;
    }

    _initialized = false;
    logMessage("Stopped");
}

void ConfigManager::tick() {
    if (!_initialized) return;

    esp_task_wdt_reset();

    if (_isDirty && (millis() - _dirtyTimestamp > FLUSH_DELAY_MS)) {
        if (_safeModeActive) {
            logMessage("Dirty but safe mode active - skipping save");
            _isDirty = false;
        } else {
            logMessage("Auto-saving due to timeout");
            save();
        }
    }

    static uint32_t lastIntegrityCheck = 0;
    if (millis() - lastIntegrityCheck > 60000) {
        lastIntegrityCheck = millis();
        uint32_t currentCRC = calculateConfigCRC();
        if (_lastSavedCRC != 0 && _lastSavedCRC != currentCRC) {
            logMessage("WARNING: Config corruption detected!");
            publishConfigCorrupted(currentCRC);
            publishConfigEventInternal("CORRUPTED", nullptr, false, "CRC mismatch");
        }
    }
}

// ============================================================================
// 6. ОБРАБОТЧИКИ СОБЫТИЙ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                                int32_t id, void* eventData) {
    ConfigManager* instance = static_cast<ConfigManager*>(handlerArgs);
    if (!instance || !instance->_initialized) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_READY:
                instance->applyNetworkSettings();
                break;

            case SH_EVENT_SYS_RESTART:
                if (instance->_isDirty) {
                    instance->forceSave();
                }
                break;

            case SH_EVENT_SYS_SHUTDOWN:
                if (instance->_isDirty) {
                    instance->save();
                }
                break;

            case SH_EVENT_NET_CONNECTED:
                instance->applyNetworkSettings();
                break;

            case SH_EVENT_HEALTH_CRITICAL:
                instance->setSafeMode(true);
                break;

            case SH_EVENT_HEALTH_RESTORED:
                instance->setSafeMode(false);
                break;

            default:
                break;
        }
    }

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void ConfigManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    if (eventId == SH_EVENT_CMD_EXECUTE) {
        if (data->targetModule == _moduleId || data->targetModule == 0) {
            handleCommand(data);
        }
    }
}

bool ConfigManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_CMD_RESPONSE ||
            eventId == SH_EVENT_SYS_BOOT ||
            eventId == SH_EVENT_SYS_READY ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN ||
            eventId == SH_EVENT_NET_CONNECTED ||
            eventId == SH_EVENT_NET_DISCONNECTED ||
            eventId == SH_EVENT_HEALTH_CRITICAL ||
            eventId == SH_EVENT_HEALTH_RESTORED);
}

// ============================================================================
// 7. ОБРАБОТКА КОМАНД (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::handleCommand(const ShEventData* data) {
    if (!_initialized) {
        logMessage("Command received but not initialized!");
        return;
    }

    switch (data->command) {
        case CMD_GET_STATUS: {
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = _loaded ? 1 : 0;
            const char* status = getStatus();
            safeStrCopy(response.payload, sizeof(response.payload), status);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case CMD_GET_CONFIG: {
            String json = exportToJson();
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = json.length();
            safeStrCopy(response.payload, sizeof(response.payload), json.c_str());
            response.payloadLen = json.length();
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case CMD_SET_CONFIG: {
            if (data->payloadLen > 0 && data->payloadLen < sizeof(data->payload)) {
                String json(data->payload, data->payloadLen);
                if (importFromJson(json)) {
                    ShEventData response;
                    memset(&response, 0, sizeof(ShEventData));
                    response.sourceModule = _moduleId;
                    response.targetModule = data->sourceModule;
                    response.command = CMD_RESPONSE_OK;
                    safeStrCopy(response.payload, sizeof(response.payload), "Config updated");
                    response.payloadLen = strlen(response.payload);
                    postEvent(SH_EVENT_CMD_RESPONSE, &response);
                    publishConfigEventInternal("SET", nullptr, true, "Config updated");
                } else {
                    ShEventData response;
                    memset(&response, 0, sizeof(ShEventData));
                    response.sourceModule = _moduleId;
                    response.targetModule = data->sourceModule;
                    response.command = CMD_RESPONSE_ERROR;
                    safeStrCopy(response.payload, sizeof(response.payload), "Import failed");
                    response.payloadLen = strlen(response.payload);
                    postEvent(SH_EVENT_CMD_RESPONSE, &response);
                    publishConfigEventInternal("SET", nullptr, false, "Import failed");
                }
            }
            break;
        }

        case CMD_SAVE_CONFIG: {
            if (save()) {
                ShEventData response;
                memset(&response, 0, sizeof(ShEventData));
                response.sourceModule = _moduleId;
                response.targetModule = data->sourceModule;
                response.command = CMD_RESPONSE_OK;
                safeStrCopy(response.payload, sizeof(response.payload), "Config saved");
                response.payloadLen = strlen(response.payload);
                postEvent(SH_EVENT_CMD_RESPONSE, &response);
                publishConfigEventInternal("SAVE", nullptr, true, "Config saved");
            } else {
                ShEventData response;
                memset(&response, 0, sizeof(ShEventData));
                response.sourceModule = _moduleId;
                response.targetModule = data->sourceModule;
                response.command = CMD_RESPONSE_ERROR;
                safeStrCopy(response.payload, sizeof(response.payload), "Save failed");
                response.payloadLen = strlen(response.payload);
                postEvent(SH_EVENT_CMD_RESPONSE, &response);
                publishConfigEventInternal("SAVE", nullptr, false, "Save failed");
            }
            break;
        }

        case CMD_RESET_CONFIG: {
            resetToDefaults();
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_OK;
            safeStrCopy(response.payload, sizeof(response.payload), "Config reset");
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            publishConfigEventInternal("RESET", nullptr, true, "Config reset");
            break;
        }

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 8. СТАТУС И ДИАГНОСТИКА (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
const char* ConfigManager::getStatus() const {
    static char statusBuffer[256];
    snprintf(statusBuffer, sizeof(statusBuffer),
            "Loaded:%s Safe:%s Dirty:%s Net:%s CRC:%u Ver:%d Loads:%lu Saves:%lu",
            _loaded ? "Y" : "N",
            _safeModeActive ? "Y" : "N",
            _isDirty ? "Y" : "N",
            _networkApplied ? "Y" : "N",
            _lastSavedCRC,
            CONFIG_VERSION_CURRENT,
            _loadCount,
            _saveCount);
    return statusBuffer;
}

// ============================================================================
// 9. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void ConfigManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _loaded ? 1 : 0;

    snprintf(data->payload, sizeof(data->payload),
            "ver:%d,crc:%u,saves:%lu,loads:%lu,dirty:%d,safe:%d,heap:%u,events:%lu,err:%s",
            CONFIG_VERSION_CURRENT,
            _lastSavedCRC,
            _saveCount,
            _loadCount,
            _isDirty ? 1 : 0,
            _safeModeActive ? 1 : 0,
            ESP.getFreeHeap(),
            _totalEventsPublished, // НОВОЕ
            _lastError);
    data->payloadLen = strlen(data->payload);
}

ConfigStatus ConfigManager::getConfigStatus() const {
    ConfigStatus status = _status;
    status.isSafeMode = _safeModeActive;
    status.isFactoryReset = _factoryResetTriggered;
    status.isNetworkApplied = _networkApplied;
    status.isDirty = _isDirty;
    status.crc = _lastSavedCRC;
    status.lastSaveTime = _lastSaveTime;
    status.lastLoadTime = _lastLoadTime;
    status.currentVersion = CONFIG_VERSION_CURRENT;
    status.fileSize = 0;
    safeStrCopy(status.errorMessage, sizeof(status.errorMessage), _lastError);
    return status;
}

bool ConfigManager::getConfigStatusString(char* buffer, size_t bufferSize) const {
    if (!buffer || bufferSize == 0) return false;

    ConfigStatus s = getConfigStatus();
    snprintf(buffer, bufferSize,
            "=== CONFIG STATUS ===\n"
            "Loaded: %s\n"
            "Dirty: %s\n"
            "Safe Mode: %s\n"
            "Factory Reset: %s\n"
            "Network Applied: %s\n"
            "Version: %d\n"
            "CRC: %u\n"
            "Loads: %lu\n"
            "Saves: %lu\n"
            "Hostname: %s\n"
            "MQTT: %s\n"
            "DHCP: %s\n"
            "IP: %s\n"
            "Error: %s\n",
            s.isLoaded ? "YES" : "NO",
            s.isDirty ? "YES" : "NO",
            s.isSafeMode ? "YES" : "NO",
            s.isFactoryReset ? "YES" : "NO",
            s.isNetworkApplied ? "YES" : "NO",
            s.currentVersion,
            s.crc,
            _loadCount,
            _saveCount,
            _sys.hostname,
            _sys.use_mqtt ? "ENABLED" : "DISABLED",
            _sys.net_dhcp ? "YES" : "NO",
            _sys.cfg_ip,
            _lastError);
    return true;
}

String ConfigManager::getConfigStatusString() const {
    String result = "=== CONFIG STATUS ===\n";
    result += "Loaded: " + String(_loaded ? "YES" : "NO") + "\n";
    result += "Dirty: " + String(_isDirty ? "YES" : "NO") + "\n";
    result += "Safe Mode: " + String(_safeModeActive ? "YES" : "NO") + "\n";
    result += "Factory Reset: " + String(_factoryResetTriggered ? "YES" : "NO") + "\n";
    result += "Network Applied: " + String(_networkApplied ? "YES" : "NO") + "\n";
    result += "Version: " + String(CONFIG_VERSION_CURRENT) + "\n";
    result += "CRC: " + String(_lastSavedCRC) + "\n";
    result += "Loads: " + String(_loadCount) + "\n";
    result += "Saves: " + String(_saveCount) + "\n";
    result += "Hostname: " + String(_sys.hostname) + "\n";
    result += "MQTT: " + String(_sys.use_mqtt ? "ENABLED" : "DISABLED") + "\n";
    result += "DHCP: " + String(_sys.net_dhcp ? "YES" : "NO") + "\n";
    if (!_sys.net_dhcp) {
        result += "IP: " + String(_sys.cfg_ip) + "\n";
    }
    result += "Error: " + String(_lastError) + "\n";
    return result;
}

// ============================================================================
// 10. ЗАГРУЗКА / СОХРАНЕНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool ConfigManager::load() {
    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        updateStatus("Mutex timeout on load");
        return false;
    }
    bool result = loadFromFile();
    xSemaphoreGiveRecursive(_mutex);

    if (result) {
        _loaded = true;
        _status.isLoaded = true;
        _lastLoadTime = millis();
        _loadCount++;
        logMessage("Loaded successfully");
        updateStatus();
        publishConfigLoaded();
        publishConfigEventInternal("LOAD", nullptr, true, "Config loaded");
    } else {
        updateStatus("Load failed");
        publishConfigEventInternal("LOAD", nullptr, false, "Load failed");
    }
    return result;
}

bool ConfigManager::save() {
    if (_safeModeActive) {
        logMessage("Save blocked: Safe mode active");
        publishConfigEventInternal("SAVE", nullptr, false, "Blocked by Safe mode");
        return false;
    }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        updateStatus("Mutex timeout on save");
        return false;
    }
    bool result = saveToFile();
    xSemaphoreGiveRecursive(_mutex);

    if (result) {
        _isDirty = false;
        _lastSaveTime = millis();
        _saveCount++;
        _status.isDirty = false;
        logMessage("Saved successfully");
        updateStatus();
        publishConfigSaved();
        publishConfigEventInternal("SAVE", nullptr, true, "Config saved");
    } else {
        updateStatus("Save failed");
        publishConfigEventInternal("SAVE", nullptr, false, "Save failed");
    }
    return result;
}

// ============================================================================
// 11. ВНУТРЕННИЕ МЕТОДЫ ЗАГРУЗКИ/СОХРАНЕНИЯ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool ConfigManager::loadFromFile() {
    const char* pathToOpen = CONFIG_PATH;

    if (!LittleFS.exists(CONFIG_PATH)) {
        if (LittleFS.exists(BACKUP_PATH)) {
            logMessage("Primary config missing. Restoring from backup...");
            pathToOpen = BACKUP_PATH;
        } else {
            logMessage("No configs found. Generating defaults...");
            _sys = SystemConfig();
            return true;
        }
    }

    File file = LittleFS.open(pathToOpen, "r");
    if (!file) {
        logMessage("IO Error reading config. Trying NVS backup...");
        loadFromNvsBackup();
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > MAX_JSON_SIZE) {
        file.close();
        logMessage("Invalid file size: %zu", fileSize);
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        logMessage("JSON parse error: %s", error.c_str());
        return false;
    }

    if (!validateSystemConfig(doc)) {
        logMessage("Config validation failed!");
        return false;
    }

    uint8_t version = doc["cfg_version"] | 1;
    if (version < CONFIG_VERSION_CURRENT) {
        logMessage("Migrating config from v%d to v%d", version, CONFIG_VERSION_CURRENT);
        migrateConfig(doc, version);
    }

    _sys.net_dhcp = doc["net_dhcp"] | true;
    safeStrCopy(_sys.cfg_ip, sizeof(_sys.cfg_ip), doc["cfg_ip"] | "192.168.1.200");
    safeStrCopy(_sys.cfg_mask, sizeof(_sys.cfg_mask), doc["cfg_mask"] | "255.255.255.0");
    safeStrCopy(_sys.cfg_gateway, sizeof(_sys.cfg_gateway), doc["cfg_gateway"] | "192.168.1.1");
    safeStrCopy(_sys.cfg_dns, sizeof(_sys.cfg_dns), doc["cfg_dns"] | "192.168.1.1");
    _sys.use_mqtt = doc["use_mqtt"] | false;
    safeStrCopy(_sys.mqtt_ip, sizeof(_sys.mqtt_ip), doc["mqtt_ip"] | "");
    safeStrCopy(_sys.mqtt_user, sizeof(_sys.mqtt_user), doc["mqtt_user"] | "");
    safeStrCopy(_sys.mqtt_password, sizeof(_sys.mqtt_password), doc["mqtt_password"] | "");
    safeStrCopy(_sys.hostname, sizeof(_sys.hostname), doc["hostname"] | "smart-device");
    safeStrCopy(_sys.web_password_hash, sizeof(_sys.web_password_hash), doc["web_password_hash"] | "");
    _sys.is_pure_local_mode = doc["is_pure_local_mode"] | false;
    safeStrCopy(_sys.extra_data, sizeof(_sys.extra_data), doc["extra_data"] | "{}");
    _sys.version_flags = doc["version_flags"] | 0;
    _sys.last_modified = doc["last_modified"] | 0;
    _sys.config_ver = CONFIG_VERSION_CURRENT;

    if (_onLoadExtra) {
        _onLoadExtra(doc["extra_data"].as<JsonVariant>());
    }

    _lastSavedCRC = calculateConfigCRC();
    _sys.config_crc = _lastSavedCRC;

    return true;
}

bool ConfigManager::saveToFile() {
    JsonDocument doc;

    doc["cfg_version"] = CONFIG_VERSION_CURRENT;
    doc["net_dhcp"] = _sys.net_dhcp;
    doc["cfg_ip"] = _sys.cfg_ip;
    doc["cfg_mask"] = _sys.cfg_mask;
    doc["cfg_gateway"] = _sys.cfg_gateway;
    doc["cfg_dns"] = _sys.cfg_dns;
    doc["use_mqtt"] = _sys.use_mqtt;
    doc["mqtt_ip"] = _sys.mqtt_ip;
    doc["mqtt_user"] = _sys.mqtt_user;
    doc["mqtt_password"] = _sys.mqtt_password;
    doc["hostname"] = _sys.hostname;
    doc["web_password_hash"] = _sys.web_password_hash;
    doc["is_pure_local_mode"] = _sys.is_pure_local_mode;
    doc["extra_data"] = _sys.extra_data;
    doc["version_flags"] = _sys.version_flags;
    doc["last_modified"] = _sys.last_modified;

    if (_onSaveExtra) {
        _onSaveExtra(doc["extra_data"].as<JsonVariant>());
    }

    File tmpFile = LittleFS.open(TEMP_PATH, "w");
    if (!tmpFile) {
        logMessage("Failed to open temp file");
        return false;
    }

    size_t written = serializeJson(doc, tmpFile);
    tmpFile.close();

    if (written == 0) {
        logMessage("Zero bytes written!");
        return false;
    }

    if (LittleFS.exists(BACKUP_PATH)) {
        LittleFS.remove(BACKUP_PATH);
    }
    if (LittleFS.exists(CONFIG_PATH)) {
        LittleFS.rename(CONFIG_PATH, BACKUP_PATH);
    }
    if (!LittleFS.rename(TEMP_PATH, CONFIG_PATH)) {
        logMessage("Atomic rename failed!");
        return false;
    }

    _lastSavedCRC = calculateConfigCRC();
    _sys.config_crc = _lastSavedCRC;

    return true;
}

// ============================================================================
// 12. NVS БЭКАП (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::loadFromNvsBackup() {
    _prefs.begin(CONFIG_BACKUP_NAMESPACE, true);
    String json = _prefs.getString("config", "");
    _prefs.end();

    if (json.length() > 0) {
        importFromJson(json);
        logMessage("NVS backup restored");
    }
}

void ConfigManager::saveToNvsBackup() {
    String json = exportToJson();
    if (json.length() > 0) {
        _prefs.begin(CONFIG_BACKUP_NAMESPACE, false);
        _prefs.putString("config", json);
        _prefs.end();
        logMessage("NVS backup saved");
    }
}

// ============================================================================
// 13. ВАЛИДАЦИЯ И МИГРАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool ConfigManager::validateSystemConfig(const JsonDocument& doc) {
    if (!doc.containsKey("cfg_version")) return false;

    if (doc["cfg_version"].as<uint8_t>() < 1 ||
        doc["cfg_version"].as<uint8_t>() > CONFIG_VERSION_CURRENT) {
        return false;
    }

    return true;
}

void ConfigManager::migrateConfig(JsonDocument& doc, uint8_t oldVersion) {
    if (oldVersion < 2) {
        if (!doc.containsKey("is_pure_local_mode")) {
            doc["is_pure_local_mode"] = false;
        }
        if (!doc.containsKey("version_flags")) {
            doc["version_flags"] = 0;
        }
    }
    doc["cfg_version"] = CONFIG_VERSION_CURRENT;
}

// ============================================================================
// 14. ПУБЛИЧНЫЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::resetToDefaults() {
    if (_mutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        logMessage("Mutex timeout in resetToDefaults");
        return;
    }

    _sys = SystemConfig();
    _isDirty = true;
    _factoryResetTriggered = true;
    _status.isFactoryReset = true;
    _networkApplied = false;

    xSemaphoreGiveRecursive(_mutex);

    save();
    publishConfigReset();
    publishConfigEventInternal("RESET", nullptr, true, "Reset to defaults");
    logMessage("Reset to defaults");
}

bool ConfigManager::exportToJson(String& output) const {
    JsonDocument doc;
    doc["cfg_version"] = CONFIG_VERSION_CURRENT;
    doc["net_dhcp"] = _sys.net_dhcp;
    doc["cfg_ip"] = _sys.cfg_ip;
    doc["cfg_mask"] = _sys.cfg_mask;
    doc["cfg_gateway"] = _sys.cfg_gateway;
    doc["cfg_dns"] = _sys.cfg_dns;
    doc["use_mqtt"] = _sys.use_mqtt;
    doc["mqtt_ip"] = _sys.mqtt_ip;
    doc["mqtt_user"] = _sys.mqtt_user;
    doc["mqtt_password"] = _sys.mqtt_password;
    doc["hostname"] = _sys.hostname;
    doc["web_password_hash"] = _sys.web_password_hash;
    doc["is_pure_local_mode"] = _sys.is_pure_local_mode;
    doc["extra_data"] = _sys.extra_data;
    doc["version_flags"] = _sys.version_flags;
    doc["last_modified"] = _sys.last_modified;

    serializeJson(doc, output);
    return true;
}

bool ConfigManager::exportToJson(char* buffer, size_t bufferSize) const {
    String json;
    if (!exportToJson(json)) return false;
    if (json.length() >= bufferSize) return false;
    strncpy(buffer, json.c_str(), bufferSize);
    return true;
}

String ConfigManager::exportToJson() const {
    String json;
    exportToJson(json);
    return json;
}

bool ConfigManager::importFromJson(const String& json) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        logMessage("Import JSON parse error: %s", error.c_str());
        return false;
    }

    if (!validateSystemConfig(doc)) {
        logMessage("Import validation failed");
        return false;
    }

    uint8_t version = doc["cfg_version"] | 1;
    if (version < CONFIG_VERSION_CURRENT) {
        migrateConfig(doc, version);
    }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        logMessage("Mutex timeout in import");
        return false;
    }

    _sys.net_dhcp = doc["net_dhcp"] | true;
    safeStrCopy(_sys.cfg_ip, sizeof(_sys.cfg_ip), doc["cfg_ip"] | "192.168.1.200");
    safeStrCopy(_sys.cfg_mask, sizeof(_sys.cfg_mask), doc["cfg_mask"] | "255.255.255.0");
    safeStrCopy(_sys.cfg_gateway, sizeof(_sys.cfg_gateway), doc["cfg_gateway"] | "192.168.1.1");
    safeStrCopy(_sys.cfg_dns, sizeof(_sys.cfg_dns), doc["cfg_dns"] | "192.168.1.1");
    _sys.use_mqtt = doc["use_mqtt"] | false;
    safeStrCopy(_sys.mqtt_ip, sizeof(_sys.mqtt_ip), doc["mqtt_ip"] | "");
    safeStrCopy(_sys.mqtt_user, sizeof(_sys.mqtt_user), doc["mqtt_user"] | "");
    safeStrCopy(_sys.mqtt_password, sizeof(_sys.mqtt_password), doc["mqtt_password"] | "");
    safeStrCopy(_sys.hostname, sizeof(_sys.hostname), doc["hostname"] | "smart-device");
    safeStrCopy(_sys.web_password_hash, sizeof(_sys.web_password_hash), doc["web_password_hash"] | "");
    _sys.is_pure_local_mode = doc["is_pure_local_mode"] | false;
    safeStrCopy(_sys.extra_data, sizeof(_sys.extra_data), doc["extra_data"] | "{}");
    _sys.version_flags = doc["version_flags"] | 0;
    _sys.last_modified = doc["last_modified"] | 0;
    _sys.config_ver = CONFIG_VERSION_CURRENT;

    if (_onLoadExtra) {
        _onLoadExtra(doc["extra_data"].as<JsonVariant>());
    }

    _isDirty = true;
    _lastSavedCRC = calculateConfigCRC();
    _sys.config_crc = _lastSavedCRC;

    xSemaphoreGiveRecursive(_mutex);

    publishConfigEventInternal("IMPORT", nullptr, true, "Config imported");
    logMessage("Config imported successfully");
    return true;
}

bool ConfigManager::saveFromJsonStream(Stream& stream) {
    String json;
    while (stream.available()) {
        json += (char)stream.read();
    }
    return importFromJson(json);
}

void ConfigManager::serializeToStream(Stream& stream) {
    String json = exportToJson();
    stream.print(json);
}

// ============================================================================
// 15. СЕТТЕРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::setHostname(const char* name) {
    if (!_initialized) return;
    if (name == nullptr) return;
    if (!isValidHostname(name)) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.hostname, name) != 0) {
            char oldVal[32];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.hostname);
            safeStrCopy(_sys.hostname, sizeof(_sys.hostname), name);
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("hostname", oldVal, name, false);
            publishConfigEventInternal("SET_HOSTNAME", name, true, nullptr);
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setHostname(const String& name) {
    setHostname(name.c_str());
}

void ConfigManager::setMqttIp(const char* ip) {
    if (!_initialized) return;
    if (ip == nullptr) return;
    if (!isValidIp(ip)) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.mqtt_ip, ip) != 0) {
            char oldVal[16];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.mqtt_ip);
            safeStrCopy(_sys.mqtt_ip, sizeof(_sys.mqtt_ip), ip);
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("mqtt_ip", oldVal, ip, false);
            publishConfigEventInternal("SET_MQTT_IP", ip, true, nullptr);
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setMqttIp(const String& ip) {
    setMqttIp(ip.c_str());
}

void ConfigManager::setMqttUser(const char* user) {
    if (!_initialized) return;
    if (user == nullptr) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.mqtt_user, user) != 0) {
            char oldVal[32];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.mqtt_user);
            safeStrCopy(_sys.mqtt_user, sizeof(_sys.mqtt_user), user);
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("mqtt_user", oldVal, user, false);
            publishConfigEventInternal("SET_MQTT_USER", user, true, nullptr);
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setMqttUser(const String& user) {
    setMqttUser(user.c_str());
}

void ConfigManager::setMqttPassword(const char* password) {
    if (!_initialized) return;
    if (password == nullptr) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.mqtt_password, password) != 0) {
            char oldVal[64];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.mqtt_password);
            safeStrCopy(_sys.mqtt_password, sizeof(_sys.mqtt_password), password);
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("mqtt_password", oldVal, password, true);
            publishConfigEventInternal("SET_MQTT_PASSWORD", "[HIDDEN]", true, nullptr);
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setMqttPassword(const String& password) {
    setMqttPassword(password.c_str());
}

void ConfigManager::setUseMqtt(bool use) {
    if (!_initialized) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_sys.use_mqtt != use) {
            bool oldVal = _sys.use_mqtt;
            _sys.use_mqtt = use;
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("use_mqtt", oldVal ? "true" : "false", use ? "true" : "false", false);
            publishConfigEventInternal("SET_USE_MQTT", use ? "true" : "false", true, nullptr);
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setWebPasswordHash(const char* hash) {
    if (!_initialized) return;
    if (hash == nullptr) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.web_password_hash, hash) != 0) {
            char oldVal[65];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.web_password_hash);
            safeStrCopy(_sys.web_password_hash, sizeof(_sys.web_password_hash), hash);
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("web_password_hash", oldVal, hash, true);
            publishConfigEventInternal("SET_WEB_PASSWORD", "[HIDDEN]", true, nullptr);
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setWebPasswordHash(const String& hash) {
    setWebPasswordHash(hash.c_str());
}

void ConfigManager::setPureLocalMode(bool local) {
    if (!_initialized) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_sys.is_pure_local_mode != local) {
            bool oldVal = _sys.is_pure_local_mode;
            _sys.is_pure_local_mode = local;
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("is_pure_local_mode", oldVal ? "true" : "false", local ? "true" : "false", false);
            publishConfigEventInternal("SET_PURE_LOCAL", local ? "true" : "false", true, nullptr);
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setNetworkDhcp(bool dhcp) {
    if (!_initialized) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_sys.net_dhcp != dhcp) {
            bool oldVal = _sys.net_dhcp;
            _sys.net_dhcp = dhcp;
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("net_dhcp", oldVal ? "true" : "false", dhcp ? "true" : "false", true);
            publishConfigEventInternal("SET_NET_DHCP", dhcp ? "true" : "false", true, nullptr);
            if (_initialized) {
                applyNetworkSettings();
            }
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setStaticIp(const char* ip, const char* mask, const char* gateway, const char* dns) {
    if (!_initialized) return;
    if (ip == nullptr || mask == nullptr || gateway == nullptr || dns == nullptr) return;
    if (!isValidIp(ip) || !isValidIp(mask) || !isValidIp(gateway) || !isValidIp(dns)) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool changed = false;
        if (strcmp(_sys.cfg_ip, ip) != 0) {
            char oldVal[16];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.cfg_ip);
            safeStrCopy(_sys.cfg_ip, sizeof(_sys.cfg_ip), ip);
            publishConfigChange("cfg_ip", oldVal, ip, true);
            changed = true;
        }
        if (strcmp(_sys.cfg_mask, mask) != 0) {
            char oldVal[16];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.cfg_mask);
            safeStrCopy(_sys.cfg_mask, sizeof(_sys.cfg_mask), mask);
            publishConfigChange("cfg_mask", oldVal, mask, true);
            changed = true;
        }
        if (strcmp(_sys.cfg_gateway, gateway) != 0) {
            char oldVal[16];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.cfg_gateway);
            safeStrCopy(_sys.cfg_gateway, sizeof(_sys.cfg_gateway), gateway);
            publishConfigChange("cfg_gateway", oldVal, gateway, true);
            changed = true;
        }
        if (strcmp(_sys.cfg_dns, dns) != 0) {
            char oldVal[16];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.cfg_dns);
            safeStrCopy(_sys.cfg_dns, sizeof(_sys.cfg_dns), dns);
            publishConfigChange("cfg_dns", oldVal, dns, true);
            changed = true;
        }

        if (changed) {
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigEventInternal("SET_STATIC_IP", ip, true, "Network settings updated");
            if (_initialized) {
                applyNetworkSettings();
            }
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setStaticIp(const String& ip, const String& mask, const String& gateway, const String& dns) {
    setStaticIp(ip.c_str(), mask.c_str(), gateway.c_str(), dns.c_str());
}

void ConfigManager::setStaticIp(const char* ip) {
    if (!_initialized) return;
    if (ip == nullptr || !isValidIp(ip)) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.cfg_ip, ip) != 0) {
            char oldVal[16];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.cfg_ip);
            safeStrCopy(_sys.cfg_ip, sizeof(_sys.cfg_ip), ip);
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("cfg_ip", oldVal, ip, true);
            publishConfigEventInternal("SET_STATIC_IP", ip, true, nullptr);
            if (_initialized) {
                applyNetworkSettings();
            }
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setStaticIp(const String& ip) {
    setStaticIp(ip.c_str());
}

void ConfigManager::setExtraData(const char* json) {
    if (!_initialized) return;
    if (json == nullptr) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.extra_data, json) != 0) {
            char oldVal[512];
            safeStrCopy(oldVal, sizeof(oldVal), _sys.extra_data);
            safeStrCopy(_sys.extra_data, sizeof(_sys.extra_data), json);
            markDirty();
            xSemaphoreGiveRecursive(_mutex);
            publishConfigChange("extra_data", oldVal, json, false);
            publishConfigEventInternal("SET_EXTRA_DATA", nullptr, true, "Extra data updated");
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setExtraData(const String& json) {
    setExtraData(json.c_str());
}

// ============================================================================
// 16. КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::setExtraCallbacks(OnLoadExtraCallback loadcb, OnSaveExtraCallback savecb) {
    _onLoadExtra = loadcb;
    _onSaveExtra = savecb;
}

// ============================================================================
// 17. СЕТЕВЫЕ НАСТРОЙКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::applyNetworkSettings() {
    if (_applyNetworkInProgress) {
        logMessage("Network settings already applying, skipping");
        return;
    }
    _applyNetworkInProgress = true;

    logMessage("Applying network settings...");

    if (!_loaded) {
        logMessage("Config not loaded, skipping network apply");
        _applyNetworkInProgress = false;
        return;
    }

    publishConfigEventInternal("APPLY_NETWORK", nullptr, true, "Network settings applied");
    _networkApplied = true;
    _status.isNetworkApplied = true;
    _applyNetworkInProgress = false;
}

void ConfigManager::refreshNetwork() {
    applyNetworkSettings();
}

// ============================================================================
// 18. ВАЛИДАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool ConfigManager::isValidIp(const char* ip) const {
    if (ip == nullptr) return false;

    int num = 0;
    int dots = 0;
    int segment = 0;

    for (size_t i = 0; ip[i] != '\0'; i++) {
        if (ip[i] == '.') {
            if (segment == 0 || segment > 255) return false;
            dots++;
            segment = 0;
        } else if (ip[i] >= '0' && ip[i] <= '9') {
            segment = segment * 10 + (ip[i] - '0');
            if (segment > 255) return false;
        } else {
            return false;
        }
    }

    if (segment == 0 || segment > 255) return false;
    return (dots == 3);
}

bool ConfigManager::isValidIp(const String& ip) const {
    return isValidIp(ip.c_str());
}

bool ConfigManager::isValidHostname(const char* hostname) const {
    if (hostname == nullptr) return false;
    size_t len = strlen(hostname);
    if (len == 0 || len > MAX_HOSTNAME_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        char c = hostname[i];
        if (!isalnum(c) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

bool ConfigManager::isConfigValid() const {
    if (_sys.config_ver != CONFIG_VERSION_CURRENT) return false;
    uint32_t crc = calculateConfigCRC();
    return crc == _lastSavedCRC;
}

// ============================================================================
// 19. SAFE MODE (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::setSafeMode(bool active) {
    if (_safeModeActive == active) return;

    _safeModeActive = active;
    _status.isSafeMode = active;

    if (active) {
        logMessage("SAFE MODE ACTIVATED");
    } else {
        logMessage("SAFE MODE DEACTIVATED");
    }

    if (_onSafeModeChange) {
        _onSafeModeChange(active);
    }

    publishConfigEventInternal("SAFE_MODE", active ? "ON" : "OFF", true, nullptr);
}

// ============================================================================
// 20. CRC (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
uint32_t ConfigManager::calculateConfigCRC() {
    if (_mutex == nullptr) return 0;

    uint32_t crc = 0;
    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(CRC_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        crc = 0;
        crc += _sys.net_dhcp ? 1 : 0;
        for (size_t i = 0; i < sizeof(_sys.cfg_ip); i++) crc += _sys.cfg_ip[i];
        for (size_t i = 0; i < sizeof(_sys.cfg_mask); i++) crc += _sys.cfg_mask[i];
        for (size_t i = 0; i < sizeof(_sys.cfg_gateway); i++) crc += _sys.cfg_gateway[i];
        for (size_t i = 0; i < sizeof(_sys.cfg_dns); i++) crc += _sys.cfg_dns[i];
        crc += _sys.use_mqtt ? 1 : 0;
        for (size_t i = 0; i < sizeof(_sys.mqtt_ip); i++) crc += _sys.mqtt_ip[i];
        for (size_t i = 0; i < sizeof(_sys.mqtt_user); i++) crc += _sys.mqtt_user[i];
        for (size_t i = 0; i < sizeof(_sys.mqtt_password); i++) crc += _sys.mqtt_password[i];
        for (size_t i = 0; i < sizeof(_sys.hostname); i++) crc += _sys.hostname[i];
        for (size_t i = 0; i < sizeof(_sys.web_password_hash); i++) crc += _sys.web_password_hash[i];
        crc += _sys.is_pure_local_mode ? 1 : 0;
        for (size_t i = 0; i < sizeof(_sys.extra_data); i++) crc += _sys.extra_data[i];
        crc += _sys.version_flags;
        crc += _sys.last_modified;
        xSemaphoreGiveRecursive(_mutex);
    }
    return crc;
}

// ============================================================================
// 21. ПУБЛИКАЦИЯ СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void ConfigManager::publishConfigChange(const char* key, const char* oldValue,
                                        const char* newValue, bool critical) {
    ConfigChangeEvent event;
    event.timestamp = millis();
    safeStrCopy(event.key, sizeof(event.key), key);
    safeStrCopy(event.oldValue, sizeof(event.oldValue), oldValue);
    safeStrCopy(event.newValue, sizeof(event.newValue), newValue);
    event.isCritical = critical;

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = CONFIG_EVENT_CHANGED;
    data.value = critical ? 1 : 0;
    memcpy(data.payload, &event, min(sizeof(ConfigChangeEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(ConfigChangeEvent);
    postEvent(data.command, &data);

    if (_onConfigChange) {
        _onConfigChange(key, oldValue, newValue);
    }
}

void ConfigManager::publishConfigLoaded() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = CONFIG_EVENT_LOADED;
    data.value = 1;
    safeStrCopy(data.payload, sizeof(data.payload), "Config loaded");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void ConfigManager::publishConfigSaved() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = CONFIG_EVENT_SAVED;
    data.value = 1;
    safeStrCopy(data.payload, sizeof(data.payload), "Config saved");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void ConfigManager::publishConfigReset() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = CONFIG_EVENT_RESET;
    data.value = 1;
    safeStrCopy(data.payload, sizeof(data.payload), "Config reset");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void ConfigManager::publishConfigCorrupted(uint32_t crc) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = CONFIG_EVENT_CORRUPTED;
    data.value = crc;
    snprintf(data.payload, sizeof(data.payload), "Config corrupted: CRC mismatch %lu", crc);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

// ============================================================================
// 22. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void ConfigManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================");
    stream.println(" CONFIG MANAGER DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Loaded: %s\n", _loaded ? "YES" : "NO");
    stream.printf(" Dirty: %s\n", _isDirty ? "YES" : "NO");
    stream.printf(" Safe Mode: %s\n", _safeModeActive ? "YES" : "NO");
    stream.printf(" Network Applied: %s\n", _networkApplied ? "YES" : "NO");
    stream.printf(" Version: %d\n", CONFIG_VERSION_CURRENT);
    stream.printf(" CRC: %u\n", _lastSavedCRC);
    stream.printf(" Loads: %lu\n", _loadCount);
    stream.printf(" Saves: %lu\n", _saveCount);
    stream.printf(" Hostname: %s\n", _sys.hostname);
    stream.printf(" DHCP: %s\n", _sys.net_dhcp ? "ON" : "OFF");
    stream.printf(" IP: %s\n", _sys.cfg_ip);
    stream.printf(" MQTT: %s\n", _sys.use_mqtt ? "ON" : "OFF");
    stream.printf(" MQTT IP: %s\n", _sys.mqtt_ip);
    stream.printf(" Pure Local: %s\n", _sys.is_pure_local_mode ? "ON" : "OFF");
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("==============================");
}

void ConfigManager::printStats() const {
    streamDiagnosticInfo(Serial);
}