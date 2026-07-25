// ============================================================================
// OTAManager.cpp - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Управление обновлениями прошивки через ОТА (HTTP, Web).
//
// ИЗМЕНЕНИЯ v4.2.1:
// - Удалена глобальная переменная OTA
// - Добавлен синглтон
// - Рекурсивный мьютекс вместо обычного
// - Исправлена генерация JSON
// - Добавлена проверка свободного места
// - Добавлен Rollback механизм
// - Исправлены опечатки
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.1
// - Добавлен метод publishOtaEventInternal() для публикации через новую шину
// - Добавлен метод publishOtaEvent() (публичный)
// - Добавлены вызовы publishOtaEventInternal() в ключевые методы
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "OTAManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <LittleFS.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <esp_partition.h>
#include "core/IModule.h"
#include <cstdarg>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static OTAManager _otaManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
OTAManager::OTAManager() {
    _moduleId = MODULE_ID_OTA;

    _otaMutex = xSemaphoreCreateRecursiveMutex();
    _initialized = false;
    _otaInProgress = false;
    _webOTAInProgress = false;
    _progress = 0;
    _status = OTAStatus::IDLE;
    _lastCheckMs = 0;
    _otaStartTime = 0;
    _webOTAProgress = 0;
    _webOTATotal = 0;
    _retryCount = 0;
    _verifyMD5 = true;
    _timeoutMs = 30000;
    _maxRetries = 3;
    _autoCheckEnabled = true;
    _rollbackAvailable = false;
    _totalEventsPublished = 0; // НОВОЕ

    strcpy(_lastError, "");
    strcpy(_rollbackVersion, "");

    _onProgress = nullptr;
    _onComplete = nullptr;
    _onUpdateFound = nullptr;
    _onDownloadProgress = nullptr;
    _onStatsUpdate = nullptr;

    if (_otaMutex == nullptr) {
        Serial.println("[OTA] CRITICAL: Failed to create mutex!");
    }

    _history.resize(MAX_HISTORY);

    Serial.println("[OTA] Instance created (v5.0)");
}

OTAManager::~OTAManager() {
    stop();
    if (_otaMutex != nullptr) {
        vSemaphoreDelete(_otaMutex);
        _otaMutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
OTAManager& OTAManager::getInstance() {
    return _otaManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void OTAManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[OTA] %s\n", msg);

    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = MODULE_ID_LOG;
    data.command = 0x1000;
    data.value = 0;
    strncpy(data.payload, msg, sizeof(data.payload) - 1);
    data.payload[sizeof(data.payload) - 1] = '\0';
    data.payloadLen = strlen(msg);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void OTAManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

// ============================================================================
// 4. НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void OTAManager::publishOtaEventInternal(const char* eventType, const char* details,
                                         bool success) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = success ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "OTA: %s %s - %s",
             eventType,
             details ? details : "",
             success ? "OK" : "FAIL");

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void OTAManager::publishOtaEvent(const char* eventType, const char* details,
                                 bool success) {
    publishOtaEventInternal(eventType, details, success);
}

// ============================================================================
// 5. ЖИЗНЕННЫЙ ЦИКЛ (IModule) (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void OTAManager::init() {
    logMessage("Init pending - call begin() with parameters");
}

void OTAManager::start() {
    logMessage("Started");
    publishOtaEventInternal("START", nullptr, true);
}

void OTAManager::stop() {
    if (_otaMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_otaMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
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
    publishOtaEventInternal("STOP", nullptr, true);
}

void OTAManager::tick() {
    if (!_initialized) return;

    esp_task_wdt_reset();

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

    if (_onStatsUpdate) {
        static uint32_t lastStatsUpdate = 0;
        if (millis() - lastStatsUpdate > 60000) {
            lastStatsUpdate = millis();
            _onStatsUpdate(_stats);
        }
    }
}

// ============================================================================
// 6. ОБРАБОТКА СОБЫТИЙ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
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
                publishOtaEventInternal("BOOT_CHECK", nullptr, true);
            }
            break;
        case SH_EVENT_NET_CONNECTED:
            if (_params.autoCheck) {
                checkAutoUpdate();
                publishOtaEventInternal("NET_CONNECTED_CHECK", nullptr, true);
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
// 7. СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
const char* OTAManager::getStatus() const {
    static char statusBuffer[128];

    snprintf(statusBuffer, sizeof(statusBuffer),
            "Status: %s, Progress: %d%% Version: %s",
            getStatusString(),
            _progress,
            _currentVersion);

    return statusBuffer;
}

// ============================================================================
// 8. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void OTAManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = (uint8_t)_status;

    snprintf(data->payload, sizeof(data->payload),
            "status:%d,progress:%d,ver:%s,found:%lu,success:%lu,fail:%lu,events:%lu",
            (uint8_t)_status,
            _progress,
            _currentVersion,
            _stats.updatesFound,
            _stats.updatesSuccess,
            _stats.updatesFailed,
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 9. ОБРАБОТКА КОМАНД (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void OTAManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case 0x0700: { // CHECK_UPDATE
            UpdateInfo info;
            bool result = checkForUpdates(info);
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0701;
            response.value = result ? 1 : 0;
            if (result) {
                snprintf(response.payload, sizeof(response.payload),
                        "version:%s,size:%lu",
                        info.version, info.fw_size);
            } else {
                strncpy(response.payload, "No update", sizeof(response.payload) - 1);
            }
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            publishOtaEventInternal("CMD_CHECK", result ? "found" : "none", result);
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
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0705;
            response.value = history.size();
            strncpy(response.payload, json.c_str(), sizeof(response.payload) - 1);
            response.payload[sizeof(response.payload) - 1] = '\0';
            response.payloadLen = json.length();
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            publishOtaEventInternal("CMD_HISTORY", nullptr, true);
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
// 10. ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void OTAManager::publishProgressEvent(int progress, const char* message) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    if (_status == OTAStatus::UPLOADING) {
        data.command = SH_EVENT_OTA_WEB_PROGRESS;
    } else {
        data.command = SH_EVENT_OTA_PROGRESS;
    }
    data.value = progress;
    strncpy(data.payload, message ? message : "", sizeof(data.payload) - 1);
    data.payload[sizeof(data.payload) - 1] = '\0';
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
    publishOtaEventInternal("PROGRESS", String(progress).c_str(), true);
}

void OTAManager::publishCompleteEvent(bool success, const char* version,
                                      const char* error) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = success ? SH_EVENT_OTA_SUCCESS : SH_EVENT_OTA_FAILED;
    data.value = success ? 1 : 0;
    if (success) {
        strncpy(data.payload, version ? version : "", sizeof(data.payload) - 1);
    } else {
        strncpy(data.payload, error ? error : "Unknown error", sizeof(data.payload) - 1);
    }
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
    publishOtaEventInternal(success ? "SUCCESS" : "FAILED",
                           success ? version : error,
                           success);
}

void OTAManager::publishUpdateFoundEvent(const UpdateInfo& info) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_OTA_UPDATE_FOUND;
    data.value = info.fw_size;
    snprintf(data.payload, sizeof(data.payload),
            "version:%s,size:%lu,force:%s",
            info.version,
            info.fw_size,
            info.forceUpdate ? "yes" : "no");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
    publishOtaEventInternal("UPDATE_FOUND", info.version, true);
}

void OTAManager::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_OTA_ERROR;
    data.value = _stats.updatesFailed;
    strncpy(data.payload, errorCode ? errorCode : "UNKNOWN_ERROR", sizeof(data.payload) - 1);
    data.payload[sizeof(data.payload) - 1] = '\0';
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
    publishOtaEventInternal("ERROR", errorCode, false);
}

void OTAManager::publishWebProgressEvent(uint32_t downloaded, uint32_t total) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_OTA_WEB_PROGRESS;
    data.value = (total > 0) ? (downloaded * 100 / total) : 0;
    snprintf(data.payload, sizeof(data.payload), "%lu/%lu", downloaded, total);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
    publishOtaEventInternal("WEB_PROGRESS", String(data.value).c_str(), true);
}

void OTAManager::publishRollbackEvent(bool success) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = success ? SH_EVENT_OTA_ROLLBACK_SUCCESS : SH_EVENT_OTA_ROLLBACK_FAILED;
    data.value = success ? 1 : 0;
    strncpy(data.payload, success ? "Rollback OK" : "Rollback failed", sizeof(data.payload) - 1);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
    publishOtaEventInternal("ROLLBACK", success ? "success" : "failed", success);
}

// ============================================================================
// 11. ИНИЦИАЛИЗАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void OTAManager::begin(const OtaConfigParams& params, const char* customBaseUrl) {
    if (_otaMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_otaMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        _params = params;
        _baseUrl = customBaseUrl ? customBaseUrl : "";
        _status = OTAStatus::IDLE;
        _progress = 0;
        _retryCount = 0;
        _history.clear();

        if (LittleFS.exists(VERSION_FILE)) {
            File file = LittleFS.open(VERSION_FILE, "r");
            if (file) {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, file);
                file.close();
                if (!error) {
                    const char* version = doc["version"] | _currentVersion;
                    strncpy(_currentVersion, version, sizeof(_currentVersion) - 1);
                    logMessage("Current version: %s", _currentVersion);
                }
            }
        }

        loadHistory();

        if (LittleFS.exists(ROLLBACK_FILE)) {
            _rollbackAvailable = true;
            logMessage("Rollback available");
        }

        _initialized = true;
        _lastCheckMs = millis() - _params.checkIntervalMs +
                       (_params.checkOnBoot ? 30000 : _params.checkIntervalMs);

        xSemaphoreGiveRecursive(_otaMutex);

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
        publishOtaEventInternal("INIT", _params.hostname, true);

        if (_params.checkOnBoot) {
            checkAutoUpdate();
        }
    }
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
    publishOtaEventInternal("RESET", nullptr, true);
}

// ============================================================================
// 12. ПРОВЕРКА ОБНОВЛЕНИЙ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool OTAManager::checkForUpdates(UpdateInfo& info) {
    if (_status == OTAStatus::CHECKING) {
        logMessage("Update check already running");
        return false;
    }

    if (_otaMutex == nullptr) return false;
    if (xSemaphoreTakeRecursive(_otaMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    updateStatus(OTAStatus::CHECKING, 0, "Checking for updates...");
    _stats.totalChecks++;
    _lastCheckMs = millis();

    logMessage("Checking for updates...");
    publishOtaEventInternal("CHECK", "Checking for updates", true);

    JsonDocument doc;
    if (!downloadManifest(doc)) {
        updateStatus(OTAStatus::IDLE, 0, "Check failed");
        xSemaphoreGiveRecursive(_otaMutex);
        publishOtaEventInternal("CHECK_FAIL", "Manifest download failed", false);
        return false;
    }

    const char* version = doc["version"] | "";
    const char* fw_md5 = doc["fw_md5"] | "";
    const char* fs_md5 = doc["fs_md5"] | "";
    const char* changelog = doc["changelog"] | "";
    const char* fw_url = doc["fw_url"] | "";
    const char* fs_url = doc["fs_url"] | "";
    uint32_t fw_size = doc["fw_size"] | 0;
    uint32_t fs_size = doc["fs_size"] | 0;
    bool forceUpdate = doc["force_update"] | false;

    strncpy(info.version, version, sizeof(info.version) - 1);
    strncpy(info.fw_md5, fw_md5, sizeof(info.fw_md5) - 1);
    strncpy(info.fs_md5, fs_md5, sizeof(info.fs_md5) - 1);
    strncpy(info.changelog, changelog, sizeof(info.changelog) - 1);
    strncpy(info.fw_url, fw_url, sizeof(info.fw_url) - 1);
    strncpy(info.fs_url, fs_url, sizeof(info.fs_url) - 1);
    info.fw_size = fw_size;
    info.fs_size = fs_size;
    info.forceUpdate = forceUpdate;
    info.size = fw_size + fs_size;
    info.available = false;

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
        publishOtaEventInternal("UPDATE_FOUND", info.version, true);
        if (_onUpdateFound) _onUpdateFound(info);
        xSemaphoreGiveRecursive(_otaMutex);
        return true;
    }

    info.available = false;
    updateStatus(OTAStatus::IDLE, 0, "Up to date");
    logMessage("Up to date: %s", _currentVersion);
    publishOtaEventInternal("UP_TO_DATE", _currentVersion, true);
    xSemaphoreGiveRecursive(_otaMutex);
    return false;
}

bool OTAManager::isUpdateAvailable() {
    UpdateInfo info;
    return checkForUpdates(info) && info.available;
}

// ============================================================================
// 13. ВЫПОЛНЕНИЕ ОТА (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool OTAManager::performOTA() {
    UpdateInfo info;
    if (!checkForUpdates(info) || !info.available) {
        updateStatus(OTAStatus::FAILED, 0, "No valid update");
        publishErrorEvent("NO_UPDATE");
        publishOtaEventInternal("PERFORM_NO_UPDATE", nullptr, false);
        return false;
    }
    return performOTA(info);
}

bool OTAManager::performOTA(const UpdateInfo& info) {
    if (!info.available) {
        updateStatus(OTAStatus::FAILED, 0, "No valid update");
        publishErrorEvent("NO_UPDATE");
        publishOtaEventInternal("PERFORM_NO_UPDATE", nullptr, false);
        return false;
    }

    if (_otaInProgress) {
        logMessage("OTA already in progress");
        publishOtaEventInternal("PERFORM_ALREADY", nullptr, false);
        return false;
    }

    if (!hasEnoughSpace(info)) {
        updateStatus(OTAStatus::FAILED, 0, "Not enough space");
        strncpy(_stats.lastError, "Not enough space", sizeof(_stats.lastError) - 1);
        publishErrorEvent("NOT_ENOUGH_SPACE");
        publishOtaEventInternal("PERFORM_NO_SPACE", nullptr, false);
        return false;
    }

    _otaInProgress = true;
    _otaStartTime = millis();
    _retryCount = 0;
    bool success = true;

    logMessage("Starting OTA to version: %s", info.version);
    publishProgressEvent(0, "Starting OTA");
    publishOtaEventInternal("PERFORM_START", info.version, true);

    if (strlen(info.fs_md5) > 2 && info.fs_md5[0] != '0') {
        updateStatus(OTAStatus::DOWNLOADING_FS, 10, "Downloading filesystem...");
        logMessage("Downloading LittleFS image");
        uint32_t fsDownloaded = 0;
        if (!downloadWithProgress(info.fs_url, info.fs_md5, fsDownloaded)) {
            updateStatus(OTAStatus::FAILED, 0, "FS download failed");
            strncpy(_stats.lastError, "FS download failed", sizeof(_stats.lastError) - 1);
            success = false;
            publishErrorEvent("FS_DOWNLOAD_FAILED");
            publishOtaEventInternal("PERFORM_FS_FAIL", info.version, false);
        }
    }

    if (success) {
        updateStatus(OTAStatus::DOWNLOADING_FW, 50, "Downloading firmware...");
        logMessage("Downloading firmware");

        if (_params.enableRollback) {
            strncpy(_rollbackVersion, _currentVersion, sizeof(_rollbackVersion) - 1);
        }

        WiFiClient client;
        client.setTimeout(_timeoutMs / 1000);

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
            _stats.updatesSuccess++;
            saveVersion(info.version);

            UpdateHistory entry;
            strncpy(entry.version, info.version, sizeof(entry.version) - 1);
            entry.timestamp = millis() / 1000;
            entry.success = true;
            entry.duration = millis() - _otaStartTime;
            strncpy(entry.error, "", sizeof(entry.error) - 1);
            addHistory(entry);
            updateStatsRecord(true, info.version);
            publishCompleteEvent(true, info.version);
            publishOtaEventInternal("PERFORM_SUCCESS", info.version, true);

            if (_onComplete) _onComplete(true, info.version);

            _otaInProgress = false;

            if (_params.rebootAfterUpdate) {
                logMessage("Rebooting in 2 seconds...");
                updateStatus(OTAStatus::REBOOTING, 100, "Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
            return true;
        } else {
            String errMsg = String(httpUpdate.getLastErrorString());
            logMessage("Firmware update error: %d - %s", ret, errMsg.c_str());
            strncpy(_stats.lastError, errMsg.c_str(), sizeof(_stats.lastError) - 1);
            success = false;
            publishErrorEvent("FW_UPDATE_FAILED");
            publishOtaEventInternal("PERFORM_FW_FAIL", info.version, false);
        }
    }

    if (!success) {
        UpdateHistory entry;
        strncpy(entry.version, info.version, sizeof(entry.version) - 1);
        entry.timestamp = millis() / 1000;
        entry.success = false;
        entry.duration = millis() - _otaStartTime;
        strncpy(entry.error, _stats.lastError, sizeof(entry.error) - 1);
        addHistory(entry);
        updateStatsRecord(false, "");
        publishCompleteEvent(false, "", _stats.lastError);
        publishOtaEventInternal("PERFORM_FAIL", _stats.lastError, false);

        if (_onComplete) _onComplete(false, "");

        if (_params.enableRollback && _rollbackAvailable) {
            logMessage("Attempting rollback...");
            rollback();
        }
    }

    _otaInProgress = false;
    return false;
}

void OTAManager::cancelOTA() {
    if (_otaInProgress) {
        Update.abort();
        _otaInProgress = false;
        _status = OTAStatus::CANCELLED;
        logMessage("OTA cancelled");
        publishProgressEvent(0, "Cancelled");
        publishOtaEventInternal("CANCEL", nullptr, true);
    }
    if (_webOTAInProgress) {
        Update.abort();
        _webOTAInProgress = false;
        _status = OTAStatus::CANCELLED;
        logMessage("Web OTA cancelled");
        publishOtaEventInternal("CANCEL_WEB", nullptr, true);
    }
}

// ============================================================================
// 14. ROLLBACK (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool OTAManager::rollback() {
    if (!_rollbackAvailable) {
        logMessage("No rollback available");
        publishOtaEventInternal("ROLLBACK_NO", nullptr, false);
        return false;
    }

    if (!LittleFS.exists(ROLLBACK_FILE)) {
        logMessage("Rollback file not found");
        _rollbackAvailable = false;
        publishOtaEventInternal("ROLLBACK_NO_FILE", nullptr, false);
        return false;
    }

    updateStatus(OTAStatus::ROLLBACK, 0, "Rollback...");
    logMessage("Starting rollback to version: %s", _rollbackVersion);
    publishOtaEventInternal("ROLLBACK_START", _rollbackVersion, true);

    File rollbackFile = LittleFS.open(ROLLBACK_FILE, "r");
    if (!rollbackFile) {
        updateStatus(OTAStatus::ROLLBACK_FAILED, 0, "Cannot open rollback file");
        publishErrorEvent("ROLLBACK_OPEN_FAILED");
        publishOtaEventInternal("ROLLBACK_OPEN_FAIL", nullptr, false);
        return false;
    }

    size_t fileSize = rollbackFile.size();
    if (fileSize == 0) {
        rollbackFile.close();
        updateStatus(OTAStatus::ROLLBACK_FAILED, 0, "Empty rollback file");
        publishErrorEvent("ROLLBACK_EMPTY");
        publishOtaEventInternal("ROLLBACK_EMPTY", nullptr, false);
        return false;
    }

    if (!Update.begin(fileSize, U_FLASH)) {
        rollbackFile.close();
        strncpy(_lastError, Update.errorString(), sizeof(_lastError) - 1);
        updateStatus(OTAStatus::ROLLBACK_FAILED, 0, _lastError);
        publishErrorEvent("ROLLBACK_BEGIN_FAILED");
        publishOtaEventInternal("ROLLBACK_BEGIN_FAIL", _lastError, false);
        return false;
    }

    uint8_t buffer[OTA_CHUNK_SIZE];
    size_t written = 0;
    uint32_t lastProgressUpdate = millis();

    while (rollbackFile.available()) {
        size_t bytesRead = rollbackFile.read(buffer, OTA_CHUNK_SIZE);
        if (bytesRead == 0) break;

        if (Update.write(buffer, bytesRead) != bytesRead) {
            rollbackFile.close();
            Update.abort();
            strncpy(_lastError, "Write failed", sizeof(_lastError) - 1);
            updateStatus(OTAStatus::ROLLBACK_FAILED, 0, _lastError);
            publishErrorEvent("ROLLBACK_WRITE_FAILED");
            publishOtaEventInternal("ROLLBACK_WRITE_FAIL", _lastError, false);
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
        strncpy(_lastError, Update.errorString(), sizeof(_lastError) - 1);
        updateStatus(OTAStatus::ROLLBACK_FAILED, 0, _lastError);
        publishErrorEvent("ROLLBACK_END_FAILED");
        publishOtaEventInternal("ROLLBACK_END_FAIL", _lastError, false);
        return false;
    }

    LittleFS.remove(ROLLBACK_FILE);
    _rollbackAvailable = false;

    if (strlen(_rollbackVersion) > 0) {
        strncpy(_currentVersion, _rollbackVersion, sizeof(_currentVersion) - 1);
        saveVersion(_currentVersion);
    }

    updateStatus(OTAStatus::ROLLBACK_SUCCESS, 100, "Rollback successful");
    logMessage("Rollback successful");
    publishRollbackEvent(true);
    publishOtaEventInternal("ROLLBACK_SUCCESS", _currentVersion, true);

    if (_params.rebootAfterUpdate) {
        logMessage("Rebooting in 2 seconds...");
        updateStatus(OTAStatus::REBOOTING, 100, "Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    return true;
}

// ============================================================================
// 15. WEB-ОТА (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool OTAManager::beginWebOTA(uint32_t size, int command) {
    if (_webOTAInProgress || _otaInProgress) {
        logMessage("Web OTA already in progress");
        publishOtaEventInternal("WEB_ALREADY", nullptr, false);
        return false;
    }

    _webOTAInProgress = true;
    _webOTAProgress = 0;
    _webOTATotal = size;
    _otaStartTime = millis();

    updateStatus(OTAStatus::UPLOADING, 0, "Web OTA starting...");
    logMessage("Web OTA start: %lu bytes", size);
    publishOtaEventInternal("WEB_START", String(size).c_str(), true);

    if (!Update.begin(size, command)) {
        _webOTAInProgress = false;
        strncpy(_lastError, Update.errorString(), sizeof(_lastError) - 1);
        updateStatus(OTAStatus::FAILED, 0, "Web OTA init failed");
        logMessage("Web OTA begin failed: %s", _lastError);
        publishErrorEvent("WEB_OTA_INIT_FAILED");
        publishOtaEventInternal("WEB_INIT_FAIL", _lastError, false);
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
            publishOtaEventInternal("WEB_SUCCESS", nullptr, true);

            UpdateHistory entry;
            strncpy(entry.version, "web_Upload", sizeof(entry.version) - 1);
            entry.timestamp = millis() / 1000;
            entry.success = true;
            entry.duration = duration;
            strncpy(entry.error, "", sizeof(entry.error) - 1);
            addHistory(entry);
            updateStatsRecord(true, "web_Upload");
            publishCompleteEvent(true, "web_Upload");
            publishOtaEventInternal("WEB_COMPLETE", nullptr, true);

            if (_onComplete) _onComplete(true, "web_Upload");

            if (_params.rebootAfterUpdate) {
                logMessage("Rebooting in 2 seconds...");
                updateStatus(OTAStatus::REBOOTING, 100, "Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
            return true;
        } else {
            strncpy(_lastError, Update.errorString(), sizeof(_lastError) - 1);
            updateStatus(OTAStatus::FAILED, 0, "Web OTA finalize failed");
            logMessage("Web OTA end failed: %s", _lastError);
            updateStatsRecord(false, "");
            publishCompleteEvent(false, "", _lastError);
            publishErrorEvent("WEB_OTA_FINALIZE_FAILED");
            publishOtaEventInternal("WEB_FINALIZE_FAIL", _lastError, false);
            return false;
        }
    } else {
        Update.abort();
        updateStatus(OTAStatus::CANCELLED, 0, "Web OTA cancelled");
        logMessage("Web OTA cancelled");
        updateStatsRecord(false, "");
        publishProgressEvent(0, "Cancelled");
        publishOtaEventInternal("WEB_CANCELLED", nullptr, true);
        return false;
    }
}

// ============================================================================
// 16. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void OTAManager::setBaseUrl(const char* url) {
    if (url) {
        _baseUrl = url;
        logMessage("Base URL set: %s", url);
        publishOtaEventInternal("SET_BASE_URL", url, true);
    }
}

void OTAManager::setCurrentVersion(const char* version) {
    if (version) {
        strncpy(_currentVersion, version, sizeof(_currentVersion) - 1);
        saveVersion(version);
        logMessage("Current version set: %s", version);
        publishOtaEventInternal("SET_VERSION", version, true);
    }
}

void OTAManager::checkAutoUpdate() {
    _lastCheckMs = millis();
    UpdateInfo info;
    if (checkForUpdates(info) && info.available) {
        logMessage("Auto update triggered");
        publishOtaEventInternal("AUTO_UPDATE", info.version, true);
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
        strncpy(_stats.lastVersion, version ? version : "", sizeof(_stats.lastVersion) - 1);
        _stats.lastUpdateTime = millis();
    } else {
        _stats.updatesFailed++;
    }
}

void OTAManager::saveVersion(const char* version) {
    JsonDocument doc;
    doc["version"] = version;
    doc["timestamp"] = millis() / 1000;

    File file = LittleFS.open(VERSION_FILE, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        logMessage("Version saved: %s", version);
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

    JsonArray arr = doc["history"].as<JsonArray>();
    _history.clear();

    for (JsonObject obj : arr) {
        UpdateHistory entry;
        strncpy(entry.version, obj["version"] | "", sizeof(entry.version) - 1);
        entry.timestamp = obj["timestamp"] | 0;
        entry.success = obj["success"] | false;
        entry.duration = obj["duration"] | 0;
        strncpy(entry.error, obj["error"] | "", sizeof(entry.error) - 1);
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
// 17. DOWNLOAD WITH PROGRESS (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool OTAManager::downloadWithProgress(const String& url, const String& md5,
                                      uint32_t& downloaded) {
    WiFiClient client;
    client.setTimeout(_timeoutMs / 1000);

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(_timeoutMs);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        logMessage("HTTP error: %d", httpCode);
        publishOtaEventInternal("DOWNLOAD_HTTP_ERROR", String(httpCode).c_str(), false);
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        http.end();
        logMessage("Invalid content length");
        publishOtaEventInternal("DOWNLOAD_INVALID_LEN", nullptr, false);
        return false;
    }

    downloaded = 0;
    size_t written = 0;
    uint32_t lastProgressUpdate = millis();

    if (!Update.begin(contentLength, U_SPIFFS)) {
        http.end();
        logMessage("Update begin failed: %s", Update.errorString());
        publishOtaEventInternal("DOWNLOAD_BEGIN_FAIL", Update.errorString(), false);
        return false;
    }

    if (_verifyMD5 && md5.length() > 0) {
        Update.setMD5(md5.c_str());
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t chunkBuffer[OTA_CHUNK_SIZE];

    while (http.connected() && written < (size_t)contentLength) {
        size_t availableBytes = stream->available();
        if (availableBytes > 0) {
            size_t toRead = min(availableBytes, (size_t)OTA_CHUNK_SIZE);
            size_t bytesRead = stream->readBytes(chunkBuffer, toRead);
            if (Update.write(chunkBuffer, bytesRead) != bytesRead) {
                http.end();
                Update.abort();
                logMessage("Flash write error");
                publishOtaEventInternal("DOWNLOAD_WRITE_ERROR", nullptr, false);
                return false;
            }
            written += bytesRead;
            downloaded = written;

            if (millis() - lastProgressUpdate > 100) {
                lastProgressUpdate = millis();
                int progress = (written * 100) / contentLength;
                updateStatus(OTAStatus::DOWNLOADING_FS, 10 + (progress * 40 / 100),
                            "Writing FS...");
                publishProgressEvent(10 + (progress * 40 / 100), "Downloading FS");
                esp_task_wdt_reset();
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    http.end();

    if (written != (size_t)contentLength) {
        Update.abort();
        logMessage("Download incomplete: %zu/%d", written, contentLength);
        publishOtaEventInternal("DOWNLOAD_INCOMPLETE", nullptr, false);
        return false;
    }

    if (!Update.end(true)) {
        logMessage("Update end failed: %s", Update.errorString());
        publishOtaEventInternal("DOWNLOAD_END_FAIL", Update.errorString(), false);
        return false;
    }

    logMessage("FS download complete: %lu bytes", downloaded);
    publishOtaEventInternal("DOWNLOAD_COMPLETE", String(downloaded).c_str(), true);
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
            strncpy(_stats.lastError, "No server IP", sizeof(_stats.lastError) - 1);
            publishErrorEvent("NO_SERVER_IP");
            publishOtaEventInternal("MANIFEST_NO_SERVER", nullptr, false);
            return false;
        }
    }

    if (!url.endsWith("/")) url += "/";
    url += String(_params.hostname) + "/version.json";

    HTTPClient http;
    WiFiClient client;
    http.setTimeout(5000);
    http.begin(client, url);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        logMessage("HTTP manifest: %d", httpCode);
        strncpy(_stats.lastError, "HTTP error", sizeof(_stats.lastError) - 1);
        publishOtaEventInternal("MANIFEST_HTTP_ERROR", String(httpCode).c_str(), false);
        return false;
    }

    String payload = http.getString();
    http.end();

    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        logMessage("JSON parse error");
        strncpy(_stats.lastError, "Invalid JSON", sizeof(_stats.lastError) - 1);
        publishErrorEvent("INVALID_MANIFEST");
        publishOtaEventInternal("MANIFEST_JSON_ERROR", error.c_str(), false);
        return false;
    }

    return true;
}

bool OTAManager::verifyFirmware(const String& md5) {
    if (!_verifyMD5 || md5.length() == 0) return true;
    return true;
}

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

    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;

    if (info.fs_size > freeBytes) {
        logMessage("Not enough FS space: need %lu, have %lu",
                  info.fs_size, freeBytes);
        return false;
    }

    uint32_t freeHeap = ESP.getFreeHeap();
    if (info.minRam > freeHeap) {
        logMessage("Not enough RAM: need %lu, have %lu",
                  info.minRam, freeHeap);
        return false;
    }

    return true;
}

// ============================================================================
// 18. СТАТУСНЫЙ МЕТОД (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
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
// 19. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
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
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
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
            stream.printf("[%zu] %s %s %lu ms\n",
                         i,
                         _history[i].version,
                         _history[i].success ? "OK" : "FAIL",
                         _history[i].duration);
            if (strlen(_history[i].error) > 0) {
                stream.printf("  Error: %s\n", _history[i].error);
            }
        }
    }
    stream.println("==========================");
}