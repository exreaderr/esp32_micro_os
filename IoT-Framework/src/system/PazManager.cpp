// ============================================================================
// PazManager.cpp - ULTIMATE MICRO-OS V5.0 (FULLY INTEGRATED)
// ============================================================================
// Описание: Противоаварийная защита (ПАЗ) - мониторинг и управление
// критическими параметрами системы.
//
// ИЗМЕНЕНИЯ v5.0:
// - Сохранена 100% функциональность v4.2.2
// - Добавлен метод publishPazEvent() для публикации через новую шину
// - Добавлены вызовы publishPazEvent() в triggerPanic, updateStatus, addLog
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика
// ============================================================================
#include "PazManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include <ETH.h>
#include <LittleFS.h>
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ СПИНЛОК
// ============================================================================
portMUX_TYPE PazManager::_pazIsrMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР
// ============================================================================
static PazManager _pazManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
PazManager::PazManager() {
    _moduleId = MODULE_ID_PAZ;
    _startTimeMs = millis();

    _pazMutex = xSemaphoreCreateRecursiveMutex();
    if (_pazMutex == nullptr) {
        Serial.println("[PAZ] CRITICAL: Failed to create mutex!");
    } else {
        if (xSemaphoreTakeRecursive(_pazMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            _panicHistory.reserve(PAZ_MAX_PANIC_HISTORY);
            _logEntries.reserve(PAZ_MAX_LOG_ENTRIES);
            xSemaphoreGiveRecursive(_pazMutex);
        }
    }

    _panicCb = nullptr;
    _statusCb = nullptr;
    _logCb = nullptr;
    _pendingPingHandle = nullptr;
    _pingDone = false;
    _initInProgress = false;
    _totalEventsPublished = 0; // НОВОЕ

    Serial.println("[PAZ] Instance created (v5.0)");
}

PazManager::~PazManager() {
    stop();
    if (_pazMutex != nullptr) {
        vSemaphoreDelete(_pazMutex);
        _pazMutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
PazManager& PazManager::getInstance() {
    return _pazManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void PazManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void PazManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[PAZ] %s\n", msg);

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

void PazManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

bool PazManager::isInitializedAndReady() const {
    return _initialized;
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void PazManager::init() {
    logMessage("Init pending - call begin() with parameters");
}

void PazManager::start() {
    logMessage("Started");
}

void PazManager::stop() {
    _initialized = false;
    if (_wdtEnabled) {
        esp_task_wdt_delete(NULL);
        _wdtEnabled = false;
    }
    if (_pendingPingHandle != nullptr) {
        esp_ping_delete_session(_pendingPingHandle);
        _pendingPingHandle = nullptr;
    }
    logMessage("Stopped");
}

void PazManager::tick() {
    if (!isInitializedAndReady()) return;

    resetWdt();

    if (_pingDone && _pendingPingHandle != nullptr) {
        esp_ping_delete_session(_pendingPingHandle);
        _pendingPingHandle = nullptr;
        _pingDone = false;
    }

    if (_panicTripped) {
        if (_actuatorPin != 255) {
            digitalWrite(_actuatorPin, !_actuatorActiveState);
        }
        if (_autoRecovery) {
            checkRecovery();
        }
        return;
    }

    uint32_t currentMs = millis();
    if (currentMs - _lastPazTicksMs < _pazTickIntervalMs) return;
    _lastPazTicksMs = currentMs;

    if (currentMs - _lastTempCheckMs >= _tempCheckIntervalMs) {
        _lastTempCheckMs = currentMs;
        checkCpuTemperature();
    }

    runActuatorProtection();
    runSensorProtection();

    if (currentMs - _lastMemoryCheckMs >= _memoryCheckIntervalMs) {
        _lastMemoryCheckMs = currentMs;
        runMemoryAndFsProtection();
    }

    if (currentMs - _lastI2cCheckMs >= _i2cCheckIntervalMs) {
        _lastI2cCheckMs = currentMs;
        runI2cRecovery();
    }

    if (currentMs - _lastPingCheckMs >= _pingCheckIntervalMs) {
        _lastPingCheckMs = currentMs;
        runNetworkDiagnostics();
    }
}

// ============================================================================
// 5. ОБРАБОТКА СОБЫТИЙ
// ============================================================================
void PazManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                              int32_t id, void* eventData) {
    PazManager* instance = static_cast<PazManager*>(handlerArgs);
    if (!instance || !instance->isInitializedAndReady()) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
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

void PazManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    switch (eventId) {
        case SH_EVENT_CMD_EXECUTE:
            if (data->targetModule == _moduleId || data->targetModule == 0) {
                handleCommand(data);
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

bool PazManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN);
}

// ============================================================================
// 6. СТАТУС И ДИАГНОСТИКА
// ============================================================================
const char* PazManager::getStatus() const {
    static char statusBuffer[128];
    snprintf(statusBuffer, sizeof(statusBuffer),
            "Status:%s, Panic:%s, Net:%s, Code:%s",
            getStatusString(),
            _panicTripped ? "YES" : "NO",
            isNetworkHealthy() ? "UP" : "DOWN",
            getCodeString(_lastCode));
    return statusBuffer;
}

// ============================================================================
// 7. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void PazManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = (uint8_t)_currentStatus;

    snprintf(data->payload, sizeof(data->payload),
            "status:%d,panic:%d,net:%d,code:%d,count:%lu,events:%lu",
            (uint8_t)_currentStatus,
            _panicTripped ? 1 : 0,
            isNetworkHealthy() ? 1 : 0,
            (uint8_t)_lastCode,
            _panicCount,
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 8. ОБРАБОТКА КОМАНД
// ============================================================================
void PazManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case 0x0A00: { // GET_STATUS
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0A01;
            response.value = (uint8_t)_currentStatus;
            snprintf(response.payload, sizeof(response.payload),
                    "status:%d,panic:%d,net:%d,code:%d",
                    (uint8_t)_currentStatus,
                    _panicTripped ? 1 : 0,
                    isNetworkHealthy() ? 1 : 0,
                    (uint8_t)_lastCode);
            response.payload[sizeof(response.payload) - 1] = '\0';
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0A02: // RESET_PAZ
            reset();
            break;

        case 0x0A03: // FORCE_RESET
            forceReset();
            break;

        case 0x0A04: // CLEAR_PANIC
            if (_panicTripped) {
                _panicTripped = false;
                publishStatusEvent(_currentStatus, _lastCode);
                logMessage("Panic cleared by command");
            }
            break;

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 9. ОТПРАВКА СОБЫТИЙ
// ============================================================================
void PazManager::publishStatusEvent(PazStatus status, PazDiagnosticCode code) {
    PazStatusEvent event;
    memset(&event, 0, sizeof(PazStatusEvent));
    event.status = (uint8_t)status;
    event.code = (uint8_t)code;
    event.timestamp = millis();
    safeStrCopy(event.message, sizeof(event.message), getStatusString());
    safeStrCopy(event.details, sizeof(event.details), getCodeString(code));
    event.panicActive = _panicTripped;
    event.panicCount = _panicCount;

    int32_t eventId;
    switch (status) {
        case PazStatus::PANIC: eventId = SH_EVENT_PAZ_PANIC; break;
        case PazStatus::CRITICAL: eventId = SH_EVENT_PAZ_CRITICAL; break;
        case PazStatus::ERROR: eventId = SH_EVENT_PAZ_ERROR; break;
        case PazStatus::WARNING: eventId = SH_EVENT_PAZ_WARNING; break;
        default: eventId = SH_EVENT_PAZ_STATUS_CHANGED; break;
    }

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = eventId;
    data.value = (uint8_t)status;
    memcpy(data.payload, &event, min(sizeof(PazStatusEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(PazStatusEvent);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

// ============================================================================
// НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void PazManager::publishPazEvent(const char* eventType, const char* details) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    LogLevel level = LOG_LEVEL_INFO;
    if (strstr(eventType, "PANIC") != nullptr || strstr(eventType, "CRITICAL") != nullptr) {
        level = LOG_LEVEL_CRITICAL;
    } else if (strstr(eventType, "WARNING") != nullptr || strstr(eventType, "ERROR") != nullptr) {
        level = LOG_LEVEL_WARNING;
    }

    event.payload.logData.level = level;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "PAZ: %s - %s", eventType, details ? details : "");

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void PazManager::publishPanicEvent(const char* reason) {
    PazPanicEvent event;
    memset(&event, 0, sizeof(PazPanicEvent));
    safeStrCopy(event.reason, sizeof(event.reason), reason ? reason : "unknown");
    event.timestamp = millis();
    event.uptime = millis() / 1000;
    event.freeHeap = ESP.getFreeHeap();
    event.freePsram = ESP.getFreePsram();
    event.networkHealthy = isNetworkHealthy();
    event.netStep = getNetStep();

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_PAZ_PANIC;
    data.value = _panicCount;
    memcpy(data.payload, &event, min(sizeof(PazPanicEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(PazPanicEvent);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void PazManager::publishLogEvent(const PazLogEntry& entry) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_PAZ_LOG;
    data.value = (uint8_t)entry.code;
    memcpy(data.payload, &entry, min(sizeof(PazLogEntry), sizeof(data.payload)));
    data.payloadLen = sizeof(PazLogEntry);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void PazManager::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_PAZ_WARNING;
    data.value = 0;
    safeStrCopy(data.payload, sizeof(data.payload), errorCode ? errorCode : "UNKNOWN_ERROR");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

// ============================================================================
// 10. ИНИЦИАЛИЗАЦИЯ
// ============================================================================
void PazManager::begin(uint8_t sdaPin, uint8_t sclPin, uint32_t wdtTimeoutMs) {
    if (_initInProgress) {
        logMessage("Begin already in progress, skipping...");
        return;
    }
    _initInProgress = true;

    if (_initialized) {
        logMessage("Already active");
        _initInProgress = false;
        return;
    }

    _sdaPin = sdaPin;
    _sclPin = sclPin;

    if (_sdaPin == 0 || _sclPin == 0) {
        logMessage("Invalid I2C pins: SDA=%d, SCL=%d", _sdaPin, _sclPin);
        _i2cAlert = true;
        _lastCode = PazDiagnosticCode::I2C_BUS_ERROR;
    } else {
        Wire.begin(_sdaPin, _sclPin);
        Wire.setClock(400000);
        Wire.setTimeout(50);
        logMessage("I2C initialized: SDA=%d, SCL=%d", _sdaPin, _sclPin);
    }

    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = wdtTimeoutMs,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };

    if (esp_task_wdt_reconfigure(&wdt_config) == ESP_OK) {
        esp_task_wdt_add(NULL);
        _wdtEnabled = true;
        logMessage("WDT armed: %lu ms", wdtTimeoutMs);
    } else {
        logMessage("WDT reconfiguration failed!");
    }

    _lastTempCheckMs = millis();
    checkCpuTemperature();

    _lastPazTicksMs = millis();
    _lastI2cCheckMs = millis();
    _lastMemoryCheckMs = millis();
    _lastPingCheckMs = millis();
    _networkHealthy = true;
    _netStep = 0;
    _currentStatus = PazStatus::OK;
    _lastCode = PazDiagnosticCode::OK;
    _initialized = true;
    _startTimeMs = millis();

    esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &PazManager::eventHandler,
        this,
        NULL
    );
    esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &PazManager::eventHandler,
        this,
        NULL
    );

    addLog(PazDiagnosticCode::OK, "PazManager deployed", "System safe start");
    logMessage("PazManager initialized");
    _initInProgress = false;
}

void PazManager::end() {
    stop();
}

void PazManager::reset() {
    _panicTripped = false;
    _relayAlert = false;
    _i2cAlert = false;
    _memoryAlert = false;
    _sensorAlert = false;

    portENTER_CRITICAL(&_pazIsrMux);
    _networkHealthy = true;
    _netStep = 0;
    _stepTimerMs = 0;
    portEXIT_CRITICAL(&_pazIsrMux);

    _openedTimestamp = 0;
    _currentStatus = PazStatus::OK;
    _lastCode = PazDiagnosticCode::OK;
    _recoveryInProgress = false;

    publishStatusEvent(_currentStatus, _lastCode);
    addLog(PazDiagnosticCode::OK, "Manual reset executed", "Alarms cleared");
    logMessage("Alarms cleared");
}

// ============================================================================
// 11. WDT
// ============================================================================
void PazManager::resetWdt() {
    if (_wdtEnabled) {
        esp_task_wdt_reset();
    }
}

void PazManager::forceReset() {
    logMessage("Force reset - restarting ESP32...");
    addLog(PazDiagnosticCode::WDT_RESET, "Force reset", "System restart");
    delay(100);
    ESP.restart();
}

// ============================================================================
// 12. КОНТРОЛЬ ТЕМПЕРАТУРЫ
// ============================================================================
void PazManager::checkCpuTemperature() {
    #ifdef ESP_TEMPERATURE_SENSOR_SUPPORTED
    float temp = temperatureRead();
    if (isnan(temp) || temp < -10.0f || temp > 120.0f) {
        return;
    }
    #else
    float temp = 35.0f + (ESP.getCpuFreqMHz() - 80) * 0.05f;
    #endif

    _cpuTemperature = temp;
    if (temp > _maxCpuTemperature) {
        _maxCpuTemperature = temp;
    }

    char buf[32];

    if (temp < _tempWarningThreshold) {
        if (_currentStatus == PazStatus::WARNING ||
            _currentStatus == PazStatus::ERROR ||
            _currentStatus == PazStatus::CRITICAL) {
            if (_lastCode == PazDiagnosticCode::PANIC_TRIGGERED) {
                return;
            }
            updateStatus(PazStatus::OK, PazDiagnosticCode::OK, "Temperature normal");
            snprintf(buf, sizeof(buf), "CPU Temp: %.1f°C (OK)", temp);
            logMessage(buf);
        }
        return;
    }

    if (temp >= _tempWarningThreshold && temp < _tempCriticalThreshold) {
        if (_currentStatus != PazStatus::WARNING) {
            snprintf(buf, sizeof(buf), "CPU Temp: %.1f°C (WARNING)", temp);
            updateStatus(PazStatus::WARNING, PazDiagnosticCode::MEMORY_LEAK, buf);
            logMessage(buf);
            addLog(PazDiagnosticCode::MEMORY_LEAK, "CPU temperature warning", buf);
        }
        return;
    }

    if (temp >= _tempCriticalThreshold && temp < _tempPanicThreshold) {
        if (_currentStatus != PazStatus::CRITICAL) {
            snprintf(buf, sizeof(buf), "CPU Temp: %.1f°C (CRITICAL)", temp);
            updateStatus(PazStatus::CRITICAL, PazDiagnosticCode::MEMORY_LEAK, buf);
            logMessage(buf);
            addLog(PazDiagnosticCode::MEMORY_LEAK, "CPU temperature critical", buf);
        }
        return;
    }

    if (temp >= _tempPanicThreshold) {
        if (_currentStatus != PazStatus::PANIC) {
            snprintf(buf, sizeof(buf), "CPU Temp: %.1f°C (PANIC!)", temp);
            updateStatus(PazStatus::PANIC, PazDiagnosticCode::PANIC_TRIGGERED, buf);
            logMessage(buf);
            triggerPanic("CPU_OVERHEAT");
        }
    }
}

// ============================================================================
// 13. РЕГИСТРАЦИЯ ПЕРИФЕРИИ
// ============================================================================
void PazManager::registerActuator(uint8_t pin, bool activateState, uint32_t maxActiveMs) {
    if (pin == 255) {
        logMessage("Actuator registration rejected: invalid pin");
        return;
    }
    _actuatorPin = pin;
    _actuatorActiveState = activateState;
    _maxActiveMs = maxActiveMs;
    pinMode(_actuatorPin, OUTPUT);
    digitalWrite(_actuatorPin, !activateState);
    logMessage("Actuator registered: pin=%d, active=%s, max=%lu ms",
              pin, activateState ? "HIGH" : "LOW", maxActiveMs);
}

void PazManager::notifyActuatorSafeClose() {
    if (_actuatorPin != 255) {
        digitalWrite(_actuatorPin, !_actuatorActiveState);
        logMessage("Actuator safe close on pin %d", _actuatorPin);
    }
}

void PazManager::registerCriticalSensor(float* sensorPtr, uint32_t maxStuckTimeMs) {
    if (sensorPtr == nullptr) {
        logMessage("Sensor registration rejected: null pointer");
        return;
    }
    _criticalSensorPtr = sensorPtr;
    _maxSensorStuckTimeMs = maxStuckTimeMs;
    _lastSensorValue = *sensorPtr;
    _lastSensorChangeTimestamp = millis();
    logMessage("Critical sensor registered: ptr=%p, max_stuck=%lu ms",
              sensorPtr, maxStuckTimeMs);
}

void PazManager::notifySensorUpdated() {
    if (_criticalSensorPtr != nullptr) {
        _lastSensorValue = *_criticalSensorPtr;
        _lastSensorChangeTimestamp = millis();
    }
}

// ============================================================================
// 14. ОБРАТНАЯ СВЯЗЬ ОТ СКУД
// ============================================================================
void PazManager::notifyCardScanned() {
    _lastCardScanTime = millis();
    logMessage("Card scanned");
}

void PazManager::notifyRelayActivated() {
    _lastRelayActivationTime = millis();
    logMessage("Relay activated");
}

void PazManager::notifyRelayDeactivated() {
    logMessage("Relay deactivated");
}

void PazManager::notifyDoorOpened() {
    _lastDoorEventTime = millis();
    logMessage("Door opened");
}

void PazManager::notifyDoorClosed() {
    _lastDoorEventTime = millis();
    logMessage("Door closed");
}

// ============================================================================
// 15. ЗАЩИТА АКТУАТОРА
// ============================================================================
void PazManager::runActuatorProtection() {
    if (_actuatorPin == 255 || _maxActiveMs == 0) return;

    bool isPinActive = (digitalRead(_actuatorPin) == _actuatorActiveState);
    _actuatorCheckCount++;

    if (isPinActive) {
        if (_openedTimestamp == 0) {
            _openedTimestamp = millis();
        } else {
            uint32_t elapsed = millis() - _openedTimestamp;
            if (elapsed > _maxActiveMs + PAZ_ACTUATOR_SAFETY_MARGIN_MS) {
                _relayAlert = true;
                _lastCode = PazDiagnosticCode::ACTUATOR_STICK;
                char errBuf[32];
                snprintf(errBuf, sizeof(errBuf), "Stuck for %lu ms", elapsed);
                updateStatus(PazStatus::ERROR, PazDiagnosticCode::ACTUATOR_STICK, errBuf);
                logMessage("Actuator STUCK on pin %d!", _actuatorPin);
                addLog(PazDiagnosticCode::ACTUATOR_STICK, "Actuator stuck", errBuf);
                triggerPanic("ACTUATOR_STICK");
            }
        }
    } else {
        _openedTimestamp = 0;
    }
}

// ============================================================================
// 16. ЗАЩИТА ДАТЧИКА
// ============================================================================
void PazManager::runSensorProtection() {
    if (_criticalSensorPtr == nullptr || _maxSensorStuckTimeMs == 0) return;

    float currentValue = *_criticalSensorPtr;
    _sensorCheckCount++;

    if (isnan(currentValue)) {
        _sensorAlert = true;
        _lastCode = PazDiagnosticCode::SENSOR_NAN;
        updateStatus(PazStatus::ERROR, PazDiagnosticCode::SENSOR_NAN, "Sensor NaN");
        logMessage("Sensor NaN detected!");
        addLog(PazDiagnosticCode::SENSOR_NAN, "Sensor NaN", "Line open");
        triggerPanic("SENSOR_NAN");
        return;
    }

    if (currentValue != _lastSensorValue) {
        _lastSensorValue = currentValue;
        _lastSensorChangeTimestamp = millis();
        if (_sensorAlert) {
            _sensorAlert = false;
            updateStatus(PazStatus::OK, PazDiagnosticCode::OK, "Sensor restored");
        }
    } else {
        uint32_t stuckTime = millis() - _lastSensorChangeTimestamp;
        if (stuckTime > _maxSensorStuckTimeMs) {
            _sensorAlert = true;
            _lastCode = PazDiagnosticCode::SENSOR_STUCK;
            char errBuf[32];
            snprintf(errBuf, sizeof(errBuf), "Frozen %lu ms", stuckTime);
            updateStatus(PazStatus::ERROR, PazDiagnosticCode::SENSOR_STUCK, errBuf);
            logMessage("Sensor stuck: %.2f for %lu ms", currentValue, stuckTime);
            addLog(PazDiagnosticCode::SENSOR_STUCK, "Sensor stuck", errBuf);
            triggerPanic("SENSOR_STUCK");
        }
    }
}

// ============================================================================
// 17. КОНТРОЛЬ ПАМЯТИ
// ============================================================================
void PazManager::runMemoryAndFsProtection() {
    uint32_t freeHeap = ESP.getFreeHeap();

    if (freeHeap < _minSafeHeapBytes) {
        _memoryAlert = true;
        _lastCode = PazDiagnosticCode::MEMORY_LEAK;
        char errBuf[32];
        snprintf(errBuf, sizeof(errBuf), "Heap: %lu bytes", freeHeap);
        updateStatus(PazStatus::CRITICAL, PazDiagnosticCode::MEMORY_LEAK, errBuf);
        logMessage("LOW MEMORY: %lu bytes (threshold: %zu)", freeHeap, _minSafeHeapBytes);
        addLog(PazDiagnosticCode::MEMORY_LEAK, "Low memory", errBuf);
        triggerPanic("LOW_MEMORY");
    } else {
        if (_memoryAlert) {
            _memoryAlert = false;
            updateStatus(PazStatus::OK, PazDiagnosticCode::OK, "Memory OK");
        }
    }

    if (!LittleFS.begin(false)) {
        logMessage("LittleFS mount failed!");
        addLog(PazDiagnosticCode::MEMORY_LEAK, "LittleFS mount failed", "");
        triggerPanic("FS_MOUNT_FAILED");
    }
}

// ============================================================================
// 18. КОНТРОЛЬ I2C
// ============================================================================
void PazManager::runI2cRecovery() {
    if (_sdaPin == 0 || _sclPin == 0) return;

    Wire.beginTransmission(0x68);
    uint8_t error = Wire.endTransmission();

    if (error != 0) {
        if (!_i2cAlert) {
            _i2cAlert = true;
            _lastCode = PazDiagnosticCode::I2C_BUS_ERROR;
            char errBuf[32];
            snprintf(errBuf, sizeof(errBuf), "I2C error: %d", error);
            updateStatus(PazStatus::WARNING, PazDiagnosticCode::I2C_BUS_ERROR, errBuf);
            logMessage("I2C error: %d", error);
            addLog(PazDiagnosticCode::I2C_BUS_ERROR, "I2C error", errBuf);

            restoreI2cBus();

            Wire.beginTransmission(0x68);
            if (Wire.endTransmission() == 0) {
                _i2cAlert = false;
                _lastCode = PazDiagnosticCode::OK;
                updateStatus(PazStatus::OK, PazDiagnosticCode::OK, "I2C recovered");
                logMessage("I2C recovered");
                addLog(PazDiagnosticCode::OK, "I2C recovered", "");
            } else {
                logMessage("I2C recovery failed");
                addLog(PazDiagnosticCode::I2C_BUS_ERROR, "I2C recovery failed", "");
            }
        }
    } else {
        if (_i2cAlert) {
            _i2cAlert = false;
            _lastCode = PazDiagnosticCode::OK;
            updateStatus(PazStatus::OK, PazDiagnosticCode::OK, "I2C OK");
            logMessage("I2C OK");
        }
    }
}

void PazManager::restoreI2cBus() {
    Wire.end();
    delay(10);
    Wire.begin(_sdaPin, _sclPin);
    Wire.setClock(400000);
    Wire.setTimeout(50);
}

// ============================================================================
// 19. СЕТЕВАЯ ДИАГНОСТИКА
// ============================================================================
void PazManager::runNetworkDiagnostics() {
    if (!ETH.linkUp()) {
        portENTER_CRITICAL(&_pazIsrMux);
        if (_networkHealthy) {
            _networkHealthy = false;
            _lastCode = PazDiagnosticCode::NETWORK_DOWN;
            portEXIT_CRITICAL(&_pazIsrMux);
            updateStatus(PazStatus::WARNING, PazDiagnosticCode::NETWORK_DOWN, "Ethernet link down");
            logMessage("Ethernet link DOWN");
            addLog(PazDiagnosticCode::NETWORK_DOWN, "Ethernet link down", "");
        } else {
            portEXIT_CRITICAL(&_pazIsrMux);
        }
        _netStep = 0;
        return;
    }

    IPAddress localIP = ETH.localIP();
    if (localIP == IPAddress() || localIP == IPAddress(0, 0, 0, 0)) {
        portENTER_CRITICAL(&_pazIsrMux);
        if (_networkHealthy) {
            _networkHealthy = false;
            _lastCode = PazDiagnosticCode::NETWORK_DOWN;
            portEXIT_CRITICAL(&_pazIsrMux);
            updateStatus(PazStatus::WARNING, PazDiagnosticCode::NETWORK_DOWN, "No IP address");
            logMessage("No IP address");
        } else {
            portEXIT_CRITICAL(&_pazIsrMux);
        }
        return;
    }

    IPAddress gatewayIP = ETH.gatewayIP();
    if (gatewayIP == IPAddress() || gatewayIP == IPAddress(0, 0, 0, 0)) {
        gatewayIP = IPAddress(192, 168, 1, 1);
    }

    ip_addr_t targetAddr;
    targetAddr.type = IPADDR_TYPE_V4;
    targetAddr.u_addr.ip4.addr = (uint32_t)gatewayIP;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = targetAddr;
    ping_config.count = _pingCount;
    ping_config.timeout_ms = _pingTimeoutMs;
    ping_config.interval_ms = 1000;

    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_end = PazManager::pingEndCallback;

    esp_ping_handle_t ping_handle;
    if (esp_ping_new_session(&ping_config, &callbacks, &ping_handle) == ESP_OK) {
        _pendingPingHandle = ping_handle;
        _pingDone = false;
        esp_ping_start(ping_handle);
    } else {
        logMessage("Ping allocation error");
        return;
    }

    if (getNetStep() > 0) {
        if (_stepTimerMs == 0) _stepTimerMs = millis();
        if (millis() - _stepTimerMs > _stepTimeoutMs) {
            _stepTimerMs = millis();
            portENTER_CRITICAL(&_pazIsrMux);
            uint8_t currentStep = ++_netStep;
            portEXIT_CRITICAL(&_pazIsrMux);

            switch (currentStep) {
                case 1:
                    logMessage("Net Step 1: Degradation");
                    updateStatus(PazStatus::WARNING, PazDiagnosticCode::NETWORK_DEGRADATION, "Net step 1");
                    break;
                case 2: {
                    logMessage("Net Step 2: Resetting Ethernet");
                    esp_eth_handle_t eth_handle = (esp_eth_handle_t)ETH.handle();
                    if (eth_handle != nullptr) {
                        esp_eth_stop(eth_handle);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        esp_eth_start(eth_handle);
                    }
                    break;
                }
                case 3:
                    logMessage("Net Step 3: Reboot");
                    addLog(PazDiagnosticCode::NETWORK_DOWN, "Network permanent loss", "Reboot");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    _wdtResets++;
                    ESP.restart();
                    break;
                default:
                    break;
            }
        }
    } else {
        portENTER_CRITICAL(&_pazIsrMux);
        if (!_networkHealthy) {
            _networkHealthy = true;
            _lastCode = PazDiagnosticCode::OK;
            portEXIT_CRITICAL(&_pazIsrMux);
            updateStatus(PazStatus::OK, PazDiagnosticCode::OK, "Network recovered");
            logMessage("Network recovered");
            addLog(PazDiagnosticCode::OK, "Network recovered", "");
        } else {
            portEXIT_CRITICAL(&_pazIsrMux);
        }
        _stepTimerMs = millis();
        portENTER_CRITICAL(&_pazIsrMux);
        _netStep = 0;
        portEXIT_CRITICAL(&_pazIsrMux);
    }
}

// ============================================================================
// 20. ПИНГ КОЛЛБЭК
// ============================================================================
void PazManager::pingEndCallback(esp_ping_handle_t hdl, void* args) {
    uint32_t transmitted = 0, received = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));

    portENTER_CRITICAL_ISR(&_pazIsrMux);
    if (received > 0) {
        _networkHealthy = true;
        _netStep = 0;
        _stepTimerMs = 0;
        _pingSuccessCount++;
    } else {
        _networkHealthy = false;
        _pingFailCount++;
    }
    _pingDone = true;
    portEXIT_CRITICAL_ISR(&_pazIsrMux);
}

// ============================================================================
// 21. ПАНИКА И ВОССТАНОВЛЕНИЕ
// ============================================================================
void PazManager::triggerPanic(const char* reason) {
    if (_panicTripped) return;

    _panicTripped = true;
    _panicCount++;
    _lastPanicTime = millis();
    safeStrCopy(_lastPanicReason, sizeof(_lastPanicReason), reason ? reason : "unknown");
    _currentStatus = PazStatus::PANIC;
    _lastCode = PazDiagnosticCode::PANIC_TRIGGERED;

    addLog(PazDiagnosticCode::PANIC_TRIGGERED, "PANIC!", reason);
    addPanicHistory(reason);
    logMessage("PANIC: %s", reason);

    // НОВОЕ: публикация через новую шину
    publishPazEvent("PANIC", reason);

    if (_actuatorPin != 255) {
        digitalWrite(_actuatorPin, !_actuatorActiveState);
        logMessage("Actuator closed on pin %d", _actuatorPin);
    }

    publishPanicEvent(reason);
    publishStatusEvent(_currentStatus, _lastCode);
    if (_panicCb) _panicCb(reason);
}

void PazManager::checkRecovery() {
    if (!_panicTripped) return;

    uint32_t elapsed = millis() - _lastPanicTime;
    if (elapsed > _recoveryTimeoutMs && !_recoveryInProgress) {
        logMessage("Recovery timeout elapsed. Starting recovery...");
        _recoveryInProgress = true;
        performRecovery();
    }
}

void PazManager::performRecovery() {
    logMessage("Performing recovery...");

    if (_actuatorPin != 255) {
        digitalWrite(_actuatorPin, !_actuatorActiveState);
    }

    _panicTripped = false;
    _relayAlert = false;
    _i2cAlert = false;
    _memoryAlert = false;
    _sensorAlert = false;

    if (_sdaPin != 0 && _sclPin != 0) {
        restoreI2cBus();
    }

    if (ETH.linkUp()) {
        esp_eth_handle_t eth_handle = (esp_eth_handle_t)ETH.handle();
        if (eth_handle != nullptr) {
            esp_eth_stop(eth_handle);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_eth_start(eth_handle);
        }
    }

    _recoveryInProgress = false;
    _lastRecoveryAttemptMs = millis();
    _currentStatus = PazStatus::OK;
    _lastCode = PazDiagnosticCode::OK;

    updateStatus(PazStatus::OK, PazDiagnosticCode::OK, "Recovery success");
    logMessage("Recovery complete");
    addLog(PazDiagnosticCode::OK, "Recovery complete", "");
    publishStatusEvent(_currentStatus, _lastCode);
}

// ============================================================================
// 22. СТАТУС И ЛОГИ (ИЗМЕНЕНО: добавлен publishPazEvent)
// ============================================================================
void PazManager::updateStatus(PazStatus newStatus, PazDiagnosticCode code, const char* msg) {
    if (_currentStatus != newStatus || _lastCode != code) {
        _currentStatus = newStatus;
        _lastCode = code;

        // НОВОЕ: публикация через новую шину
        publishPazEvent(getStatusString(), msg);

        if (_statusCb) _statusCb(newStatus, code);
        publishStatusEvent(newStatus, code);

        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf), "Status: %s | Code: %s",
                getStatusString(), getCodeString(code));
        logMessage(logBuf);
    }
}

void PazManager::addLog(PazDiagnosticCode code, const char* msg, const char* details) {
    if (_pazMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_pazMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        PazLogEntry entry;
        memset(&entry, 0, sizeof(PazLogEntry));
        entry.timestamp = millis();
        entry.code = code;
        safeStrCopy(entry.message, sizeof(entry.message), msg ? msg : "");
        safeStrCopy(entry.details, sizeof(entry.details), details ? details : "");

        // НОВОЕ: публикация через новую шину
        publishPazEvent("LOG", msg);

        if (_logEntries.size() >= PAZ_MAX_LOG_ENTRIES) {
            _logEntries.erase(_logEntries.begin());
        }
        _logEntries.push_back(entry);
        publishLogEvent(entry);
        if (_logCb) _logCb(entry);
        xSemaphoreGiveRecursive(_pazMutex);
    }
}

void PazManager::addPanicHistory(const char* reason) {
    if (_pazMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_pazMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        PanicHistoryEntry entry;
        memset(&entry, 0, sizeof(PanicHistoryEntry));
        safeStrCopy(entry.reason, sizeof(entry.reason), reason ? reason : "unknown");
        entry.timestamp = millis();
        entry.uptime = millis() / 1000;
        entry.freeHeap = ESP.getFreeHeap();
        entry.freePsram = ESP.getFreePsram();
        entry.networkHealthy = isNetworkHealthy();

        if (_panicHistory.size() >= PAZ_MAX_PANIC_HISTORY) {
            _panicHistory.erase(_panicHistory.begin());
        }
        _panicHistory.push_back(entry);
        xSemaphoreGiveRecursive(_pazMutex);
    }
}

// ============================================================================
// 23. ГЕТТЕРЫ
// ============================================================================
bool PazManager::isNetworkHealthy() const {
    bool result;
    portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_pazIsrMux));
    result = _networkHealthy;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_pazIsrMux));
    return result;
}

uint8_t PazManager::getNetStep() const {
    uint8_t step;
    portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_pazIsrMux));
    step = _netStep;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_pazIsrMux));
    return step;
}

std::vector<PanicHistoryEntry> PazManager::getPanicHistory(size_t limit) const {
    std::vector<PanicHistoryEntry> result;
    if (_pazMutex == nullptr) return result;

    if (xSemaphoreTakeRecursive(_pazMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t outSize = min(limit, _panicHistory.size());
        if (outSize > 0) {
            result.assign(_panicHistory.end() - outSize, _panicHistory.end());
        }
        xSemaphoreGiveRecursive(_pazMutex);
    }
    return result;
}

// ============================================================================
// 24. СТАТИЧЕСКИЕ МЕТОДЫ
// ============================================================================
const char* PazManager::getStatusString() const {
    switch (_currentStatus) {
        case PazStatus::OK: return "OK";
        case PazStatus::WARNING: return "WARNING";
        case PazStatus::ERROR: return "ERROR";
        case PazStatus::CRITICAL: return "CRITICAL";
        case PazStatus::PANIC: return "PANIC";
        default: return "UNKNOWN";
    }
}

const char* PazManager::getCodeString(PazDiagnosticCode code) const {
    switch (code) {
        case PazDiagnosticCode::OK: return "OK";
        case PazDiagnosticCode::ACTUATOR_STICK: return "ACTUATOR_STICK";
        case PazDiagnosticCode::MEMORY_LEAK: return "MEMORY_LEAK";
        case PazDiagnosticCode::SENSOR_STUCK: return "SENSOR_STUCK";
        case PazDiagnosticCode::SENSOR_NAN: return "SENSOR_NAN";
        case PazDiagnosticCode::I2C_BUS_ERROR: return "I2C_ERROR";
        case PazDiagnosticCode::NETWORK_DOWN: return "NETWORK_DOWN";
        case PazDiagnosticCode::NETWORK_DEGRADATION: return "NETWORK_DEGRADATION";
        case PazDiagnosticCode::WDT_RESET: return "WDT_RESET";
        case PazDiagnosticCode::PANIC_TRIGGERED: return "PANIC";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// 25. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void PazManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================");
    stream.println(" PAZ MANAGER DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Status: %s\n", getStatusString());
    stream.printf(" Code: %s\n", getCodeString(_lastCode));
    stream.printf(" Panic: %s\n", _panicTripped ? "ENGAGED" : "CLEAR");
    stream.printf(" WDT: %s\n", _wdtEnabled ? "ARMED" : "DISABLED");
    stream.printf(" Panic Count: %lu\n", _panicCount);
    stream.printf(" WDT Resets: %lu\n", _wdtResets);
    stream.printf(" Last Panic: %s\n", _lastPanicReason);
    stream.printf(" CPU Temp: %.1f°C (max: %.1f°C)\n", _cpuTemperature, _maxCpuTemperature);
    stream.println("-- Alerts --");
    stream.printf(" Actuator: %s\n", _relayAlert ? "ALERT" : "OK");
    stream.printf(" I2C: %s\n", _i2cAlert ? "ALERT" : "OK");
    stream.printf(" Memory: %s\n", _memoryAlert ? "ALERT" : "OK");
    stream.printf(" Sensor: %s\n", _sensorAlert ? "ALERT" : "OK");
    stream.printf(" Network: %s\n", isNetworkHealthy() ? "OK" : "DOWN");
    stream.printf(" Net Step: %d\n", getNetStep());
    stream.println("-- Actuator --");
    stream.printf(" Pin: %d\n", _actuatorPin);
    stream.printf(" Active: %s\n", _actuatorActiveState ? "HIGH" : "LOW");
    stream.printf(" Max Active: %lu ms\n", _maxActiveMs);
    stream.printf(" Check Count: %lu\n", _actuatorCheckCount);
    stream.println("-- Sensor --");
    stream.printf(" Ptr: %p\n", _criticalSensorPtr);
    stream.printf(" Check Count: %lu\n", _sensorCheckCount);
    stream.printf(" Max Stuck: %lu ms\n", _maxSensorStuckTimeMs);
    stream.println("-- Network --");
    stream.printf(" Ping Success: %lu\n", _pingSuccessCount);
    stream.printf(" Ping Fail: %lu\n", _pingFailCount);
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("-- Panic History --");
    for (const auto& entry : _panicHistory) {
        stream.printf(" %s at %lu (uptime: %lu, heap: %lu)\n",
                     entry.reason, entry.timestamp, entry.uptime, entry.freeHeap);
    }
    stream.println("-- Log --");
    for (const auto& entry : _logEntries) {
        stream.printf(" [%lu] %s: %s\n",
                     entry.timestamp, getCodeString(entry.code), entry.message);
    }
    stream.println("==============================");
}

void PazManager::printStats() const {
    streamDiagnosticInfo(Serial);
}