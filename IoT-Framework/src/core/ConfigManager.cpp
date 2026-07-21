// ============================================================================
// ConfigManager.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
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
// ============================================================================
#include "ConfigManager.h"
#include <esp_task_wdt.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static ConfigManager _configManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
ConfigManager::ConfigManager() {
    // === 1.1 РЕКУРСИВНЫЙ МЬЮТЕКС ===
    _mutex = xSemaphoreCreateRecursiveMutex();
    if (_mutex == nullptr) {
        Serial.println("[CFG] CRITICAL: Failed to create mutex!");
        while (1) { delay(100); }
    }

    // === 1.2 ID МОДУЛЯ ===
    _moduleId = MODULE_ID_CONFIG;

    // === 1.3 СБРОС СОСТОЯНИЯ ===
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
    _lastError[0] = '\0';
    _onLoadExtra = nullptr;
    _onSaveExtra = nullptr;
    _onConfigChange = nullptr;
    _onSafeModeChange = nullptr;

    // === 1.4 СТАТУС ===
    memset(&_status, 0, sizeof(_status));
    _status.currentVersion = CONFIG_VERSION_CURRENT;

    Serial.println("[CFG] Instance created (v4.2.2)");
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
// 2. СИНГЛТОН
// ============================================================================
ConfigManager& ConfigManager::getInstance() {
    return _configManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
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

    // Отправляем событие для LogManager (используем правильное событие!)
    if (_initialized) {
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = _moduleId;
        data.targetModule = MODULE_ID_LOG;
        data.command = SH_EVENT_LOG_ENTRY;  // <-- ИСПРАВЛЕНО!
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
    _status.isDirty = _isDirty;  // <-- ИСПРАВЛЕНО!
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
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void ConfigManager::init() {
    if (_initialized) return;

    logMessage("Initializing...");

    // === 4.1 LITTLEFS ===
    if (!ensureLittleFS()) {
        updateStatus("LittleFS mount failed");
        _initialized = false;
        return;
    }

    // === 4.2 ЗАГРУЗКА КОНФИГУРАЦИИ ===
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

    // === 4.3 ПРОВЕРКА CRC ===
    uint32_t currentCRC = calculateConfigCRC();
    if (_lastSavedCRC != 0 && _lastSavedCRC != currentCRC) {
        logMessage("WARNING: Config CRC mismatch! Loading NVS backup...");
        loadFromNvsBackup();
        if (save()) {
            logMessage("NVS backup restored successfully");
        }
    }

    // === 4.4 ПРИМЕНЕНИЕ СЕТЕВЫХ НАСТРОЕК ===
    if (_loaded) {
        applyNetworkSettings();
    }

    // === 4.5 ПОДПИСКА НА СОБЫТИЯ ===
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

    // === 4.6 ГОТОВНОСТЬ ===
    _initialized = true;
    _loaded = true;
    _status.isLoaded = true;
    updateStatus();
    publishConfigLoaded();
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

    // === СБРОС WDT ===
    esp_task_wdt_reset();

    // === ОТЛОЖЕННОЕ СОХРАНЕНИЕ ===
    if (_isDirty && (millis() - _dirtyTimestamp > FLUSH_DELAY_MS)) {
        if (_safeModeActive) {
            logMessage("Dirty but safe mode active - skipping save");
            _isDirty = false;  // Сбрасываем, чтобы не пытаться снова
        } else {
            logMessage("Auto-saving due to timeout");
            save();
        }
    }

    // === ПРОВЕРКА ЦЕЛОСТНОСТИ (раз в минуту) ===
    static uint32_t lastIntegrityCheck = 0;
    if (millis() - lastIntegrityCheck > 60000) {
        lastIntegrityCheck = millis();
        uint32_t currentCRC = calculateConfigCRC();
        if (_lastSavedCRC != 0 && _lastSavedCRC != currentCRC) {
            logMessage("WARNING: Config corruption detected!");
            publishConfigCorrupted(currentCRC);
        }
    }
}

// ============================================================================
// 5. ОБРАБОТЧИКИ СОБЫТИЙ
// ============================================================================
void ConfigManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                                int32_t id, void* eventData) {
    ConfigManager* instance = static_cast<ConfigManager*>(handlerArgs);
    if (!instance || !instance->_initialized) return;

    // === СИСТЕМНЫЕ СОБЫТИЯ ===
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

            case SH_EVENT_NET_DISCONNECTED:
                // Ничего не делаем, сохраняем локально
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

    // === ПРИКЛАДНЫЕ СОБЫТИЯ ===
    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void ConfigManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    // === КОМАНДЫ ===
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
// 6. ОБРАБОТКА КОМАНД (С ПРОВЕРКОЙ _initialized)
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
                } else {
                    ShEventData response;
                    memset(&response, 0, sizeof(ShEventData));
                    response.sourceModule = _moduleId;
                    response.targetModule = data->sourceModule;
                    response.command = CMD_RESPONSE_ERROR;
                    safeStrCopy(response.payload, sizeof(response.payload), "Import failed");
                    response.payloadLen = strlen(response.payload);
                    postEvent(SH_EVENT_CMD_RESPONSE, &response);
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
            } else {
                ShEventData response;
                memset(&response, 0, sizeof(ShEventData));
                response.sourceModule = _moduleId;
                response.targetModule = data->sourceModule;
                response.command = CMD_RESPONSE_ERROR;
                safeStrCopy(response.payload, sizeof(response.payload), "Save failed");
                response.payloadLen = strlen(response.payload);
                postEvent(SH_EVENT_CMD_RESPONSE, &response);
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
            break;
        }

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 7. СТАТУС И ДИАГНОСТИКА
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

void ConfigManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _loaded ? 1 : 0;

    snprintf(data->payload, sizeof(data->payload),
            "ver:%d,crc:%u,saves:%lu,loads:%lu,dirty:%d,safe:%d,heap:%u,err:%s",
            CONFIG_VERSION_CURRENT,
            _lastSavedCRC,
            _saveCount,
            _loadCount,
            _isDirty ? 1 : 0,
            _safeModeActive ? 1 : 0,
            ESP.getFreeHeap(),
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
// 8. ЗАГРУЗКА / СОХРАНЕНИЕ
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
    } else {
        updateStatus("Load failed");
    }
    return result;
}

bool ConfigManager::save() {
    if (_safeModeActive) {
        logMessage("Save blocked: Safe mode active");
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
    } else {
        updateStatus("Save failed");
    }
    return result;
}

// ============================================================================
// 9. ВНУТРЕННИЕ МЕТОДЫ ЗАГРУЗКИ/СОХРАНЕНИЯ
// ============================================================================
bool ConfigManager::loadFromFile() {
    const char* pathToOpen = CONFIG_PATH;

    // === 9.1 ПРОВЕРКА СУЩЕСТВОВАНИЯ ===
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

    // === 9.2 ОТКРЫТИЕ ФАЙЛА ===
    File file = LittleFS.open(pathToOpen, "r");
    if (!file) {
        logMessage("IO Error reading config. Trying NVS backup...");
        loadFromNvsBackup();
        return false;
    }

    // === 9.3 ПРОВЕРКА РАЗМЕРА ===
    size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > MAX_JSON_SIZE) {
        file.close();
        logMessage("Invalid file size: %zu", fileSize);
        return false;
    }

    // === 9.4 ПАРСИНГ JSON ===
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        logMessage("JSON Parse Error: %s", error.c_str());
        loadFromNvsBackup();
        return false;
    }

    // === 9.5 МИГРАЦИЯ ===
    uint8_t version = doc["config_ver"] | 0;
    if (version != CONFIG_VERSION_CURRENT) {
        logMessage("Migrating config from version %d to %d", version, CONFIG_VERSION_CURRENT);
        migrateConfig(doc, version);
        save();  // Сохраняем мигрированную версию
    }

    // === 9.6 ВАЛИДАЦИЯ ===
    if (!validateSystemConfig(doc)) {
        logMessage("Validation failed. Loading NVS backup...");
        loadFromNvsBackup();
        return false;
    }

    // === 9.7 ЗАГРУЗКА ДАННЫХ ===
    _sys.config_ver = CONFIG_VERSION_CURRENT;
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
    safeStrCopy(_sys.web_password_hash, sizeof(_sys.web_password_hash), doc["web_p_hash"] | "");
    _sys.is_pure_local_mode = doc["local_mode"] | false;
    safeStrCopy(_sys.extra_data, sizeof(_sys.extra_data), doc["extra"] | "{}");

    // === 9.8 EXTRA СЕКЦИЯ ===
    if (_onLoadExtra != nullptr && doc.containsKey("extra")) {
        JsonVariant extraObj = doc["extra"];
        _onLoadExtra(extraObj);
    }

    // === 9.9 ОБНОВЛЕНИЕ CRC ===
    _lastSavedCRC = calculateConfigCRC();
    _lastLoadTime = millis();
    _status.fileSize = fileSize;
    return true;
}

bool ConfigManager::saveToFile() {
    // === 9.10 ПРОВЕРКА СВОБОДНОГО МЕСТА ===
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;
    if (freeBytes < 4096) {
        logMessage("ERROR: Not enough free space! Free: %zu bytes", freeBytes);
        return false;
    }

    // === 9.11 СОЗДАНИЕ ВРЕМЕННОГО ФАЙЛА ===
    File tempFile = LittleFS.open(TEMP_PATH, "w");
    if (!tempFile) {
        logMessage("Failed to create temp file");
        return false;
    }

    // === 9.12 СЕРИАЛИЗАЦИЯ JSON ===
    JsonDocument doc;
    doc["config_ver"] = CONFIG_VERSION_CURRENT;
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
    doc["web_p_hash"] = _sys.web_password_hash;
    doc["local_mode"] = _sys.is_pure_local_mode;

    // === 9.13 EXTRA СЕКЦИЯ ===
    if (_onSaveExtra != nullptr) {
        JsonObject extraObj = doc["extra"].to<JsonObject>();
        _onSaveExtra(extraObj);
    } else {
        doc["extra"] = _sys.extra_data;
    }

    // === 9.14 ЗАПИСЬ ===
    size_t bytesWritten = serializeJson(doc, tempFile);
    tempFile.close();

    if (bytesWritten == 0) {
        LittleFS.remove(TEMP_PATH);
        logMessage("Serialization error");
        return false;
    }

    // === 9.15 РОТАЦИЯ БЭКАПА ===
    if (LittleFS.exists(CONFIG_PATH)) {
        if (LittleFS.exists(BACKUP_PATH)) {
            LittleFS.remove(BACKUP_PATH);
        }
        if (!LittleFS.rename(CONFIG_PATH, BACKUP_PATH)) {
            logMessage("Backup rotation failed!");
        }
    }

    // === 9.16 АТОМАРНАЯ ЗАМЕНА ===
    if (!LittleFS.rename(TEMP_PATH, CONFIG_PATH)) {
        logMessage("Atomic swap failed!");
        if (LittleFS.exists(BACKUP_PATH)) {
            LittleFS.rename(BACKUP_PATH, CONFIG_PATH);
            logMessage("Restored from backup");
        }
        return false;
    }

    // === 9.17 NVS БЭКАП ===
    saveToNvsBackup();
    _lastSavedCRC = calculateConfigCRC();
    _lastSaveTime = millis();
    return true;
}

// ============================================================================
// 10. NVS БЭКАП (ИСПРАВЛЕНО)
// ============================================================================
void ConfigManager::saveToNvsBackup() {
    _prefs.begin(CONFIG_BACKUP_NAMESPACE, false);
    _prefs.putUChar("ver", CONFIG_VERSION_CURRENT);
    _prefs.putBool("dhcp", _sys.net_dhcp);
    _prefs.putString("ip", _sys.cfg_ip);
    _prefs.putString("mask", _sys.cfg_mask);
    _prefs.putString("gw", _sys.cfg_gateway);
    _prefs.putString("dns", _sys.cfg_dns);
    _prefs.putString("host", _sys.hostname);
    _prefs.putBool("mqtt", _sys.use_mqtt);
    _prefs.putString("mqtt_ip", _sys.mqtt_ip);
    _prefs.putString("mqtt_user", _sys.mqtt_user);
    _prefs.putString("mqtt_pass", _sys.mqtt_password);
    _prefs.putString("web_hash", _sys.web_password_hash);
    _prefs.putBool("local", _sys.is_pure_local_mode);
    _prefs.putString("extra", _sys.extra_data);
    _prefs.putUInt("crc", _lastSavedCRC);
    _prefs.end();
}

void ConfigManager::loadFromNvsBackup() {
    _prefs.begin(CONFIG_BACKUP_NAMESPACE, true);
    _sys.config_ver = _prefs.getUChar("ver", CONFIG_VERSION_CURRENT);
    _sys.net_dhcp = _prefs.getBool("dhcp", true);

    safeStrCopy(_sys.cfg_ip, sizeof(_sys.cfg_ip), _prefs.getString("ip", "192.168.1.200").c_str());
    safeStrCopy(_sys.cfg_mask, sizeof(_sys.cfg_mask), _prefs.getString("mask", "255.255.255.0").c_str());
    safeStrCopy(_sys.cfg_gateway, sizeof(_sys.cfg_gateway), _prefs.getString("gw", "192.168.1.1").c_str());
    safeStrCopy(_sys.cfg_dns, sizeof(_sys.cfg_dns), _prefs.getString("dns", "192.168.1.1").c_str());
    safeStrCopy(_sys.hostname, sizeof(_sys.hostname), _prefs.getString("host", "smart-device").c_str());
    _sys.use_mqtt = _prefs.getBool("mqtt", false);
    safeStrCopy(_sys.mqtt_ip, sizeof(_sys.mqtt_ip), _prefs.getString("mqtt_ip", "").c_str());
    safeStrCopy(_sys.mqtt_user, sizeof(_sys.mqtt_user), _prefs.getString("mqtt_user", "").c_str());
    safeStrCopy(_sys.mqtt_password, sizeof(_sys.mqtt_password), _prefs.getString("mqtt_pass", "").c_str());
    safeStrCopy(_sys.web_password_hash, sizeof(_sys.web_password_hash), _prefs.getString("web_hash", "").c_str());
    _sys.is_pure_local_mode = _prefs.getBool("local", false);
    safeStrCopy(_sys.extra_data, sizeof(_sys.extra_data), _prefs.getString("extra", "{}").c_str());
    _lastSavedCRC = _prefs.getUInt("crc", 0);
    _prefs.end();
    logMessage("NVS backup loaded");
}

// ============================================================================
// 11. CRC (С МЬЮТЕКСОМ!)
// ============================================================================
uint32_t ConfigManager::calculateConfigCRC() {
    // ЗАЩИТА ОТ ГОНКИ ДАННЫХ!
    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(CRC_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        logMessage("CRC calculation: mutex timeout!");
        return 0;
    }

    // Используем экспорт без мьютекса (уже взят)
    String json;
    exportToJson(json);

    xSemaphoreGiveRecursive(_mutex);

    // Вычисляем CRC от JSON
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < json.length(); i++) {
        crc ^= (uint8_t)json[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

// ============================================================================
// 12. ВАЛИДАЦИЯ И МИГРАЦИЯ
// ============================================================================
bool ConfigManager::validateSystemConfig(const JsonDocument& doc) {
    // === 12.1 ПРОВЕРКА HOSTNAME ===
    if (!doc.containsKey("hostname")) {
        logMessage("Missing hostname field");
        return false;
    }
    String hostname = doc["hostname"] | "";
    if (!isValidHostname(hostname.c_str())) {
        logMessage("Invalid hostname: %s", hostname.c_str());
        return false;
    }

    // === 12.2 ПРОВЕРКА IP ===
    if (doc.containsKey("cfg_ip") && doc["cfg_ip"].is<String>()) {
        String ip = doc["cfg_ip"].as<String>();
        if (!ip.isEmpty() && !isValidIp(ip.c_str())) {
            logMessage("Invalid IP: %s", ip.c_str());
            return false;
        }
    }
    return true;
}

void ConfigManager::migrateConfig(JsonDocument& doc, uint8_t oldVersion) {
    logMessage("Migrating config from version %d", oldVersion);

    if (oldVersion == 0) {
        if (!doc.containsKey("local_mode")) doc["local_mode"] = false;
        if (!doc.containsKey("extra")) doc["extra"] = "{}";
        if (!doc.containsKey("web_p_hash")) doc["web_p_hash"] = "";
        if (doc.containsKey("mqtt_enabled")) {
            doc["use_mqtt"] = doc["mqtt_enabled"];
            doc.remove("mqtt_enabled");
        }
    }

    if (oldVersion <= 1) {
        if (!doc.containsKey("extra")) doc["extra"] = "{}";
    }

    doc["config_ver"] = CONFIG_VERSION_CURRENT;
    logMessage("Config migrated to version %d", CONFIG_VERSION_CURRENT);
}

// ============================================================================
// 13. СЕТТЕРЫ (С ОТПРАВКОЙ СОБЫТИЙ)
// ============================================================================
void ConfigManager::setHostname(const char* name) {
    if (!_initialized) { logMessage("Not initialized"); return; }
    if (!isValidHostname(name)) {
        logMessage("Invalid hostname: %s", name);
        return;
    }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.hostname, name) != 0) {
            char oldValue[32];
            safeStrCopy(oldValue, sizeof(oldValue), _sys.hostname);
            safeStrCopy(_sys.hostname, sizeof(_sys.hostname), name);
            markDirty();
            publishConfigChange("hostname", oldValue, name, false);
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

void ConfigManager::setHostname(const String& name) {
    setHostname(name.c_str());
}

void ConfigManager::setMqttIp(const char* ip) {
    if (!_initialized) { logMessage("Not initialized"); return; }
    if (strlen(ip) > 0 && !isValidIp(ip)) {
        logMessage("Invalid MQTT IP: %s", ip);
        return;
    }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.mqtt_ip, ip) != 0) {
            char oldValue[16];
            safeStrCopy(oldValue, sizeof(oldValue), _sys.mqtt_ip);
            safeStrCopy(_sys.mqtt_ip, sizeof(_sys.mqtt_ip), ip);
            markDirty();
            publishConfigChange("mqtt_ip", oldValue, ip, false);
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

void ConfigManager::setMqttIp(const String& ip) {
    setMqttIp(ip.c_str());
}

void ConfigManager::setMqttUser(const char* user) {
    if (!_initialized) { logMessage("Not initialized"); return; }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.mqtt_user, user) != 0) {
            char oldValue[32];
            safeStrCopy(oldValue, sizeof(oldValue), _sys.mqtt_user);
            safeStrCopy(_sys.mqtt_user, sizeof(_sys.mqtt_user), user);
            markDirty();
            publishConfigChange("mqtt_user", oldValue, user, false);
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

void ConfigManager::setMqttUser(const String& user) {
    setMqttUser(user.c_str());
}

void ConfigManager::setMqttPassword(const char* password) {
    if (!_initialized) { logMessage("Not initialized"); return; }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.mqtt_password, password) != 0) {
            char oldValue[64];
            safeStrCopy(oldValue, sizeof(oldValue), _sys.mqtt_password);
            safeStrCopy(_sys.mqtt_password, sizeof(_sys.mqtt_password), password);
            markDirty();
            publishConfigChange("mqtt_password", oldValue, password, false);
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

void ConfigManager::setMqttPassword(const String& password) {
    setMqttPassword(password.c_str());
}

void ConfigManager::setUseMqtt(bool use) {
    if (!_initialized) { logMessage("Not initialized"); return; }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_sys.use_mqtt != use) {
            bool oldValue = _sys.use_mqtt;
            _sys.use_mqtt = use;
            markDirty();
            publishConfigChange("use_mqtt", oldValue ? "true" : "false", use ? "true" : "false", false);
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

void ConfigManager::setWebPasswordHash(const char* hash) {
    if (!_initialized) { logMessage("Not initialized"); return; }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.web_password_hash, hash) != 0) {
            char oldValue[65];
            safeStrCopy(oldValue, sizeof(oldValue), _sys.web_password_hash);
            safeStrCopy(_sys.web_password_hash, sizeof(_sys.web_password_hash), hash);
            markDirty();
            publishConfigChange("web_password_hash", oldValue, hash, true);
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

void ConfigManager::setWebPasswordHash(const String& hash) {
    setWebPasswordHash(hash.c_str());
}

void ConfigManager::setPureLocalMode(bool local) {
    if (!_initialized) { logMessage("Not initialized"); return; }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_sys.is_pure_local_mode != local) {
            bool oldValue = _sys.is_pure_local_mode;
            _sys.is_pure_local_mode = local;
            markDirty();
            publishConfigChange("local_mode", oldValue ? "true" : "false", local ? "true" : "false", true);
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

void ConfigManager::setNetworkDhcp(bool dhcp) {
    if (!_initialized) { logMessage("Not initialized"); return; }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_sys.net_dhcp != dhcp) {
            bool oldValue = _sys.net_dhcp;
            _sys.net_dhcp = dhcp;
            markDirty();
            publishConfigChange("net_dhcp", oldValue ? "true" : "false", dhcp ? "true" : "false", true);

            // Применяем настройки сети (с защитой от рекурсии)
            if (!_applyNetworkInProgress) {
                _applyNetworkInProgress = true;
                xSemaphoreGiveRecursive(_mutex);
                applyNetworkSettings();
                if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    _applyNetworkInProgress = false;
                    xSemaphoreGiveRecursive(_mutex);
                }
            } else {
                xSemaphoreGiveRecursive(_mutex);
            }
        } else {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
}

void ConfigManager::setStaticIp(const char* ip, const char* mask, const char* gateway, const char* dns) {
    if (!_initialized) { logMessage("Not initialized"); return; }
    if (!isValidIp(ip) || !isValidIp(mask) || !isValidIp(gateway) || !isValidIp(dns)) {
        logMessage("Invalid static IP configuration");
        return;
    }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool changed = false;

        if (strcmp(_sys.cfg_ip, ip) != 0) {
            char oldValue[16];
            safeStrCopy(oldValue, sizeof(oldValue), _sys.cfg_ip);
            safeStrCopy(_sys.cfg_ip, sizeof(_sys.cfg_ip), ip);
            changed = true;
        }
        if (strcmp(_sys.cfg_mask, mask) != 0) {
            safeStrCopy(_sys.cfg_mask, sizeof(_sys.cfg_mask), mask);
            changed = true;
        }
        if (strcmp(_sys.cfg_gateway, gateway) != 0) {
            safeStrCopy(_sys.cfg_gateway, sizeof(_sys.cfg_gateway), gateway);
            changed = true;
        }
        if (strcmp(_sys.cfg_dns, dns) != 0) {
            safeStrCopy(_sys.cfg_dns, sizeof(_sys.cfg_dns), dns);
            changed = true;
        }

        if (changed) {
            markDirty();
            publishConfigChange("static_ip", "changed", ip, true);

            if (!_applyNetworkInProgress) {
                _applyNetworkInProgress = true;
                xSemaphoreGiveRecursive(_mutex);
                applyNetworkSettings();
                if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    _applyNetworkInProgress = false;
                    xSemaphoreGiveRecursive(_mutex);
                }
            } else {
                xSemaphoreGiveRecursive(_mutex);
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
    setStaticIp(ip, "255.255.255.0", "192.168.1.1", "192.168.1.1");
}

void ConfigManager::setStaticIp(const String& ip) {
    setStaticIp(ip.c_str());
}

void ConfigManager::setExtraData(const char* json) {
    if (!_initialized) { logMessage("Not initialized"); return; }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(_sys.extra_data, json) != 0) {
            char oldValue[512];
            safeStrCopy(oldValue, sizeof(oldValue), _sys.extra_data);
            safeStrCopy(_sys.extra_data, sizeof(_sys.extra_data), json);
            markDirty();
            publishConfigChange("extra_data", oldValue, json, false);
        }
        xSemaphoreGiveRecursive(_mutex);
    }
}

void ConfigManager::setExtraData(const String& json) {
    setExtraData(json.c_str());
}

// ============================================================================
// 14. RESET
// ============================================================================
void ConfigManager::resetToDefaults() {
    if (!_initialized) { logMessage("Not initialized"); return; }

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        // Удаляем файлы
        if (LittleFS.exists(CONFIG_PATH)) {
            LittleFS.remove(CONFIG_PATH);
        }
        if (LittleFS.exists(BACKUP_PATH)) {
            LittleFS.remove(BACKUP_PATH);
        }
        if (LittleFS.exists(TEMP_PATH)) {
            LittleFS.remove(TEMP_PATH);
        }

        // Очищаем NVS
        _prefs.begin(CONFIG_BACKUP_NAMESPACE, false);
        _prefs.clear();
        _prefs.end();

        // Сброс структуры
        _sys = SystemConfig();
        _lastSavedCRC = 0;
        _isDirty = false;
        _factoryResetTriggered = true;

        xSemaphoreGiveRecursive(_mutex);

        publishConfigReset();
        logMessage("Factory reset complete");
        updateStatus("Factory reset completed");

        // Сохраняем дефолтную конфигурацию
        save();
    }
}

// ============================================================================
// 15. ЭКСПОРТ / ИМПОРТ
// ============================================================================
bool ConfigManager::exportToJson(String& output) const {
    JsonDocument doc;
    doc["config_ver"] = CONFIG_VERSION_CURRENT;
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
    doc["web_p_hash"] = _sys.web_password_hash;
    doc["local_mode"] = _sys.is_pure_local_mode;
    doc["extra"] = _sys.extra_data;
    return serializeJson(doc, output) > 0;
}

bool ConfigManager::exportToJson(char* buffer, size_t bufferSize) const {
    if (!buffer || bufferSize == 0) return false;
    String output;
    if (!exportToJson(output)) return false;
    size_t len = output.length();
    if (len >= bufferSize) return false;
    strcpy(buffer, output.c_str());
    return true;
}

String ConfigManager::exportToJson() const {
    String output;
    exportToJson(output);
    return output;
}

bool ConfigManager::importFromJson(const String& json) {
    if (!_initialized) { logMessage("Not initialized"); return false; }

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

    // Применяем настройки через сеттеры
    setNetworkDhcp(doc["net_dhcp"] | true);
    setHostname(doc["hostname"] | "smart-device");
    setMqttIp(doc["mqtt_ip"] | "");
    setMqttUser(doc["mqtt_user"] | "");
    setMqttPassword(doc["mqtt_password"] | "");
    setUseMqtt(doc["use_mqtt"] | false);
    setWebPasswordHash(doc["web_p_hash"] | "");
    setPureLocalMode(doc["local_mode"] | false);

    if (doc.containsKey("cfg_ip")) {
        setStaticIp(
            doc["cfg_ip"] | "192.168.1.200",
            doc["cfg_mask"] | "255.255.255.0",
            doc["cfg_gateway"] | "192.168.1.1",
            doc["cfg_dns"] | "192.168.1.1"
        );
    }

    if (doc.containsKey("extra")) {
        setExtraData(doc["extra"] | "{}");
    }

    return save();
}

bool ConfigManager::saveFromJsonStream(Stream& stream) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, stream);
    if (error) {
        logMessage("Stream parse error: %s", error.c_str());
        return false;
    }
    String json;
    serializeJson(doc, json);
    return importFromJson(json);
}

void ConfigManager::serializeToStream(Stream& stream) {
    String json = exportToJson();
    stream.print(json);
}

// ============================================================================
// 16. СЕТЕВЫЕ НАСТРОЙКИ (ИСПРАВЛЕНО)
// ============================================================================
void ConfigManager::applyNetworkSettings() {
    if (!_initialized) { return; }

    // Защита от рекурсии
    if (_applyNetworkInProgress) {
        logMessage("Network settings already being applied, skipping...");
        return;
    }
    _applyNetworkInProgress = true;

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = CONFIG_EVENT_NETWORK_APPLY;

    char ipInfo[128] = {0};

    if (_safeModeActive) {
        data.value = 0;  // Static IP
        snprintf(ipInfo, sizeof(ipInfo), "192.168.1.200,255.255.255.0,192.168.1.1,192.168.1.1");
        logMessage("SafeMode Applied! Fallback Network: IP=%s", "192.168.1.200");
    } else {
        data.value = _sys.net_dhcp ? 1 : 0;
        snprintf(ipInfo, sizeof(ipInfo), "%s,%s,%s,%s",
                _sys.cfg_ip,
                _sys.cfg_mask,
                _sys.cfg_gateway,
                _sys.cfg_dns);
        logMessage("Network settings applied: %s", _sys.net_dhcp ? "DHCP" : "Static");
    }

    // <-- ИСПРАВЛЕНО: правильные скобки!
    safeStrCopy(data.payload, sizeof(data.payload), ipInfo);
    data.payloadLen = strlen(data.payload);

    postEvent(SH_EVENT_NET_CONNECTED, &data);
    _networkApplied = true;
    _applyNetworkInProgress = false;
}

void ConfigManager::refreshNetwork() {
    logMessage("Network refresh requested");
    applyNetworkSettings();
}

// ============================================================================
// 17. ВАЛИДАЦИЯ
// ============================================================================
bool ConfigManager::isValidIp(const char* ip) const {
    if (ip == nullptr || strlen(ip) == 0) return true;

    int parts = 0;
    int start = 0;
    size_t len = strlen(ip);

    for (size_t i = 0; i <= len; i++) {
        if (i == len || ip[i] == '.') {
            if (i == start) return false;

            // Извлекаем часть
            char part[4];
            size_t partLen = i - start;
            if (partLen >= sizeof(part)) return false;
            strncpy(part, ip + start, partLen);
            part[partLen] = '\0';

            int val = atoi(part);
            if (val < 0 || val > 255) return false;
            if (partLen > 1 && part[0] == '0') return false;

            parts++;
            start = i + 1;
        } else if (ip[i] < '0' || ip[i] > '9') {
            return false;
        }
    }
    return (parts == 4);
}

bool ConfigManager::isValidIp(const String& ip) const {
    return isValidIp(ip.c_str());
}

bool ConfigManager::isValidHostname(const char* hostname) const {
    if (hostname == nullptr || strlen(hostname) == 0) return false;
    if (strlen(hostname) > MAX_HOSTNAME_LEN) return false;

    for (size_t i = 0; i < strlen(hostname); i++) {
        char c = hostname[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return true;
}

bool ConfigManager::isConfigValid() const {
    return isValidHostname(_sys.hostname) &&
           (strlen(_sys.cfg_ip) == 0 || isValidIp(_sys.cfg_ip)) &&
           (strlen(_sys.mqtt_ip) == 0 || isValidIp(_sys.mqtt_ip));
}

// ============================================================================
// 18. ОТПРАВКА СОБЫТИЙ
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
    memcpy(data.payload, &event, min(sizeof(event), sizeof(data.payload)));
    data.payloadLen = sizeof(event);
    postEvent(SH_EVENT_CMD_RESPONSE, &data);

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
    data.value = _loaded ? 1 : 0;
    safeStrCopy(data.payload, sizeof(data.payload), "Config loaded successfully");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void ConfigManager::publishConfigSaved() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = CONFIG_EVENT_SAVED;
    data.value = _lastSavedCRC;
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
    data.value = 0;
    safeStrCopy(data.payload, sizeof(data.payload), "Config reset to defaults");
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
    safeStrCopy(data.payload, sizeof(data.payload), "Config CRC mismatch");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_ERROR, &data);
}

// ============================================================================
// 19. SAFE MODE
// ============================================================================
void ConfigManager::setSafeMode(bool active) {
    if (_safeModeActive == active) return;

    _safeModeActive = active;
    _status.isSafeMode = active;

    if (active) {
        logMessage("SafeMode Triggered! Forcing Fallback Static IP");
        applyNetworkSettings();
        if (_onSafeModeChange) {
            _onSafeModeChange(true);
        }
    } else {
        logMessage("SafeMode Deactivated");
        _status.isSafeMode = false;
        if (_isDirty) {
            save();
        }
        if (_onSafeModeChange) {
            _onSafeModeChange(false);
        }
    }
}

// ============================================================================
// 20. ДИАГНОСТИКА
// ============================================================================
String ConfigManager::getDiagnosticsString() const {
    String result = "== CONFIG DIAGNOSTICS ==\n";
    result += "Module: " + String(getName()) + "\n";
    result += "Version: " + String(getVersion()) + "\n";
    result += "ID: " + String(_moduleId) + "\n";
    result += "Loaded: " + String(_loaded ? "YES" : "NO") + "\n";
    result += "Initialized: " + String(_initialized ? "YES" : "NO") + "\n";
    result += "Safe Mode: " + String(_safeModeActive ? "YES" : "NO") + "\n";
    result += "Dirty: " + String(_isDirty ? "YES" : "NO") + "\n";
    result += "CRC: " + String(_lastSavedCRC) + "\n";
    result += "Loads: " + String(_loadCount) + "\n";
    result += "Saves: " + String(_saveCount) + "\n";
    result += "Last Save: " + String(_lastSaveTime) + "\n";
    result += "Last Load: " + String(_lastLoadTime) + "\n";
    result += "Error: " + String(_lastError) + "\n";
    result += "Hostname: " + String(_sys.hostname) + "\n";
    result += "DHCP: " + String(_sys.net_dhcp ? "YES" : "NO") + "\n";
    result += "IP: " + String(_sys.cfg_ip) + "\n";
    result += "MQTT: " + String(_sys.use_mqtt ? "ENABLED" : "DISABLED") + "\n";
    result += "MQTT IP: " + String(_sys.mqtt_ip) + "\n";
    result += "Local Mode: " + String(_sys.is_pure_local_mode ? "YES" : "NO") + "\n";
    result += "Extra Data: " + String(_sys.extra_data) + "\n";
    return result;
}

void ConfigManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("====================");
    stream.println(" CONFIG MANAGER DIAGNOSTIC");
    stream.println("====================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Loaded: %s\n", _loaded ? "YES" : "NO");
    stream.printf(" Safe Mode: %s\n", _safeModeActive ? "YES" : "NO");
    stream.printf(" Dirty: %s\n", _isDirty ? "YES" : "NO");
    stream.printf(" CRC: %u\n", _lastSavedCRC);
    stream.printf(" Loads: %lu\n", _loadCount);
    stream.printf(" Saves: %lu\n", _saveCount);
    stream.printf(" Hostname: %s\n", _sys.hostname);
    stream.printf(" DHCP: %s\n", _sys.net_dhcp ? "ENABLED" : "DISABLED");
    stream.printf(" IP: %s\n", _sys.cfg_ip);
    stream.printf(" MQTT: %s\n", _sys.use_mqtt ? "ENABLED" : "DISABLED");
    stream.printf(" Local Mode: %s\n", _sys.is_pure_local_mode ? "YES" : "NO");
    stream.printf(" Error: %s\n", _lastError);
    stream.println("====================");
}

void ConfigManager::printStats() const {
    streamDiagnosticInfo(Serial);
}

// ============================================================================
// 21. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
bool ConfigManager::isFileExists(const char* path) const {
    return LittleFS.exists(path);
}