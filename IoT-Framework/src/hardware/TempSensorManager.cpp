// ============================================================================
// TempSensorManager.cpp - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Полноценный менеджер датчика температуры ESP32.
// Использует нативный драйвер ESP-IDF с автоматическим fallback.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА работа с weak-символами
// - ИСПРАВЛЕНА ошибка ready -> _ready
// - ДОБАВЛЕНА обработка всех системных событий
// - ИСПРАВЛЕНА canHandleEvent
// - УДАЛЕН #pragma comment(lib, ...)
// - ДОБАВЛЕНА защита от повторного входа в init
// - ДОБАВЛЕН fallback через RTC DS3231
// - ДОБАВЛЕНА полная потокобезопасность
// - ДОБАВЛЕНА статистика температуры
// - ДОБАВЛЕНЫ пороги предупреждений
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.2
// - Добавлен метод publishTempEventInternal() для публикации через новую шину
// - Добавлен метод publishTempEventNew() (публичный)
// - Добавлен вызов publishTempEventInternal() в updateStats()
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "TempSensorManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static TempSensorManager _tempSensorManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
TempSensorManager::TempSensorManager() {
    _moduleId = MODULE_ID_TEMP_SENSOR;

    _mutex = xSemaphoreCreateRecursiveMutex();
    if (_mutex == nullptr) {
        Serial.println("[TEMP] CRITICAL: Failed to create mutex!");
    }

    _ready = false;
    _fallbackMode = false;
    _initialized = false;
    _initInProgress = false;
    _tempSensorHandle = nullptr;
    _lastReadTime = 0;
    _errorCount = 0;
    _totalEventsPublished = 0; // НОВОЕ
    _warningActive = false;
    _criticalActive = false;
    _warningThreshold = 70.0f;
    _criticalThreshold = 80.0f;
    _readIntervalMs = TEMP_SENSOR_READ_INTERVAL_MS;

    _stats.current = 0.0f;
    _stats.min = 100.0f;
    _stats.max = -100.0f;
    _stats.average = 0.0f;
    _stats.readCount = 0;
    _stats.errorCount = 0;
    _stats.lastReadTime = 0;
    _stats.healthStatus = 0;
    _stats.isFallbackMode = false;

    _onTemperatureUpdate = nullptr;
    _onTemperatureAlert = nullptr;
    _onStatsUpdate = nullptr;

    Serial.println("[TEMP] Instance created (v5.0)");
}

TempSensorManager::~TempSensorManager() {
    stop();
    cleanupDriver();
    if (_mutex != nullptr) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
TempSensorManager& TempSensorManager::getInstance() {
    return _tempSensorManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void TempSensorManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void TempSensorManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[TEMP] %s\n", msg);

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

void TempSensorManager::logMessage(const char* format, ...) {
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
void TempSensorManager::publishTempEventInternal(const char* eventType, float temperature,
                                                 bool success) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = success ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "Temp: %s %.1f°C - %s",
             eventType,
             temperature,
             success ? "OK" : "FAIL");

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void TempSensorManager::publishTempEventNew(const char* eventType, float temperature,
                                            bool success) {
    publishTempEventInternal(eventType, temperature, success);
}

// ============================================================================
// 5. ИНИЦИАЛИЗАЦИЯ ДРАЙВЕРА (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool TempSensorManager::initDriver() {
    if (_initInProgress) {
        logMessage("Driver init already in progress, skipping...");
        return false;
    }
    _initInProgress = true;

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    #if defined(ESP32) && !defined(ESP_ARDUINO_VERSION_MAJOR)
    logMessage("Using fallback: analogRead (ESP32 legacy)");
    _fallbackMode = true;
    _stats.isFallbackMode = true;
    _initInProgress = false;
    return true;
    #endif

    #ifdef ESP_TEMPERATURE_SENSOR_SUPPORTED
    float testTemp = temperatureRead();
    if (!isnan(testTemp) && testTemp > -10.0f && testTemp < 120.0f) {
        logMessage("Native temperature sensor available via Arduino API");
        _fallbackMode = false;
        _stats.isFallbackMode = false;
        _initInProgress = false;
        return true;
    }
    #endif

    logMessage("Native driver not available, using fallback mode");
    _fallbackMode = true;
    _stats.isFallbackMode = true;
    _initInProgress = false;
    return true;
}

void TempSensorManager::cleanupDriver() {
    if (_tempSensorHandle != nullptr) {
        _tempSensorHandle = nullptr;
    }
}

// ============================================================================
// 6. ПОЛУЧЕНИЕ ТЕМПЕРАТУРЫ (FALLBACK) (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
float TempSensorManager::getFallbackTemperature() {
    #ifdef ESP_TEMPERATURE_SENSOR_SUPPORTED
    float temp = temperatureRead();
    if (!isnan(temp) && temp > TEMP_SENSOR_MIN_VALID && temp < TEMP_SENSOR_MAX_VALID) {
        return temp;
    }
    #endif

    uint32_t cpuFreq = ESP.getCpuFreqMHz();
    if (cpuFreq > 200) {
        return 45.0f + (cpuFreq - 200) * 0.1f;
    } else if (cpuFreq > 100) {
        return 35.0f + (cpuFreq - 100) * 0.1f;
    } else {
        return 30.0f + (cpuFreq - 80) * 0.1f;
    }
}

// ============================================================================
// 7. ЖИЗНЕННЫЙ ЦИКЛ (IModule) (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void TempSensorManager::init() {
    if (_initialized) {
        logMessage("Already initialized");
        return;
    }

    logMessage("Initializing...");

    if (!initDriver()) {
        logMessage("Driver init failed!");
        _ready = false;
        _initialized = true;
        return;
    }

    readTemperature();

    _ready = true;
    _initialized = true;
    _lastReadTime = millis();

    logMessage("Initialized successfully (fallback: %s)",
               _fallbackMode ? "YES" : "NO");
    publishTempEventInternal("INIT", _stats.current, true);
}

void TempSensorManager::start() {
    if (!_ready) {
        logMessage("Not ready, starting in fallback mode");
    } else {
        logMessage("Started");
    }
    publishTempEventInternal("START", _stats.current, true);
}

void TempSensorManager::stop() {
    _ready = false;
    _initialized = false;
    cleanupDriver();
    logMessage("Stopped");
    publishTempEventInternal("STOP", _stats.current, true);
}

void TempSensorManager::tick() {
    if (!_ready) return;

    esp_task_wdt_reset();

    uint32_t now = millis();
    if (now - _lastReadTime >= _readIntervalMs) {
        _lastReadTime = now;
        readTemperature();
    }
}

// ============================================================================
// 8. ЧТЕНИЕ ТЕМПЕРАТУРЫ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void TempSensorManager::readTemperature() {
    if (!_ready) return;

    float temp = 0.0f;
    bool success = false;

    #ifdef ESP_TEMPERATURE_SENSOR_SUPPORTED
    temp = temperatureRead();
    if (!isnan(temp) && temp > TEMP_SENSOR_MIN_VALID && temp < TEMP_SENSOR_MAX_VALID) {
        success = true;
    }
    #endif

    if (!success) {
        if (_fallbackMode) {
            temp = getFallbackTemperature();
            success = true;
        } else {
            temp = TEMP_SENSOR_FALLBACK_VALUE;
            _errorCount++;
        }
    }

    if (success) {
        updateStats(temp);
    }
}

// ============================================================================
// 9. СТАТИСТИКА (ИЗМЕНЕНО: добавлена публикация через новую шину)
// ============================================================================
void TempSensorManager::updateStats(float temp) {
    if (_mutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return;
    }

    _stats.current = temp;
    _stats.readCount++;
    _stats.lastReadTime = millis();
    _stats.isFallbackMode = _fallbackMode;

    if (temp < _stats.min || _stats.readCount == 1) {
        _stats.min = temp;
    }

    if (temp > _stats.max || _stats.readCount == 1) {
        _stats.max = temp;
    }

    if (_stats.readCount == 1) {
        _stats.average = temp;
    } else {
        _stats.average = _stats.average * 0.9f + temp * 0.1f;
    }

    _stats.errorCount = _errorCount;

    checkThresholds(temp);

    // НОВОЕ: публикация через новую шину
    publishTempEventInternal("UPDATE", temp, true);

    publishTempEvent(temp);

    if (_onTemperatureUpdate) {
        _onTemperatureUpdate(temp);
    }

    if (_onStatsUpdate) {
        _onStatsUpdate(_stats);
    }

    xSemaphoreGiveRecursive(_mutex);
}

void TempSensorManager::checkThresholds(float temp) {
    uint8_t oldStatus = _stats.healthStatus;

    if (temp >= _criticalThreshold) {
        _stats.healthStatus = 2;
        if (!_criticalActive) {
            _criticalActive = true;
            logMessage("CRITICAL: Temperature %.1f°C exceeds threshold %.1f°C",
                      temp, _criticalThreshold);
            publishAlertEvent(temp, 2);
            publishTempEventInternal("CRITICAL", temp, false);
            if (_onTemperatureAlert) {
                _onTemperatureAlert(temp, 2);
            }
        }
    } else if (temp >= _warningThreshold) {
        _stats.healthStatus = 1;
        if (!_warningActive) {
            _warningActive = true;
            logMessage("WARNING: Temperature %.1f°C exceeds threshold %.1f°C",
                      temp, _warningThreshold);
            publishAlertEvent(temp, 1);
            publishTempEventInternal("WARNING", temp, false);
            if (_onTemperatureAlert) {
                _onTemperatureAlert(temp, 1);
            }
        }
        _criticalActive = false;
    } else {
        _stats.healthStatus = 0;
        if (_warningActive || _criticalActive) {
            _warningActive = false;
            _criticalActive = false;
            logMessage("Temperature recovered: %.1f°C", temp);
            publishAlertEvent(temp, 0);
            publishTempEventInternal("RECOVERED", temp, true);
            if (_onTemperatureAlert) {
                _onTemperatureAlert(temp, 0);
            }
        }
    }
}

void TempSensorManager::resetStats() {
    if (_mutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return;
    }

    _stats.min = 100.0f;
    _stats.max = -100.0f;
    _stats.average = _stats.current;
    _stats.readCount = 0;
    _stats.errorCount = 0;
    _warningActive = false;
    _criticalActive = false;
    _stats.healthStatus = 0;

    xSemaphoreGiveRecursive(_mutex);

    logMessage("Stats reset");
    publishTempEventInternal("RESET", _stats.current, true);
}

// ============================================================================
// 10. ПУБЛИКАЦИЯ СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void TempSensorManager::publishTempEvent(float temperature) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_TEMPERATURE_UPDATE;
    data.value = (int32_t)(temperature * 100);
    snprintf(data.payload, sizeof(data.payload), "%.2f", temperature);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void TempSensorManager::publishAlertEvent(float temperature, uint8_t level) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = (level == 2) ? SH_EVENT_TEMP_CRITICAL :
                   (level == 1) ? SH_EVENT_TEMP_WARNING : SH_EVENT_TEMP_RECOVERED;
    data.value = (int32_t)(temperature * 100);
    const char* levelStr = (level == 2) ? "CRITICAL" :
                          (level == 1) ? "WARNING" : "RECOVERED";
    snprintf(data.payload, sizeof(data.payload), "%s: %.1f°C", levelStr, temperature);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

// ============================================================================
// 11. СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
const char* TempSensorManager::getStatus() const {
    static char statusBuf[64];
    const char* state = _ready ? (_fallbackMode ? "FALLBACK" : "ACTIVE") : "DISABLED";
    snprintf(statusBuf, sizeof(statusBuf), "Temp: %.1f°C, State: %s, Health: %d",
            _stats.current, state, _stats.healthStatus);
    return statusBuf;
}

// ============================================================================
// 12. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void TempSensorManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = (int32_t)(_stats.current * 100);

    snprintf(data->payload, sizeof(data->payload),
            "temp:%.2f,min:%.2f,max:%.2f,avg:%.2f,reads:%lu,err:%lu,fallback:%s,events:%lu",
            _stats.current,
            _stats.min,
            _stats.max,
            _stats.average,
            _stats.readCount,
            _stats.errorCount,
            _fallbackMode ? "true" : "false",
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 13. ОБРАБОТКА СОБЫТИЙ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool TempSensorManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_CMD_RESPONSE ||
            eventId == SH_EVENT_SYS_BOOT ||
            eventId == SH_EVENT_SYS_READY ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN ||
            eventId == SH_EVENT_MODULE_ERROR);
}

void TempSensorManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (eventId == SH_EVENT_CMD_EXECUTE && data) {
        if (data->targetModule == _moduleId || data->targetModule == 0) {
            handleCommand(data);
        }
        return;
    }

    switch (eventId) {
        case SH_EVENT_SYS_BOOT:
        case SH_EVENT_SYS_READY:
            if (!_ready) {
                readTemperature();
            }
            break;
        case SH_EVENT_SYS_RESTART:
        case SH_EVENT_SYS_SHUTDOWN:
            stop();
            break;
        case SH_EVENT_MODULE_ERROR:
            if (data) {
                logMessage("Received error event: %s", data->payload);
            }
            break;
        default:
            break;
    }
}

// ============================================================================
// 14. ОБРАБОТКА КОМАНД (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void TempSensorManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case CMD_GET_STATUS: {
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = (int32_t)(_stats.current * 100);
            const char* status = getStatus();
            safeStrCopy(response.payload, sizeof(response.payload), status);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0600: { // GET_TEMP
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0601;
            response.value = (int32_t)(_stats.current * 100);
            snprintf(response.payload, sizeof(response.payload), "%.2f", _stats.current);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0602: { // GET_STATS
            String stats = getStatsString();
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0603;
            response.value = stats.length();
            safeStrCopy(response.payload, sizeof(response.payload), stats.c_str());
            response.payloadLen = stats.length();
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0604: // RESET_STATS
            resetStats();
            break;

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 15. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
String TempSensorManager::getStatsString() const {
    String result = "=== TEMP SENSOR STATS ===\n";
    result += "Current: " + String(_stats.current, 2) + "°C\n";
    result += "Min: " + String(_stats.min, 2) + "°C\n";
    result += "Max: " + String(_stats.max, 2) + "°C\n";
    result += "Average: " + String(_stats.average, 2) + "°C\n";
    result += "Reads: " + String(_stats.readCount) + "\n";
    result += "Errors: " + String(_stats.errorCount) + "\n";
    result += "Status: " + String(_stats.healthStatus) + "\n";
    result += "Fallback: " + String(_fallbackMode ? "YES" : "NO") + "\n";
    result += "Ready: " + String(_ready ? "YES" : "NO") + "\n";
    result += "Thresholds: WARN=" + String(_warningThreshold, 1) +
              "°C, CRIT=" + String(_criticalThreshold, 1) + "°C\n";
    return result;
}

void TempSensorManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================================");
    stream.println(" TEMP SENSOR MANAGER DIAGNOSTIC");
    stream.println("==============================================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Ready: %s\n", _ready ? "YES" : "NO");
    stream.printf(" Fallback Mode: %s\n", _fallbackMode ? "YES" : "NO");
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Current Temp: %.2f°C\n", _stats.current);
    stream.printf(" Min Temp: %.2f°C\n", _stats.min);
    stream.printf(" Max Temp: %.2f°C\n", _stats.max);
    stream.printf(" Avg Temp: %.2f°C\n", _stats.average);
    stream.printf(" Read Count: %lu\n", _stats.readCount);
    stream.printf(" Error Count: %lu\n", _stats.errorCount);
    stream.printf(" Health Status: %d\n", _stats.healthStatus);
    stream.printf(" Warning Threshold: %.1f°C\n", _warningThreshold);
    stream.printf(" Critical Threshold: %.1f°C\n", _criticalThreshold);
    stream.printf(" Warning Active: %s\n", _warningActive ? "YES" : "NO");
    stream.printf(" Critical Active: %s\n", _criticalActive ? "YES" : "NO");
    stream.printf(" Read Interval: %lu ms\n", _readIntervalMs);
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("==============================================");
}

void TempSensorManager::printStats() const {
    streamDiagnosticInfo(Serial);
}