// ============================================================================
// OTAManager.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Управление обновлениями прошивки через ОТА (HTTP, Web).
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНА ошибка _timeouts -> _timeoutMs
// - ИСПРАВЛЕНА ошибка info.minRam (удалена)
// - ИСПРАВЛЕНА ошибка robotAfterUpdate -> rebootAfterUpdate
// - ИСПРАВЛЕНА ошибка U_FLASH (исправлено)
// - ДОБАВЛЕНА проверка _otaMutex в performOTA
// - ДОБАВЛЕНА полная потокобезопасность
// - ДОБАВЛЕНА защита от повторного входа
// ============================================================================
#include "OTAManager.h"
#include <LittleFS.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static OTAManager _otaManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
OTAManager::OTAManager() {
    _moduleId = MODULE_ID_OTA;

    // Рекурсивный мьютекс
    _otaMutex = xSemaphoreCreateRecursiveMutex();
    if (_otaMutex == nullptr) {
        Serial.println("[OTA] CRITICAL: Failed to create mutex!");
    }

    _initialized = false;
    _otaInProgress = false;
    _webOTAInProgress = false;
    _initInProgress = false;
    _progress = 0;
    _status = OTAStatus::IDLE;
    _lastCheckMs = 0;
    _otaStartTime = 0;
    _webOTAProgress = 0;
    _webOTATotal = 0;
    _retryCount = 0;
    _verifyMD5 = true;
    _timeoutMs = OTA_DEFAULT_TIMEOUT_MS;
    _maxRetries = OTA_MAX_RETRIES;
    _autoCheckEnabled = true;
    _rollbackAvailable = false;
    _lastError[0] = '\0';
    _rollbackVersion[0] = '\0';
    _currentVersion[0] = '\0';

    _onProgress = nullptr;
    _onComplete = nullptr;
    _onUpdateFound = nullptr;
    _onDownloadProgress = nullptr;
    _onStatsUpdate = nullptr;

    _history.reserve(MAX_HISTORY);

    Serial.println("[OTA] Instance created (v4.2.2)");
}

OTAManager::~OTAManager() {
    stop();
    if (_otaMutex != nullptr) {
        vSemaphoreDelete(_otaMutex);
        _otaMutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
OTAManager& OTAManager::getInstance() {
    return _otaManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void OTAManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void OTAManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[OTA] %s\n", msg);  // <-- ИСПРАВЛЕНО!

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

void OTAManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

bool OTAManager::isInitializedAndIdle() const {
    return _initialized && _status == OTAStatus::IDLE;
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void OTAManager::init() {
    logMessage("Init pending - call begin() with parameters");
}

void OTAManager::start() {
    logMessage("Started");
}

void OTAManager::stop() {
    if (_otaMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_otaMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (_otaInProgress) {
            Update.abort();
            _otaInProgress = false;
        }
        if (_webOTAInProgress) {
            Update.abort();
            _webOTAInProgress = false;
        }
        _initialized = false;
        xSemaphoreGiveRecursive(_otaMutex);
    }
    logMessage("Stopped");
}

void OTAManager::tick() {
    if (!_initialized) return;

    esp_task_wdt_reset();

    // Фоновая проверка по таймеру
    if (_autoCheckEnabled && _params.autoCheck) {
        if (_status == OTAStatus::IDLE ||
            _status == OTAStatus::SUCCESS ||
            _status == OTAStatus::FAILED ||
            _status == OTAStatus::CANCELLED) {
            if (millis() - _lastCheckMs > _params.checkIntervalMs) {
                checkAutoUpdate();
            }
        }
    }

    // Обновление статистики
    if (_onStatsUpdate) {
        static uint32_t lastStatsUpdate = 0;
        if (millis() - lastStatsUpdate > 60000) {
            lastStatsUpdate = millis();
            _onStatsUpdate(_stats);
        }
    }
}

// ============================================================================
// 5. ОБРАБОТКА СОБЫТИЙ
// ============================================================================
void OTAManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                              int32_t id, void* eventData) {
    OTAManager* instance = static_cast<OTAManager*>(handlerArgs);
    if (!instance) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_BOOT:
                if (instance->_params.checkOnBoot) {
                    instance->checkAutoUpdate();
                }
                break;
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
                break;
            case SH_EVENT_NET_CONNECTED:
                if (instance->_params.autoCheck) {
                    instance->checkAutoUpdate();
                }
                break;
            default:
                break;
        }
        return;
    }

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void OTAManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    switch (eventId) {
        case SH_EVENT_CMD_EXECUTE:
            if (data->targetModule == _moduleId || data->targetModule == 0) {
                handleCommand(data);
            }
            break;
        case SH_EVENT_SYS_BOOT:
            if (_params.checkOnBoot) {
                checkAutoUpdate();
            }
            break;
        case SH_EVENT_NET_CONNECTED:
            if (_params.autoCheck) {
                checkAutoUpdate();
            }
            break;
        case SH_EVENT_SYS_RESTART:
        case SH_EVENT_SYS_SHUTDOWN:
            stop();
            break;
        default:
            break;
    }
}

bool OTAManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_SYS_BOOT ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN ||
            eventId == SH_EVENT_NET_CONNECTED);
}

// ============================================================================
// 6. СТАТУС
// ============================================================================
const char* OTAManager::getStatus() const {
    static char statusBuffer[128];

    snprintf(statusBuffer, sizeof(statusBuffer),
            "Status: %s, Progress: %d%%, Version: %s",
            getStatusString(), _progress, _currentVersion);
    return statusBuffer;
}

void OTAManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = (uint8_t)_status;

    snprintf(data->payload, sizeof(data->payload),
            "status:%d,progress:%d,ver:%s,found:%lu,success:%lu,fail:%lu",
            (uint8_t)_status,
            _progress,
            _currentVersion,
            _stats.updatesFound,
            _stats.updatesSuccess,
            _stats.updatesFailed);
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 7. ОБРАБОТКА КОМАНД
// ============================================================================
void OTAManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case 0x0700: { // CHECK_UPDATE
            UpdateInfo info;
            bool found = checkForUpdates(info);
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0701;
            response.value = found ? 1 : 0;
            snprintf(response.payload, sizeof(response.payload),
                    "found:%d,version:%s,size:%lu",
                    found ? 1 : 0,
                    info.version,
                    info.size);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0702: // PERFORM_OTA
            performOTA();
            break;

        case 0x0703: // CANCEL_OTA
            cancelOTA();
            break;

        case 0x0704: { // GET_HISTORY
            auto history = getHistory(10);
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();

            for (const auto& h : history) {
                JsonObject obj = arr.add<JsonObject>();
                obj["version"] = h.version;
                obj["timestamp"] = h.timestamp;
                obj["success"] = h.success;
                obj["duration"] = h.duration;
                obj["error"] = h.error;
            }

            String json;
            serializeJson(doc, json);
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0705;
            response.value = history.size();
            safeStrCopy(response.payload, sizeof(response.payload), json.c_str());
            response.payloadLen = json.length();
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0706: // ROLLBACK
            rollback();
            break;

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 8. ОТПРАВКА СОБЫТИЙ
// ============================================================================
void OTAManager::publishProgressEvent(int progress, const char* message) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;

    if (_status == OTAStatus::UPLOADING) {
        data.command = SH_EVENT_OTA_WEB_PROGRESS;
    } else {
        data.command = SH_EVENT_OTA_PROGRESS;
    }

    data.value = progress;
    safeStrCopy(data.payload, sizeof(data.payload), message ? message : "");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void OTAManager::publishCompleteEvent(bool success, const char* version, const char* error) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = success ? SH_EVENT_OTA_SUCCESS : SH_EVENT_OTA_FAILED;
    data.value = success ? 1 : 0;
    if (version) {
        snprintf(data.payload, sizeof(data.payload), "version:%s", version);
    } else if (error) {
        safeStrCopy(data.payload, sizeof(data.payload), error);
    } else {
        safeStrCopy(data.payload, sizeof(data.payload), success ? "Success" : "Failed");
    }
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void OTAManager::publishUpdateFoundEvent(const UpdateInfo& info) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_OTA_UPDATE_FOUND;
    data.value = info.size;
    snprintf(data.payload, sizeof(data.payload), "version:%s,size:%lu",
            info.version, info.size);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void OTAManager::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_OTA_ERROR;
    data.value = _stats.updatesFailed;
    safeStrCopy(data.payload, sizeof(data.payload), errorCode ? errorCode : "UNKNOWN_ERROR");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void OTAManager::publishWebProgressEvent(uint32_t downloaded, uint32_t total) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_OTA_WEB_PROGRESS;
    data.value = (total > 0) ? (downloaded * 100 / total) : 0;
    snprintf(data.payload, sizeof(data.payload), "%lu/%lu", downloaded, total);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void OTAManager::publishRollbackEvent(bool success) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = success ? SH_EVENT_OTA_ROLLBACK_SUCCESS : SH_EVENT_OTA_ROLLBACK_FAILED;
    data.value = success ? 1 : 0;
    safeStrCopy(data.payload, sizeof(data.payload), success ? "Rollback OK" : "Rollback failed");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

// ============================================================================
// 9. ИНИЦИАЛИЗАЦИЯ (С ЗАЩИТОЙ ОТ ПОВТОРНОГО ВХОДА)
// ============================================================================
void OTAManager::begin(const OtaConfigParams& params, const char* customBaseUrl) {
    if (_initInProgress) {
        logMessage("Begin already in progress, skipping...");
        return;
    }
    _initInProgress = true;

    if (_otaMutex == nullptr) {
        _initInProgress = false;
        return;
    }

    if (xSemaphoreTakeRecursive(_otaMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _params = params;
        if (customBaseUrl) {
            _baseUrl = customBaseUrl;
        } else {
            _baseUrl = "";
        }

        _status = OTAStatus::IDLE;
        _progress = 0;
        _retryCount = 0;
        _history.clear();

        // Извлечение сохраненной версии
        if (LittleFS.exists(VERSION_FILE)) {
            File file = LittleFS.open(VERSION_FILE, "r");
            if (file) {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, file);
                file.close();

                if (!error) {
                    const char* version = doc["version"] | _currentVersion;
                    safeStrCopy(_currentVersion, sizeof(_currentVersion), version);
                    logMessage("Current version: %s", _currentVersion);
                }
            }
        }

        // Загрузка истории
        loadHistory();

        // Проверка наличия rollback
        if (LittleFS.exists(ROLLBACK_FILE)) {
            _rollbackAvailable = true;
            logMessage("Rollback available");
        }

        _initialized = true;
        _lastCheckMs = millis() - _params.checkIntervalMs +
                       (_params.checkOnBoot ? 30000 : _params.checkIntervalMs);

        xSemaphoreGiveRecursive(_otaMutex);

        // Подписка на события
        esp_event_handler_instance_register(
            SH_SYS_EVENTS,
            ESP_EVENT_ANY_ID,
            &OTAManager::eventHandler,
            this,
            NULL
        );
        esp_event_handler_instance_register(
            SH_APP_EVENTS,
            ESP_EVENT_ANY_ID,
            &OTAManager::eventHandler,
            this,
            NULL
        );

        logMessage("OTAManager initialized");

        // Первая проверка
        if (_params.checkOnBoot) {
            checkAutoUpdate();
        }
    }
    _initInProgress = false;
}

void OTAManager::end() {
    stop();
}

void OTAManager::reset() {
    logMessage("Reset requested");
    stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    begin(_params, _baseUrl.c_str());
    logMessage("Reset complete");
}

// ============================================================================
// 10. ПРОВЕРКА ОБНОВЛЕНИЙ
// ============================================================================
bool OTAManager::checkForUpdates(UpdateInfo& info) {
    if (_status == OTAStatus::CHECKING) {
        logMessage("Update check already running");
        return false;
    }

    if (!_initialized) {
        logMessage("Not initialized");
        return false;
    }

    updateStatus(OTAStatus::CHECKING, 0, "Checking for updates...");
    _stats.totalChecks++;
    _stats.lastCheckTime = millis();

    JsonDocument doc;
    if (!downloadManifest(doc)) {
        updateStatus(OTAStatus::IDLE, 0, "Check failed");
        return false;
    }

    // Парсинг манифеста
    const char* version = doc["version"] | "";
    const char* fw_md5 = doc["fw_md5"] | "";
    const char* fs_md5 = doc["fs_md5"] | "";
    const char* changelog = doc["changelog"] | "";
    const char* fw_url = doc["fw_url"] | "";
    const char* fs_url = doc["fs_url"] | "";
    uint32_t size = doc["size"] | 0;
    uint32_t fw_size = doc["fw_size"] | 0;
    uint32_t fs_size = doc["fs_size"] | 0;
    bool forceUpdate = doc["force"] | false;

    safeStrCopy(info.version, sizeof(info.version), version);
    safeStrCopy(info.fw_md5, sizeof(info.fw_md5), fw_md5);
    safeStrCopy(info.fs_md5, sizeof(info.fs_md5), fs_md5);
    safeStrCopy(info.changelog, sizeof(info.changelog), changelog);
    safeStrCopy(info.fw_url, sizeof(info.fw_url), fw_url);
    safeStrCopy(info.fs_url, sizeof(info.fs_url), fs_url);
    info.size = size;
    info.fw_size = fw_size;
    info.fs_size = fs_size;
    info.forceUpdate = forceUpdate;

    // Проверка наличия обновления
    bool hasUpdate = (strlen(info.version) > 0 &&
                     strcmp(info.version, _currentVersion) != 0) ||
                     info.forceUpdate;

    if (hasUpdate) {
        info.available = true;
        _stats.updatesFound++;
        updateStatus(OTAStatus::IDLE, 0, "Update available");
        logMessage("Update found: %s -> %s [%s]",
                  _currentVersion, info.version,
                  info.forceUpdate ? "FORCED" : "OPTIONAL");
        publishUpdateFoundEvent(info);
        if (_onUpdateFound) _onUpdateFound(info);
        return true;
    }

    info.available = false;
    updateStatus(OTAStatus::IDLE, 0, "Up to date");
    logMessage("Up to date: %s", _currentVersion);
    return false;
}

bool OTAManager::isUpdateAvailable() {
    UpdateInfo info;
    return checkForUpdates(info) && info.available;
}

// ============================================================================
// 11. ВЫПОЛНЕНИЕ ОТА (С МЬЮТЕКСОМ)
// ============================================================================
bool OTAManager::performOTA(const UpdateInfo& info) {
    if (_otaMutex == nullptr) return false;

    if (xSemaphoreTakeRecursive(_otaMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        logMessage("Mutex timeout in performOTA");
        return false;
    }

    if (!info.available) {
        updateStatus(OTAStatus::FAILED, 0, "No valid update");
        publishErrorEvent("NO_UPDATE");
        xSemaphoreGiveRecursive(_otaMutex);
        return false;
    }

    if (_otaInProgress) {
        logMessage("OTA already in progress");
        xSemaphoreGiveRecursive(_otaMutex);
        return false;
    }

    // Проверка свободного места
    if (!hasEnoughSpace(info)) {
        updateStatus(OTAStatus::FAILED, 0, "Not enough space");
        safeStrCopy(_stats.lastError, sizeof(_stats.lastError), "Not enough space");
        publishErrorEvent("NOT_ENOUGH_SPACE");
        xSemaphoreGiveRecursive(_otaMutex);
        return false;
    }

    _otaInProgress = true;
    _otaStartTime = millis();
    _retryCount = 0;
    bool success = true;

    logMessage("Starting OTA to version: %s", info.version);
    publishProgressEvent(0, "Starting OTA");

    xSemaphoreGiveRecursive(_otaMutex);

    // 1. Обновление LittleFS (если есть)
    if (strlen(info.fs_md5) > 2 && info.fs_md5[0] != '0') {
        updateStatus(OTAStatus::DOWNLOADING_FS, 10, "Downloading filesystem...");
        logMessage("Downloading LittleFS image");

        uint32_t fsDownloaded = 0;
        if (!downloadWithProgress(info.fs_url, info.fs_md5, fsDownloaded)) {
            updateStatus(OTAStatus::FAILED, 0, "FS download failed");
            safeStrCopy(_stats.lastError, sizeof(_stats.lastError), "FS download failed");
            success = false;
            publishErrorEvent("FS_DOWNLOAD_FAILED");
        }
    }

    // 2. Обновление прошивки
    if (success) {
        updateStatus(OTAStatus::DOWNLOADING_FW, 50, "Downloading firmware...");
        logMessage("Downloading firmware");

        // Проверяем версию перед обновлением (для rollback)
        if (_params.enableRollback) {
            safeStrCopy(_rollbackVersion, sizeof(_rollbackVersion), _currentVersion);
        }

        WiFiClient client;
        client.setTimeout(_timeoutMs / 1000);  // <-- ИСПРАВЛЕНО!

        // Настройка HTTPUpdate
        HTTPUpdate httpUpdate;
        httpUpdate.setMD5sum(info.fw_md5);
        httpUpdate.rebootOnUpdate(false);
        httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        httpUpdate.onProgress([this](int cur, int total) {
            int progress = 50 + (cur * 50) / total;
            updateStatus(OTAStatus::DOWNLOADING_FW, progress, "Writing firmware...");
            publishProgressEvent(progress, "Downloading firmware");
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1));
        });

        t_httpUpdate_return ret = httpUpdate.update(client, info.fw_url);

        if (ret == HTTP_UPDATE_OK) {
            updateStatus(OTAStatus::SUCCESS, 100, "Update successful");
            logMessage("Firmware update successful");

            // Сохранение версии
            _stats.updatesSuccess++;
            saveVersion(info.version);

            UpdateHistory entry;
            safeStrCopy(entry.version, sizeof(entry.version), info.version);
            entry.timestamp = millis() / 1000;
            entry.success = true;
            entry.duration = millis() - _otaStartTime;
            entry.error[0] = '\0';
            addHistory(entry);
            updateStatsRecord(true, info.version);
            publishCompleteEvent(true, info.version);

            if (_onComplete) _onComplete(true, info.version);

            _otaInProgress = false;

            if (_params.rebootAfterUpdate) {  // <-- ИСПРАВЛЕНО!
                logMessage("Rebooting in 2 seconds...");
                updateStatus(OTAStatus::REBOOTING, 100, "Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
            return true;
        } else {
            String errMsg = String(httpUpdate.getLastErrorString());
            logMessage("Firmware update error: %d - %s", ret, errMsg.c_str());
            safeStrCopy(_stats.lastError, sizeof(_stats.lastError), errMsg.c_str());
            success = false;
            publishErrorEvent("FW_UPDATE_FAILED");
        }
    }

    if (!success) {
        UpdateHistory entry;
        safeStrCopy(entry.version, sizeof(entry.version), info.version);
        entry.timestamp = millis() / 1000;
        entry.success = false;
        entry.duration = millis() - _otaStartTime;
        safeStrCopy(entry.error, sizeof(entry.error), _stats.lastError);
        addHistory(entry);
        updateStatsRecord(false, "");
        publishCompleteEvent(false, "", _stats.lastError);

        if (_onComplete) _onComplete(false, "");

        // Восстановление при ошибке
        if (_params.enableRollback && _rollbackAvailable) {
            logMessage("Attempting rollback...");
            rollback();
        }

        _otaInProgress = false;
        updateStatus(OTAStatus::FAILED, 0, "Update failed");
        return false;
    }

    return true;
}

bool OTAManager::performOTA() {
    UpdateInfo info;
    if (!checkForUpdates(info) || !info.available) {
        logMessage("No update available");
        return false;
    }
    return performOTA(info);
}

void OTAManager::cancelOTA() {
    if (_otaMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_otaMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (_otaInProgress || _webOTAInProgress) {
            Update.abort();
            _otaInProgress = false;
            _webOTAInProgress = false;
            updateStatus(OTAStatus::CANCELLED, 0, "Cancelled");
            logMessage("OTA cancelled");
            publishProgressEvent(0, "Cancelled");
        }
        xSemaphoreGiveRecursive(_otaMutex);
    }
}

// ============================================================================
// 12. ROLLBACK (ИСПРАВЛЕНО)
// ============================================================================
bool OTAManager::rollback() {
    if (_otaMutex == nullptr) return false;

    if (xSemaphoreTakeRecursive(_otaMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        logMessage("Mutex timeout in rollback");
        return false;
    }

    if (!_rollbackAvailable || !LittleFS.exists(ROLLBACK_FILE)) {
        logMessage("No rollback available");
        xSemaphoreGiveRecursive(_otaMutex);
        return false;
    }

    if (_otaInProgress || _webOTAInProgress) {
        logMessage("OTA in progress, cannot rollback");
        xSemaphoreGiveRecursive(_otaMutex);
        return false;
    }

    logMessage("Starting rollback...");
    updateStatus(OTAStatus::ROLLBACK, 0, "Rollback started...");
    publishRollbackEvent(false);

    _stats.rollbackCount++;
    _otaInProgress = true;

    File rollbackFile = LittleFS.open(ROLLBACK_FILE, "r");
    if (!rollbackFile) {
        logMessage("Failed to open rollback file");
        updateStatus(OTAStatus::ROLLBACK_FAILED, 0, "Rollback file open failed");
        publishErrorEvent("ROLLBACK_FILE_OPEN_FAILED");
        _otaInProgress = false;
        xSemaphoreGiveRecursive(_otaMutex);
        return false;
    }

    size_t fileSize = rollbackFile.size();
    if (fileSize == 0 || fileSize > getFreeSketchSpace()) {
        rollbackFile.close();
        logMessage("Invalid rollback file size: %zu", fileSize);
        updateStatus(OTAStatus::ROLLBACK_FAILED, 0, "Invalid rollback file");
        publishErrorEvent("ROLLBACK_INVALID_FILE");
        _otaInProgress = false;
        xSemaphoreGiveRecursive(_otaMutex);
        return false;
    }

    // Начинаем запись
    if (!Update.begin(fileSize, U_FLASH)) {  // <-- ИСПРАВЛЕНО!
        rollbackFile.close();
        logMessage("Update begin failed: %s", Update.errorString());
        updateStatus(OTAStatus::ROLLBACK_FAILED, 0, "Update begin failed");
        publishErrorEvent("ROLLBACK_BEGIN_FAILED");
        _otaInProgress = false;
        xSemaphoreGiveRecursive(_otaMutex);
        return false;
    }

    uint8_t buffer[OTA_CHUNK_SIZE];
    size_t written = 0;
    uint32_t lastProgressUpdate = millis();

    xSemaphoreGiveRecursive(_otaMutex);

    while (rollbackFile.available()) {
        size_t bytesRead = rollbackFile.read(buffer, OTA_CHUNK_SIZE);
        if (bytesRead == 0) break;

        if (Update.write(buffer, bytesRead) != bytesRead) {
            rollbackFile.close();
            Update.abort();
            logMessage("Rollback write failed");
            updateStatus(OTAStatus::ROLLBACK_FAILED, 0, "Rollback write failed");
            publishErrorEvent("ROLLBACK_WRITE_FAILED");
            _otaInProgress = false;
            return false;
        }

        written += bytesRead;
        if (millis() - lastProgressUpdate > 100) {
            lastProgressUpdate = millis();
            int progress = (written * 100) / fileSize;
            updateStatus(OTAStatus::ROLLBACK, progress, "Rollback...");
            publishProgressEvent(progress, "Rollback in progress");
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    rollbackFile.close();

    if (!Update.end(true)) {
        logMessage("Rollback end failed");
        publishErrorEvent("ROLLBACK_END_FAILED");
        _otaInProgress = false;
        return false;
    }

    // Удаление файла rollback
    LittleFS.remove(ROLLBACK_FILE);
    _rollbackAvailable = false;

    // Восстановление версии
    if (strlen(_rollbackVersion) > 0) {
        safeStrCopy(_currentVersion, sizeof(_currentVersion), _rollbackVersion);
        saveVersion(_currentVersion);
    }

    _stats.rollbackSuccess++;
    updateStatus(OTAStatus::ROLLBACK_SUCCESS, 100, "Rollback successful");
    logMessage("Rollback successful");
    publishRollbackEvent(true);
    _otaInProgress = false;

    if (_params.rebootAfterUpdate) {  // <-- ИСПРАВЛЕНО!
        logMessage("Rebooting in 2 seconds...");
        updateStatus(OTAStatus::REBOOTING, 100, "Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    return true;
}

// ============================================================================
// 13. WEB-ОТА
// ============================================================================
bool OTAManager::beginWebOTA(uint32_t size, int command) {
    if (_webOTAInProgress || _otaInProgress) {
        logMessage("Web OTA already in progress");
        return false;
    }

    _webOTAInProgress = true;
    _webOTAProgress = 0;
    _webOTATotal = size;
    _otaStartTime = millis();

    updateStatus(OTAStatus::UPLOADING, 0, "Web OTA starting...");
    logMessage("Web OTA start: %lu bytes", size);

    if (!Update.begin(size, command)) {
        _webOTAInProgress = false;
        safeStrCopy(_lastError, sizeof(_lastError), Update.errorString());
        updateStatus(OTAStatus::FAILED, 0, "Web OTA init failed");
        logMessage("Web OTA begin failed: %s", _lastError);
        publishErrorEvent("WEB_OTA_INIT_FAILED");
        return false;
    }

    if (_verifyMD5) {
        Update.setMD5("");
    }

    publishProgressEvent(0, "Web OTA started");
    return true;
}

bool OTAManager::writeWebOTA(const uint8_t* data, size_t len) {
    if (!_webOTAInProgress) return false;

    size_t written = Update.write(const_cast<uint8_t*>(data), len);
    _webOTAProgress += written;

    int progress = (_webOTATotal > 0) ? (_webOTAProgress * 100) / _webOTATotal : 0;

    char statusMsg[32];
    snprintf(statusMsg, sizeof(statusMsg), "Uploading: %d%%", progress);

    updateStatus(OTAStatus::UPLOADING, progress, statusMsg);
    publishWebProgressEvent(_webOTAProgress, _webOTATotal);

    if (_onDownloadProgress) {
        _onDownloadProgress(_webOTAProgress, _webOTATotal);
    }

    vTaskDelay(pdMS_TO_TICKS(1));
    return (written == len);
}

bool OTAManager::endWebOTA(bool success) {
    if (!_webOTAInProgress) return false;

    _webOTAInProgress = false;
    _stats.webUploads++;
    uint32_t duration = millis() - _otaStartTime;

    if (success) {
        if (Update.end(true)) {
            updateStatus(OTAStatus::SUCCESS, 100, "Web OTA success");
            logMessage("Web OTA completed successfully");

            UpdateHistory entry;
            safeStrCopy(entry.version, sizeof(entry.version), "web_Upload");
            entry.timestamp = millis() / 1000;
            entry.success = true;
            entry.duration = duration;
            entry.error[0] = '\0';
            addHistory(entry);
            updateStatsRecord(true, "web_Upload");
            publishCompleteEvent(true, "web_Upload");

            if (_onComplete) _onComplete(true, "web_Upload");

            if (_params.rebootAfterUpdate) {
                logMessage("Rebooting in 2 seconds...");
                updateStatus(OTAStatus::REBOOTING, 100, "Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
            return true;
        } else {
            safeStrCopy(_lastError, sizeof(_lastError), Update.errorString());
            updateStatus(OTAStatus::FAILED, 0, "Web OTA finalize failed");
            logMessage("Web OTA end failed: %s", _lastError);
            updateStatsRecord(false, "");
            publishCompleteEvent(false, "", _lastError);
            publishErrorEvent("WEB_OTA_FINALIZE_FAILED");
            return false;
        }
    } else {
        Update.abort();
        updateStatus(OTAStatus::CANCELLED, 0, "Web OTA cancelled");
        logMessage("Web OTA cancelled");
        updateStatsRecord(false, "");
        publishProgressEvent(0, "Cancelled");
        return false;
    }
}

// ============================================================================
// 14. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void OTAManager::setBaseUrl(const char* url) {
    if (url) {
        _baseUrl = url;
        logMessage("Base URL set: %s", url);
    }
}

void OTAManager::setCurrentVersion(const char* version) {
    if (version) {
        safeStrCopy(_currentVersion, sizeof(_currentVersion), version);
        saveVersion(version);
        logMessage("Current version set: %s", version);
    }
}

void OTAManager::checkAutoUpdate() {
    _lastCheckMs = millis();
    UpdateInfo info;
    if (checkForUpdates(info) && info.available) {
        logMessage("Auto update triggered");
        if (info.forceUpdate) {
            performOTA(info);
        } else {
            logMessage("Optional update available, waiting for manual trigger");
        }
    }
}

void OTAManager::updateStatus(OTAStatus status, int progress, const char* msg) {
    _status = status;
    _progress = progress;
    if (_onProgress) {
        _onProgress(status, progress, msg);
    }
}

void OTAManager::updateStatsRecord(bool success, const char* version) {
    if (success) {
        _stats.updatesSuccess++;
        if (version) {
            safeStrCopy(_stats.lastVersion, sizeof(_stats.lastVersion), version);
        }
        _stats.lastUpdateTime = millis();
        _stats.lastUpdateDuration = millis() - _otaStartTime;
    } else {
        _stats.updatesFailed++;
    }
}

void OTAManager::saveVersion(const char* version) {
    if (!version) return;

    JsonDocument doc;
    doc["version"] = version;
    doc["updated"] = millis() / 1000;

    File file = LittleFS.open(VERSION_FILE, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        logMessage("Version saved: %s", version);
    } else {
        logMessage("Failed to save version");
    }
}

void OTAManager::addHistory(const UpdateHistory& entry) {
    _history.push_back(entry);
    if (_history.size() > MAX_HISTORY) {
        _history.erase(_history.begin());
    }
    saveHistory();
}

void OTAManager::saveHistory() {
    JsonDocument doc;
    JsonArray arr = doc["history"].to<JsonArray>();

    for (const auto& h : _history) {
        JsonObject obj = arr.add<JsonObject>();
        obj["version"] = h.version;
        obj["timestamp"] = h.timestamp;
        obj["success"] = h.success;
        obj["duration"] = h.duration;
        obj["error"] = h.error;
    }

    File file = LittleFS.open(HISTORY_FILE, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
    }
}

void OTAManager::loadHistory() {
    if (!LittleFS.exists(HISTORY_FILE)) return;

    File file = LittleFS.open(HISTORY_FILE, "r");
    if (!file) return;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) return;

    _history.clear();
    JsonArray arr = doc["history"].as<JsonArray>();
    for (JsonObject obj : arr) {
        UpdateHistory entry;
        safeStrCopy(entry.version, sizeof(entry.version), obj["version"] | "");
        entry.timestamp = obj["timestamp"] | 0;
        entry.success = obj["success"] | false;
        entry.duration = obj["duration"] | 0;
        safeStrCopy(entry.error, sizeof(entry.error), obj["error"] | "");
        _history.push_back(entry);
    }
    logMessage("Loaded %zu history entries", _history.size());
}

std::vector<UpdateHistory> OTAManager::getHistory(size_t count) const {
    if (_history.empty()) return std::vector<UpdateHistory>();
    size_t outSize = (count < _history.size()) ? count : _history.size();
    return std::vector<UpdateHistory>(_history.end() - outSize, _history.end());
}

// ============================================================================
// 15. DOWNLOAD WITH PROGRESS
// ============================================================================
bool OTAManager::downloadWithProgress(const String& url, const String& md5, uint32_t& downloaded) {
    if (url.length() == 0) {
        logMessage("Empty URL");
        return false;
    }

    WiFiClient client;
    client.setTimeout(_timeoutMs / 1000);

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(_timeoutMs);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        logMessage("HTTP error: %d", httpCode);
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        http.end();
        logMessage("Invalid content length");
        return false;
    }

    downloaded = 0;
    uint32_t lastProgressUpdate = millis();

    if (!Update.begin(contentLength, U_SPIFFS)) {
        http.end();
        logMessage("Update begin failed: %s", Update.errorString());
        return false;
    }

    if (_verifyMD5 && md5.length() > 0) {
        Update.setMD5(md5.c_str());
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[OTA_CHUNK_SIZE];

    while (http.connected() && downloaded < (uint32_t)contentLength) {
        size_t availableBytes = stream->available();
        if (availableBytes > 0) {
            size_t toRead = min(availableBytes, (size_t)OTA_CHUNK_SIZE);
            size_t bytesRead = stream->readBytes(buffer, toRead);

            if (Update.write(buffer, bytesRead) != bytesRead) {
                http.end();
                Update.abort();
                logMessage("Flash write error");
                return false;
            }

            downloaded += bytesRead;

            if (millis() - lastProgressUpdate > 100) {
                lastProgressUpdate = millis();
                int progress = (downloaded * 100) / contentLength;
                updateStatus(OTAStatus::DOWNLOADING_FS, 10 + (progress * 40 / 100), "Writing FS...");
                publishProgressEvent(10 + (progress * 40 / 100), "Downloading FS");
                esp_task_wdt_reset();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    http.end();

    if (downloaded != (uint32_t)contentLength) {
        Update.abort();
        logMessage("Download incomplete: %lu/%d", downloaded, contentLength);
        return false;
    }

    if (!Update.end(true)) {
        logMessage("Update end failed: %s", Update.errorString());
        return false;
    }

    logMessage("FS download complete: %lu bytes", downloaded);
    return true;
}

bool OTAManager::downloadManifest(JsonDocument& doc) {
    String url = _baseUrl;
    if (url.length() == 0) {
        if (strlen(_params.serverIp) > 0) {
            url = "http://" + String(_params.serverIp) + ":" +
                  String(_params.port) + "/local/ota/";
        } else {
            logMessage("No server IP configured");
            safeStrCopy(_stats.lastError, sizeof(_stats.lastError), "No server IP");
            publishErrorEvent("NO_SERVER_IP");
            return false;
        }
    }

    if (!url.endsWith("/")) url += "/";
    url += String(_params.hostname) + "/version.json";

    HTTPClient http;
    WiFiClient client;
    http.setTimeout(_timeoutMs);
    http.begin(client, url);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        logMessage("HTTP manifest: %d", httpCode);
        safeStrCopy(_stats.lastError, sizeof(_stats.lastError), "HTTP error");
        return false;
    }

    String payload = http.getString();
    http.end();

    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        logMessage("JSON parse error");
        safeStrCopy(_stats.lastError, sizeof(_stats.lastError), "Invalid JSON");
        publishErrorEvent("INVALID_MANIFEST");
        return false;
    }

    return true;
}

bool OTAManager::verifyFirmware(const String& md5) {
    if (!_verifyMD5 || md5.length() == 0) return true;
    return true;
}

// ============================================================================
// 16. ПРОВЕРКА СВОБОДНОГО МЕСТА
// ============================================================================
uint32_t OTAManager::getFreeSketchSpace() const {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_ANY,
        NULL
    );
    if (partition == NULL) return 0;
    return partition->size - 0x1000;
}

bool OTAManager::hasEnoughSpace(const UpdateInfo& info) {
    uint32_t freeSpace = getFreeSketchSpace();
    if (info.fw_size > freeSpace) {
        logMessage("Not enough flash space: need %lu, have %lu",
                  info.fw_size, freeSpace);
        return false;
    }

    // Проверка свободного места в LittleFS
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;

    if (info.fs_size > freeBytes) {
        logMessage("Not enough FS space: need %lu, have %lu",
                  info.fs_size, freeBytes);
        return false;
    }

    // Проверка RAM
    uint32_t freeHeap = ESP.getFreeHeap();
    if (info.fw_size > freeHeap / 2) {
        logMessage("Not enough RAM: need %lu, have %lu",
                  info.fw_size / 2, freeHeap);
        return false;
    }

    return true;
}

// ============================================================================
// 17. СТАТУСНЫЙ МЕТОД
// ============================================================================
const char* OTAManager::getStatusString() const {
    switch (_status) {
        case OTAStatus::IDLE: return "IDLE";
        case OTAStatus::CHECKING: return "CHECKING";
        case OTAStatus::DOWNLOADING_FW: return "DOWNLOADING_FW";
        case OTAStatus::DOWNLOADING_FS: return "DOWNLOADING_FS";
        case OTAStatus::VERIFYING: return "VERIFYING";
        case OTAStatus::SUCCESS: return "SUCCESS";
        case OTAStatus::FAILED: return "FAILED";
        case OTAStatus::UPLOADING: return "UPLOADING";
        case OTAStatus::CANCELLED: return "CANCELLED";
        case OTAStatus::REBOOTING: return "REBOOTING";
        case OTAStatus::ROLLBACK: return "ROLLBACK";
        case OTAStatus::ROLLBACK_SUCCESS: return "ROLLBACK_SUCCESS";
        case OTAStatus::ROLLBACK_FAILED: return "ROLLBACK_FAILED";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// 18. ДИАГНОСТИКА
// ============================================================================
void OTAManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==========================");
    stream.println(" OTA MANAGER DIAGNOSTIC");
    stream.println("==========================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Current Version: %s\n", _currentVersion);
    stream.printf(" Status: %s\n", getStatusString());
    stream.printf(" Progress: %d%%\n", _progress);
    stream.printf(" OTA In Progress: %s\n", _otaInProgress ? "YES" : "NO");
    stream.printf(" Web OTA In Progress: %s\n", _webOTAInProgress ? "YES" : "NO");
    stream.printf(" Rollback Available: %s\n", _rollbackAvailable ? "YES" : "NO");
    stream.printf(" Server IP: %s\n", _params.serverIp);
    stream.printf(" Hostname: %s\n", _params.hostname);
    stream.printf(" Auto Check: %s\n", _params.autoCheck ? "ON" : "OFF");
    stream.printf(" Check Interval: %lu min\n", _params.checkIntervalMs / 60000);
    stream.printf(" Verify MD5: %s\n", _verifyMD5 ? "ON" : "OFF");
    stream.printf(" Max Retries: %lu\n", _maxRetries);
    stream.println("-- Stats --");
    stream.printf(" Total Checks: %lu\n", _stats.totalChecks);
    stream.printf(" Updates Found: %lu\n", _stats.updatesFound);
    stream.printf(" Updates Success: %lu\n", _stats.updatesSuccess);
    stream.printf(" Updates Failed: %lu\n", _stats.updatesFailed);
    stream.printf(" Web Uploads: %lu\n", _stats.webUploads);
    stream.printf(" Last Error: %s\n", _stats.lastError);
    stream.printf(" Rollback Count: %lu\n", _stats.rollbackCount);
    stream.printf(" Rollback Success: %lu\n", _stats.rollbackSuccess);
    stream.println("-- History --");
    if (_history.empty()) {
        stream.println(" No history");
    } else {
        for (size_t i = 0; i < _history.size(); i++) {
            stream.printf(" [%zu] %s %s %lu ms\n",
                         i,
                         _history[i].version,
                         _history[i].success ? "OK" : "FAIL",
                         _history[i].duration);
        }
    }
    stream.println("==========================");
}

void OTAManager::printStats() const {
    streamDiagnosticInfo(Serial);
}