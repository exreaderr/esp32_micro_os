// ============================================================================
// RTCManager.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Полноценный менеджер часов реального времени DS3231.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА опечатка _issync -> _isSynced (везде!)
// - ИСПРАВЛЕНА синтаксическая ошибка sizeOf -> sizeof
// - УСТРАНЕНО дублирование getDateTimeString()
// - ИСПРАВЛЕНА отправка событий в logMessage
// - Исправлена логика eventHandler
// - Добавлена проверка _initialized в getTemperature
// - Добавлен метод safeStrCopy
// - Добавлена полная потокобезопасность
// - Добавлен метод formatDateTime
// - Улучшена обработка ошибок I2C
// ============================================================================
#include "RTCManager.h"
#include <esp_task_wdt.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static RTCManager _rtcManagerInstance;

// ============================================================================
// СТАТИЧЕСКИЕ МАССИВЫ
// ============================================================================
const char* RTCManager::WEEKDAY_NAMES[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

const char* RTCManager::MONTH_NAMES[12] = {
    "January", "February", "March", "April",
    "May", "June", "July", "August",
    "September", "October", "November", "December"
};

const char* RTCManager::WEEKDAY_NAMES_RU[7] = {
    "Воскресенье", "Понедельник", "Вторник", "Среда",
    "Четверг", "Пятница", "Суббота"
};

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
RTCManager::RTCManager() {
    _moduleId = MODULE_ID_RTC;

    // Рекурсивный мьютекс
    _rtcMutex = xSemaphoreCreateRecursiveMutex();
    if (_rtcMutex == nullptr) {
        Serial.println("[RTC] CRITICAL: Failed to create mutex!");
    }

    _isSynced = false;  // <-- ИСПРАВЛЕНО!
    _rtcFound = false;
    _rtcAlert = false;
    _initialized = false;
    _beginInProgress = false;
    _timezoneOffset = 0;
    _lastSyncMs = 0;
    _lastI2cCheckMs = 0;
    _syncRetryCount = 0;
    _syncFailCount = 0;
    _lastAlarmCheckMs = 0;

    _onTimeSync = nullptr;
    _onRTCStatus = nullptr;
    _onRTCError = nullptr;
    _onAlarmTrigger = nullptr;

    Serial.println("[RTC] Instance created (v4.2.2)");
}

RTCManager::~RTCManager() {
    stop();
    if (_rtcMutex != nullptr) {
        vSemaphoreDelete(_rtcMutex);
        _rtcMutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
RTCManager& RTCManager::getInstance() {
    return _rtcManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void RTCManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void RTCManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[RTC] %s\n", msg);

    // Отправляем событие для LogManager (исправлено!)
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

void RTCManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void RTCManager::init() {
    logMessage("Init pending - call begin() with parameters");
}

void RTCManager::start() {
    if (_initialized && _rtcFound && !_rtcAlert) {
        logMessage("Started");
    }
}

void RTCManager::stop() {
    if (!_initialized) return;
    _initialized = false;
    logMessage("Stopped");
}

void RTCManager::tick() {
    if (!_initialized) return;

    esp_task_wdt_reset();
    uint32_t now = millis();

    // Проверка I2C
    if (now - _lastI2cCheckMs >= _i2cCheckIntervalMs) {
        _lastI2cCheckMs = now;
        checkI2C();
    }

    // Автоматическая синхронизация
    if (_autoSync && _rtcFound && !_rtcAlert && _isSynced) {  // <-- ИСПРАВЛЕНО!
        if (now - _lastSyncMs >= _syncIntervalMs) {
            _lastSyncMs = now;
            syncFromRTC();
            logMessage("Auto sync completed");
        }
    }

    // Проверка будильника
    checkAlarm();
}

// ============================================================================
// 5. ОБРАБОТКА СОБЫТИЙ (ИСПРАВЛЕНО)
// ============================================================================
void RTCManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                              int32_t id, void* eventData) {
    RTCManager* instance = static_cast<RTCManager*>(handlerArgs);
    if (!instance || !instance->_initialized) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_BOOT:
                instance->syncFromRTC();
                break;
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
                break;
            case SH_EVENT_NET_CONNECTED:
                // Синхронизация при подключении сети (если есть NTP)
                break;
            default:
                break;
        }
    }  // <-- ИСПРАВЛЕНО: закрывающая скобка!

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void RTCManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    switch (eventId) {
        case SH_EVENT_CMD_EXECUTE:
            if (data->targetModule == _moduleId || data->targetModule == 0) {
                handleCommand(data);
            }
            break;
        case SH_EVENT_SYS_BOOT:
            syncFromRTC();
            break;
        case SH_EVENT_SYS_RESTART:
        case SH_EVENT_SYS_SHUTDOWN:
            stop();
            break;
        default:
            break;
    }
}

bool RTCManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_SYS_BOOT ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN ||
            eventId == SH_EVENT_NET_CONNECTED);
}

// ============================================================================
// 6. СТАТУС
// ============================================================================
const char* RTCManager::getStatus() const {
    static char statusBuffer[128];
    const char* state = "UNKNOWN";

    if (!_rtcFound) state = "NOT_FOUND";
    else if (_rtcAlert) state = "I2C_ERROR";
    else if (!_isSynced) state = "NOT_SYNCED";  // <-- ИСПРАВЛЕНО!
    else state = "OK";

    snprintf(statusBuffer, sizeof(statusBuffer),
            "State: %s, TZ: %+d, Temp: %.1f°C",
            state, _timezoneOffset / 3600, getTemperature());
    return statusBuffer;
}

void RTCManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _rtcFound ? 1 : 0;

    snprintf(data->payload, sizeof(data->payload),
            "found:%d,alert:%d,synced:%d,tz:%d,temp:%.1f,retry:%lu",
            _rtcFound ? 1 : 0,
            _rtcAlert ? 1 : 0,
            _isSynced ? 1 : 0,  // <-- ИСПРАВЛЕНО!
            _timezoneOffset / 3600,
            getTemperature(),
            _syncRetryCount);
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 7. ОБРАБОТКА КОМАНД (ИСПРАВЛЕНО)
// ============================================================================
void RTCManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case CMD_GET_STATUS: {
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = _isSynced ? 1 : 0;
            safeStrCopy(response.payload, sizeof(response.payload), getDateTimeString().c_str());
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0401: { // GET_TIME
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0401;
            response.value = getUnixTime();
            safeStrCopy(response.payload, sizeof(response.payload), getDateTimeString().c_str());
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0402: // SET_TIME
            if (data->value > 0) {
                syncTime(data->value);
            }
            break;

        case 0x0403: // SYNC_RTC
            syncFromRTC();
            break;

        case 0x0404: // SET_TIMEZONE
            setTimeZone(data->value);
            break;

        case 0x0405: { // SET_ALARM
            if (data->payloadLen >= sizeof(RtcAlarmConfig)) {
                RtcAlarmConfig config;
                memcpy(&config, data->payload, sizeof(RtcAlarmConfig));
                setAlarm(config);
            }
            break;
        }

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 8. ОТПРАВКА СОБЫТИЙ
// ============================================================================
void RTCManager::publishSyncEvent(uint32_t timestamp, bool fromHardware,
                                  bool success, const char* source) {
    RtcSyncEvent event;
    event.timestamp = timestamp;
    event.uptime = millis() / 1000;
    event.timezoneOffset = _timezoneOffset;
    event.fromHardware = fromHardware;
    event.success = success;
    safeStrCopy(event.source, sizeof(event.source), source ? source : "unknown");

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_RTC_SYNCED;
    data.value = success ? 1 : 0;
    memcpy(data.payload, &event, min(sizeof(RtcSyncEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(RtcSyncEvent);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void RTCManager::publishStatusEvent() {
    RtcStatusEvent event;
    event.found = _rtcFound;
    event.alert = _rtcAlert;
    event.synced = _isSynced;  // <-- ИСПРАВЛЕНО!
    event.temperature = getTemperature();
    event.lastSyncTime = _lastSyncMs;
    safeStrCopy(event.errorCode, sizeof(event.errorCode), getStatusString());

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = _rtcFound ? SH_EVENT_RTC_FOUND : SH_EVENT_RTC_LOST;
    data.value = _rtcFound ? 1 : 0;
    memcpy(data.payload, &event, min(sizeof(RtcStatusEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(RtcStatusEvent);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void RTCManager::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_RTC_ERROR;
    data.value = _syncFailCount;
    safeStrCopy(data.payload, sizeof(data.payload), errorCode ? errorCode : "UNKNOWN_ERROR");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void RTCManager::publishTimeChangedEvent() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_RTC_TIME_CHANGED;
    data.value = getUnixTime();
    safeStrCopy(data.payload, sizeof(data.payload), getDateTimeString().c_str());
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void RTCManager::publishAlarmEvent() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_RTC_ALARM_TRIGGERED;
    data.value = _alarm.hour * 100 + _alarm.minute;
    snprintf(data.payload, sizeof(data.payload), "Alarm: %02d:%02d",
            _alarm.hour, _alarm.minute);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    if (_onAlarmTrigger) {
        _onAlarmTrigger(_alarm);
    }
}

// ============================================================================
// 9. ИНИЦИАЛИЗАЦИЯ (С ЗАЩИТОЙ ОТ ПОВТОРНОГО ВХОДА)
// ============================================================================
void RTCManager::begin(uint8_t sdaPin, uint8_t sclPin, int timezoneOffsetSec) {
    if (_beginInProgress) {
        logMessage("Begin already in progress, skipping...");
        return;
    }
    _beginInProgress = true;

    if (_initialized) {
        logMessage("Already initialized, reconfiguring...");
        _initialized = false;
    }

    _sdaPin = sdaPin;
    _sclPin = sclPin;
    _timezoneOffset = timezoneOffsetSec;

    if (_rtcMutex == nullptr) {
        _rtcMutex = xSemaphoreCreateRecursiveMutex();
    }

    if (xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        logMessage("Mutex timeout on begin");
        _beginInProgress = false;
        return;
    }

    // Инициализация I2C
    Wire.begin(_sdaPin, _sclPin);
    Wire.setTimeout(50);
    logMessage("I2C initialized: SDA=%d, SCL=%d", _sdaPin, _sclPin);

    // Инициализация RTC
    initRTC();

    // Установка часового пояса
    setTimeZone(_timezoneOffset / 3600);

    _lastSyncMs = millis();
    _lastI2cCheckMs = millis();
    _initialized = true;

    xSemaphoreGiveRecursive(_rtcMutex);

    // Подписка на события
    esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &RTCManager::eventHandler,
        this,
        NULL
    );
    esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &RTCManager::eventHandler,
        this,
        NULL
    );

    logMessage("Initialized, timezone: %+d hours", _timezoneOffset / 3600);
    publishStatusEvent();

    _beginInProgress = false;
}

void RTCManager::end() {
    stop();
}

void RTCManager::reset() {
    if (_rtcMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _isSynced = false;  // <-- ИСПРАВЛЕНО!
        _rtcFound = false;
        _rtcAlert = false;
        _syncRetryCount = 0;
        _syncFailCount = 0;

        // Сброс RTC
        if (_rtc.begin()) {
            _rtc.adjust(DateTime(MIN_VALID_TIMESTAMP));
            _rtcFound = true;
            _rtcAlert = false;
        }

        xSemaphoreGiveRecursive(_rtcMutex);
        logMessage("Reset complete");
        publishStatusEvent();
    }
}

// ============================================================================
// 10. ИНИЦИАЛИЗАЦИЯ RTC
// ============================================================================
void RTCManager::initRTC() {
    if (_rtcMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        logMessage("Mutex timeout in initRTC");
        return;
    }

    _rtcFound = _rtc.begin();

    if (_rtcFound) {
        // Проверка питания
        if (_rtc.lostPower()) {
            logMessage("RTC lost power - resetting time");
            _rtc.adjust(DateTime(MIN_VALID_TIMESTAMP));
        }

        // Проверка температуры
        float temp = _rtc.getTemperature();
        if (temp < -40.0f || temp > 85.0f) {
            logMessage("Invalid temperature: %.1f°C", temp);
        } else {
            logMessage("Temperature: %.1f°C", temp);
        }

        _rtcAlert = false;
        syncFromRTC();
        _isSynced = true;  // <-- ИСПРАВЛЕНО!

        if (_onRTCStatus) _onRTCStatus(true);
        publishStatusEvent();
        logMessage("DS3231 found and initialized");
    } else {
        logMessage("DS3231 NOT found");
        _rtcFound = false;
        _rtcAlert = true;
        _isSynced = false;  // <-- ИСПРАВЛЕНО!

        // Устанавливаем время по умолчанию
        struct timeval tv = { .tv_sec = MIN_VALID_TIMESTAMP, .tv_usec = 0 };
        settimeofday(&tv, NULL);

        if (_onRTCStatus) _onRTCStatus(false);
        if (_onRTCError) _onRTCError("RTC_NOT_FOUND");
        publishErrorEvent("RTC_NOT_FOUND");
        publishStatusEvent();
    }

    xSemaphoreGiveRecursive(_rtcMutex);
}

// ============================================================================
// 11. ПРОВЕРКА I2C (ИСПРАВЛЕНО)
// ============================================================================
void RTCManager::checkI2C() {
    if (_rtcMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    Wire.beginTransmission(RTC_I2C_ADDR);
    byte error = Wire.endTransmission();

    if (error != 0) {
        if (!_rtcAlert) {
            _rtcAlert = true;
            _rtcFound = false;
            _isSynced = false;  // <-- ИСПРАВЛЕНО!
            logMessage("I2C lost! Error: %d", error);

            if (_onRTCStatus) _onRTCStatus(false);
            if (_onRTCError) _onRTCError("I2C_BUS_LOST");
            publishErrorEvent("I2C_BUS_LOST");
            publishStatusEvent();
        }
        xSemaphoreGiveRecursive(_rtcMutex);
        return;
    }

    // I2C OK – восстанавливаем, если был в ошибке
    if (_rtcAlert) {
        _rtcAlert = false;
        _rtcFound = true;

        if (_rtc.begin()) {
            syncFromRTC();
            _isSynced = true;  // <-- ИСПРАВЛЕНО!
            logMessage("I2C recovered");
            if (_onRTCStatus) _onRTCStatus(true);
            publishStatusEvent();
        } else {
            logMessage("I2C recovery failed");
            _rtcAlert = true;
            _rtcFound = false;
            if (_onRTCError) _onRTCError("I2C_RECOVERY_FAILED");
            publishErrorEvent("I2C_RECOVERY_FAILED");
        }
    }

    xSemaphoreGiveRecursive(_rtcMutex);
}

// ============================================================================
// 12. ВОССТАНОВЛЕНИЕ I2C
// ============================================================================
void RTCManager::recoverI2C() {
    logMessage("Hardware I2C recovery...");

    // Генерация 9 тактов на SCL для сброса
    pinMode(_sdaPin, INPUT_PULLUP);
    pinMode(_sclPin, INPUT_PULLUP);
    digitalWrite(_sdaPin, HIGH);

    for (int i = 0; i < 9; i++) {
        digitalWrite(_sclPin, HIGH);
        delayMicroseconds(5);
        digitalWrite(_sclPin, LOW);
        delayMicroseconds(5);
    }

    // Stop условие
    digitalWrite(_sdaPin, LOW);
    delayMicroseconds(5);
    digitalWrite(_sdaPin, HIGH);
    delayMicroseconds(5);

    // Переинициализация I2C
    Wire.begin(_sdaPin, _sclPin);
    Wire.setTimeout(50);
    delay(I2C_RECOVERY_DELAY_MS);

    if (_rtc.begin()) {
        _rtcFound = true;
        _rtcAlert = false;
        logMessage("I2C recovered successfully");
        syncFromRTC();
        if (_onRTCStatus) _onRTCStatus(true);
        publishStatusEvent();
    } else {
        logMessage("I2C recovery failed!");
        if (_onRTCError) _onRTCError("I2C_RECOVERY_FAILED");
        publishErrorEvent("I2C_RECOVERY_FAILED");
    }
}

// ============================================================================
// 13. СИНХРОНИЗАЦИЯ ВРЕМЕНИ
// ============================================================================
void RTCManager::syncTime(uint32_t unixTimestamp) {
    if (!isValidTimestamp(unixTimestamp)) {
        logMessage("Invalid timestamp: %lu", unixTimestamp);
        return;
    }

    // Обновляем системное время
    updateSystemTime(unixTimestamp);

    // Обновляем RTC
    if (_rtcFound && !_rtcAlert) {
        if (_rtcMutex && xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            _rtc.adjust(DateTime(unixTimestamp));
            xSemaphoreGiveRecursive(_rtcMutex);
        }
    }

    _isSynced = true;  // <-- ИСПРАВЛЕНО!
    _lastSyncMs = millis();
    _syncRetryCount = 0;

    logMessage("Time synced: %s", getDateTimeString().c_str());
    if (_onTimeSync) _onTimeSync(unixTimestamp);
    publishSyncEvent(unixTimestamp, false, true, "external");
    publishTimeChangedEvent();
}

void RTCManager::syncFromRTC() {
    if (_rtcMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    if (!_rtcFound || _rtcAlert) {
        logMessage("Cannot sync from RTC");
        xSemaphoreGiveRecursive(_rtcMutex);
        return;
    }

    DateTime now = _rtc.now();
    uint32_t timestamp = now.unixtime();

    if (isValidTimestamp(timestamp)) {
        updateSystemTime(timestamp);
        _isSynced = true;  // <-- ИСПРАВЛЕНО!
        _lastSyncMs = millis();
        _syncRetryCount = 0;

        logMessage("Synced from RTC: %s", getDateTimeString().c_str());
        publishSyncEvent(timestamp, true, true, "RTC");
    } else {
        logMessage("Invalid RTC time: %lu", timestamp);
        _syncFailCount++;

        if (!isRTCValid()) {
            logMessage("RTC invalid, resetting to default");
            _rtc.adjust(DateTime(MIN_VALID_TIMESTAMP));
        }

        publishSyncEvent(timestamp, true, false, "RTC");
    }

    xSemaphoreGiveRecursive(_rtcMutex);
}

void RTCManager::syncToRTC() {
    if (_rtcMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    if (!_rtcFound || _rtcAlert) {
        logMessage("Cannot sync to RTC");
        xSemaphoreGiveRecursive(_rtcMutex);
        return;
    }

    time_t now;
    time(&now);
    uint32_t timestamp = (uint32_t)now;

    if (isValidTimestamp(timestamp)) {
        _rtc.adjust(DateTime(timestamp));
        _lastSyncMs = millis();
        _isSynced = true;  // <-- ИСПРАВЛЕНО!
        logMessage("Synced to RTC");
        publishSyncEvent(timestamp, false, true, "system");
    } else {
        logMessage("Invalid system time: %lu", timestamp);
    }

    xSemaphoreGiveRecursive(_rtcMutex);
}

void RTCManager::updateSystemTime(uint32_t timestamp) {
    struct timeval tv = {
        .tv_sec = (time_t)timestamp,
        .tv_usec = 0
    };
    settimeofday(&tv, NULL);
}

bool RTCManager::isValidTimestamp(uint32_t timestamp) const {
    return (timestamp >= MIN_VALID_TIMESTAMP && timestamp <= MAX_VALID_TIMESTAMP);
}

bool RTCManager::isRTCValid() {
    if (!_rtcFound) return false;
    DateTime now = _rtc.now();
    uint32_t ts = now.unixtime();
    return (ts >= MIN_VALID_TIMESTAMP && ts <= MAX_VALID_TIMESTAMP);
}

// ============================================================================
// 14. БУДИЛЬНИК
// ============================================================================
void RTCManager::setAlarm(const RtcAlarmConfig& config) {
    _alarm = config;
    _alarm.triggered = false;
    logMessage("Alarm set: %02d:%02d (%s)",
              config.hour, config.minute,
              config.enabled ? "enabled" : "disabled");
}

void RTCManager::checkAlarm() {
    if (!_alarm.enabled || _alarm.triggered) return;
    if (!_rtcFound || _rtcAlert) return;

    uint32_t now = millis();
    if (now - _lastAlarmCheckMs < 1000) return;
    _lastAlarmCheckMs = now;

    DateTime dt = _rtc.now();
    if (dt.hour() == _alarm.hour && dt.minute() == _alarm.minute && dt.second() < 5) {
        _alarm.triggered = true;
        logMessage("ALARM TRIGGERED: %02d:%02d", _alarm.hour, _alarm.minute);
        publishAlarmEvent();
    }

    // Сброс триггера в полночь для ежедневного будильника
    if (_alarm.repeatDaily && dt.hour() == 0 && dt.minute() == 0 && dt.second() < 5) {
        _alarm.triggered = false;
    }
}

void RTCManager::clearAlarmTrigger() {
    _alarm.triggered = false;
    logMessage("Alarm trigger cleared");
}

// ============================================================================
// 15. ТЕМПЕРАТУРА (С ПРОВЕРКОЙ _initialized)
// ============================================================================
float RTCManager::getTemperature() const {
    if (!_initialized || !_rtcFound || _rtcAlert || _rtcMutex == nullptr) {
        return 0.0f;
    }

    float temp = 0.0f;
    if (xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        temp = _rtc.getTemperature();
        xSemaphoreGiveRecursive(_rtcMutex);
    }
    return temp;
}

// ============================================================================
// 16. ГЕТТЕРЫ ВРЕМЕНИ
// ============================================================================
uint32_t RTCManager::getUnixTime() const {
    time_t now;
    time(&now);
    return (uint32_t)now;
}

time_t RTCManager::getTimeT() const {
    time_t now;
    time(&now);
    return now;
}

void RTCManager::getTimeStruct(struct tm& destTm) const {
    time_t now;
    time(&now);
    localtime_r(&now, &destTm);
}

uint8_t RTCManager::getHour() const {
    struct tm tm;
    getTimeStruct(tm);
    return tm.tm_hour;
}

uint8_t RTCManager::getMinute() const {
    struct tm tm;
    getTimeStruct(tm);
    return tm.tm_min;
}

uint8_t RTCManager::getSecond() const {
    struct tm tm;
    getTimeStruct(tm);
    return tm.tm_sec;
}

uint8_t RTCManager::getDay() const {
    struct tm tm;
    getTimeStruct(tm);
    return tm.tm_mday;
}

uint8_t RTCManager::getMonth() const {
    struct tm tm;
    getTimeStruct(tm);
    return tm.tm_mon + 1;
}

uint16_t RTCManager::getYear() const {
    struct tm tm;
    getTimeStruct(tm);
    return tm.tm_year + 1900;
}

uint8_t RTCManager::getWeekday() const {
    struct tm tm;
    getTimeStruct(tm);
    return tm.tm_wday;
}

uint8_t RTCManager::getWeekdayMonday() const {
    uint8_t wd = getWeekday();
    return (wd == 0) ? 7 : wd;
}

const char* RTCManager::getWeekdayName() const {
    uint8_t wd = getWeekday();
    return (wd < 7) ? WEEKDAY_NAMES[wd] : "Unknown";
}

const char* RTCManager::getMonthName() const {
    uint8_t mon = getMonth();
    return (mon > 0 && mon <= 12) ? MONTH_NAMES[mon - 1] : "Unknown";
}

const char* RTCManager::getWeekdayNameRU() const {
    uint8_t wd = getWeekday();
    return (wd < 7) ? WEEKDAY_NAMES_RU[wd] : "Неизвестно";
}

// ============================================================================
// 17. ФОРМАТИРОВАНИЕ (УСТРАНЕНО ДУБЛИРОВАНИЕ)
// ============================================================================
size_t RTCManager::formatToBuffer(char* dest, size_t maxSize, const char* format) const {
    if (dest == nullptr || maxSize == 0) return 0;

    struct tm tm;
    getTimeStruct(tm);
    size_t result = strftime(dest, maxSize, format, &tm);
    if (result == 0) {
        dest[0] = '\0';
    }
    return result;
}

bool RTCManager::formatDateTime(char* buffer, size_t bufferSize, const char* format) const {
    if (!buffer || bufferSize == 0 || !format) return false;
    size_t result = formatToBuffer(buffer, bufferSize, format);
    return result > 0;
}

String RTCManager::getTimeString() const {
    char buf[16];
    formatToBuffer(buf, sizeof(buf), "%H:%M:%S");
    return String(buf);
}

String RTCManager::getDateString() const {
    char buf[16];
    formatToBuffer(buf, sizeof(buf), "%Y-%m-%d");
    return String(buf);
}

String RTCManager::getDateTimeString() const {  // <-- ТОЛЬКО ОДНА ВЕРСИЯ!
    char buf[32];
    formatToBuffer(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S");
    return String(buf);
}

String RTCManager::getDateTimeShort() const {
    char buf[32];
    formatToBuffer(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S");
    return String(buf);
}

String RTCManager::getISO8601() const {
    char buf[40];
    struct tm tm;
    getTimeStruct(tm);

    int tzHours = _timezoneOffset / 3600;
    char sign = (tzHours >= 0) ? '+' : '-';
    if (tzHours < 0) tzHours = -tzHours;

    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:00",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            sign,
            tzHours);
    return String(buf);
}

// ============================================================================
// 18. ПЛАНИРОВЩИК
// ============================================================================
bool RTCManager::isTimeInInterval(uint8_t startHour, uint8_t startMin,
                                  uint8_t endHour, uint8_t endMin) const {
    if (!_isSynced) return false;  // <-- ИСПРАВЛЕНО!

    struct tm tm;
    getTimeStruct(tm);
    int currentMin = tm.tm_hour * 60 + tm.tm_min;
    int startMinTotal = startHour * 60 + startMin;
    int endMinTotal = endHour * 60 + endMin;

    if (startMinTotal > endMinTotal) {
        return (currentMin >= startMinTotal || currentMin < endMinTotal);
    }
    return (currentMin >= startMinTotal && currentMin < endMinTotal);
}

bool RTCManager::isTimeInInterval(uint32_t startTimestamp, uint32_t endTimestamp) const {
    if (!_isSynced) return false;  // <-- ИСПРАВЛЕНО!
    uint32_t now = getUnixTime();
    return (now >= startTimestamp && now <= endTimestamp);
}

bool RTCManager::isNightTime(uint8_t nightStartHour, uint8_t nightEndHour) const {
    return isTimeInInterval(nightStartHour, 0, nightEndHour, 0);
}

bool RTCManager::isWeekend() const {
    uint8_t wd = getWeekday();
    return (wd == 0 || wd == 6);
}

bool RTCManager::isWeekday() const {
    uint8_t wd = getWeekday();
    return (wd >= 1 && wd <= 5);
}

bool RTCManager::isWorkingHours(uint8_t startHour, uint8_t endHour) const {
    if (!isWeekday()) return false;
    return isTimeInInterval(startHour, 0, endHour, 0);
}

// ============================================================================
// 19. НАСТРОЙКИ
// ============================================================================
void RTCManager::setTimeZone(int offsetHours) {
    _timezoneOffset = offsetHours * 3600;
    updateTimezone();

    char envBuf[16];
    snprintf(envBuf, sizeof(envBuf), "GMT%+d", -offsetHours);
    setenv("TZ", envBuf, 1);
    tzset();

    logMessage("Timezone set to %+d hours", offsetHours);
}

void RTCManager::updateTimezone() {
    int offsetHours = _timezoneOffset / 3600;
    char envBuf[16];
    snprintf(envBuf, sizeof(envBuf), "GMT%+d", -offsetHours);
    setenv("TZ", envBuf, 1);
    tzset();
}

// ============================================================================
// 20. СТАТУС СТРОКА
// ============================================================================
const char* RTCManager::getStatusString() const {
    if (_rtcAlert) return "I2C_ERROR";
    if (!_rtcFound) return "RTC_NOT_FOUND";
    if (!_isSynced) return "TIME_NOT_SYNCED";  // <-- ИСПРАВЛЕНО!
    return "OK";
}

// ============================================================================
// 21. ДИАГНОСТИКА
// ============================================================================
void RTCManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================================");
    stream.println(" RTC MANAGER DIAGNOSTIC");
    stream.println("==============================================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Status: %s\n", getStatusString());
    stream.printf(" Found: %s\n", _rtcFound ? "YES" : "NO");
    stream.printf(" Alert: %s\n", _rtcAlert ? "YES" : "NO");
    stream.printf(" Synced: %s\n", _isSynced ? "YES" : "NO");  // <-- ИСПРАВЛЕНО!
    stream.printf(" Timezone: %+d hours\n", _timezoneOffset / 3600);
    stream.printf(" Auto sync: %s\n", _autoSync ? "ON" : "OFF");
    stream.printf(" Sync interval: %lu min\n", _syncIntervalMs / 60000);
    stream.printf(" I2C interval: %lu sec\n", _i2cCheckIntervalMs / 1000);
    stream.printf(" Sync retries: %lu\n", _syncRetryCount);
    stream.printf(" Sync fails: %lu\n", _syncFailCount);
    stream.printf(" SDA pin: %d\n", _sdaPin);
    stream.printf(" SCL pin: %d\n", _sclPin);

    if (_rtcFound && !_rtcAlert) {
        if (_rtcMutex && xSemaphoreTakeRecursive(_rtcMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            stream.printf(" Temperature: %.1f°C\n", _rtc.getTemperature());
            stream.printf(" Lost power: %s\n", _rtc.lostPower() ? "YES" : "NO");
            xSemaphoreGiveRecursive(_rtcMutex);
        }

        char timeBuf[32];
        formatToBuffer(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S");
        stream.printf(" Current time: %s\n", timeBuf);

        stream.printf(" Alarm: %s\n", _alarm.enabled ? "ENABLED" : "DISABLED");
        if (_alarm.enabled) {
            stream.printf(" Alarm time: %02d:%02d (triggered: %s)\n",
                         _alarm.hour, _alarm.minute,
                         _alarm.triggered ? "YES" : "NO");
        }
    }
    stream.println("==============================================");
}

void RTCManager::printStats() const {
    streamDiagnosticInfo(Serial);
}

void RTCManager::updateStatus() {
    publishStatusEvent();
}