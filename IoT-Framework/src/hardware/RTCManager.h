// ============================================================================
// RTCManager.h - ULTIMATE MICRO-OS V5.0 (ADAPTED)
// ============================================================================
// Описание: Менеджер часов реального времени DS3231.
//           Адаптирован для новой событийной архитектуры МикроОС v5.0.
//
// ИЗМЕНЕНИЯ v5.0:
// - Сохранена 100% функциональность v4.2.2
// - Добавлена NTP-синхронизация (EVENT_NET_CONNECTED)
// - Добавлено событие EVENT_TIME_TICK (публикация времени)
// - Добавлена коррекция хода (drift compensation)
// - Расширена диагностика
// - Улучшена потокобезопасность
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>
#include <sys/time.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

#include "core/IModule.h"
#include "core/ShEventData.h"

// ============================================================================
// 1. СОБЫТИЯ RTC MANAGER (РАСШИРЕНЫ)
// ============================================================================
enum RtcEvents : int32_t {
    SH_EVENT_RTC_SYNCED = SH_EVENT_USER_BASE + 0x0400,
    SH_EVENT_RTC_FOUND = SH_EVENT_USER_BASE + 0x0401,
    SH_EVENT_RTC_LOST = SH_EVENT_USER_BASE + 0x0402,
    SH_EVENT_RTC_RECOVERED = SH_EVENT_USER_BASE + 0x0403,
    SH_EVENT_RTC_ERROR = SH_EVENT_USER_BASE + 0x0404,
    SH_EVENT_RTC_TIME_CHANGED = SH_EVENT_USER_BASE + 0x0405,
    SH_EVENT_RTC_ALARM_TRIGGERED = SH_EVENT_USER_BASE + 0x0406,
    // НОВОЕ: публикация тика времени
    SH_EVENT_RTC_TICK = SH_EVENT_USER_BASE + 0x0407
};

// ============================================================================
// 2. СТРУКТУРЫ ДАННЫХ (ВАШИ, СОХРАНЕНЫ)
// ============================================================================
struct RtcSyncEvent {
    uint32_t timestamp;
    uint32_t uptime;
    int timezoneOffset;
    bool fromHardware;
    bool success;
    char source[32];
};

struct RtcStatusEvent {
    bool found;
    bool alert;
    bool synced;
    float temperature;
    uint32_t lastSyncTime;
    char errorCode[32];
};

struct RtcAlarmConfig {
    bool enabled = false;
    bool repeatDaily = true;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    bool triggered = false;
};

// ============================================================================
// 3. ОСНОВНОЙ КЛАСС (РАСШИРЕН)
// ============================================================================
class RTCManager : public IModule {
public:
    // === КОЛБЭКИ (ВАШИ) ===
    typedef std::function<void(uint32_t timestamp)> OnTimeSyncCallback;
    typedef std::function<void(bool found)> OnRTCStatusCallback;
    typedef std::function<void(const char* error)> OnRTCErrorCallback;
    typedef std::function<void(const RtcAlarmConfig& alarm)> OnAlarmTriggerCallback;

    // === СИНГЛТОН (ВАШ) ===
    static RTCManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ) ===
    RTCManager();
    ~RTCManager();

    RTCManager(const RTCManager&) = delete;
    RTCManager& operator=(const RTCManager&) = delete;

    // === IModule (ВАШ, ДОПОЛНЕН) ===
    const char* getName() const override { return "RTCManager"; }
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_RTC; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized && _rtcFound && !_rtcAlert; }
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ИНИЦИАЛИЗАЦИЯ (ВАШ) ===
    void begin(uint8_t sdaPin = 32, uint8_t sclPin = 33, int timezoneOffsetSec = 0);
    void end();
    void reset();

    // === СИНХРОНИЗАЦИЯ (ВАША, ДОПОЛНЕНА) ===
    void syncTime(uint32_t unixTimestamp);
    void syncFromRTC();
    void syncToRTC();
    bool isTimeSynced() const { return _isSynced; }
    uint32_t getLastSyncTime() const { return _lastSyncMs; }

    // НОВОЕ: NTP-синхронизация
    void setNtpServer(const char* server);
    bool syncNTP();

    // === ГЕТТЕРЫ ВРЕМЕНИ (ВАШИ) ===
    uint32_t getUnixTime() const;
    time_t getTimeT() const;
    void getTimeStruct(struct tm& destTm) const;
    uint8_t getHour() const;
    uint8_t getMinute() const;
    uint8_t getSecond() const;
    uint8_t getDay() const;
    uint8_t getMonth() const;
    uint16_t getYear() const;
    uint8_t getWeekday() const;
    uint8_t getWeekdayMonday() const;
    const char* getWeekdayName() const;
    const char* getMonthName() const;
    const char* getWeekdayNameRU() const;

    // === ФОРМАТИРОВАНИЕ (ВАШЕ) ===
    size_t formatToBuffer(char* dest, size_t maxSize, const char* format) const;
    bool formatDateTime(char* buffer, size_t bufferSize, const char* format) const;
    String getTimeString() const;
    String getDateString() const;
    String getDateTimeString() const;
    String getDateTimeShort() const;
    String getISO8601() const;

    // === ПЛАНИРОВЩИК (ВАШ) ===
    bool isTimeInInterval(uint8_t startHour, uint8_t startMin,
                         uint8_t endHour, uint8_t endMin) const;
    bool isTimeInInterval(uint32_t startTimestamp, uint32_t endTimestamp) const;
    bool isNightTime(uint8_t nightStartHour = 23, uint8_t nightEndHour = 7) const;
    bool isWeekend() const;
    bool isWeekday() const;
    bool isWorkingHours(uint8_t startHour = 9, uint8_t endHour = 18) const;

    // === ТЕМПЕРАТУРА (ВАША) ===
    float getTemperature() const;

    // === БУДИЛЬНИК (ВАШ) ===
    void setAlarm(const RtcAlarmConfig& config);
    RtcAlarmConfig getAlarm() const { return _alarm; }
    void enableAlarm(bool enable) { _alarm.enabled = enable; }
    void clearAlarmTrigger();

    // === ДИАГНОСТИКА (ВАША, РАСШИРЕНА) ===
    bool isHardwareRTCFound() const { return _rtcFound; }
    const char* getStatusString() const;
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

    // === КОЛБЭКИ (ВАШИ) ===
    void setOnTimeSync(OnTimeSyncCallback cb) { _onTimeSync = cb; }
    void setOnRTCStatus(OnRTCStatusCallback cb) { _onRTCStatus = cb; }
    void setOnRTCError(OnRTCErrorCallback cb) { _onRTCError = cb; }
    void setOnAlarmTrigger(OnAlarmTriggerCallback cb) { _onAlarmTrigger = cb; }

    // === НАСТРОЙКИ (ВАШИ, РАСШИРЕНЫ) ===
    void setTimeZone(int offsetHours);
    int getTimeZone() const { return _timezoneOffset / 3600; }
    void setAutoSync(bool enable) { _autoSync = enable; }
    bool isAutoSync() const { return _autoSync; }
    void setSyncInterval(uint32_t intervalMs) { _syncIntervalMs = intervalMs; }
    void setI2cCheckInterval(uint32_t intervalMs) { _i2cCheckIntervalMs = intervalMs; }

    // НОВОЕ: Коррекция хода
    void setDriftCompensation(int32_t ppm) { _driftCompensationPpm = ppm; }
    int32_t getDriftCompensation() const { return _driftCompensationPpm; }

    // НОВОЕ: Включить/выключить публикацию EVENT_TIME_TICK
    void enableTickPublish(bool enable) { _publishTick = enable; }

private:
    // === ВНУТРЕННИЕ МЕТОДЫ (ВАШИ, ДОПОЛНЕНЫ) ===
    void initRTC();
    void checkI2C();
    void recoverI2C();
    void updateSystemTime(uint32_t timestamp);
    bool isValidTimestamp(uint32_t timestamp) const;
    bool isRTCValid();
    void updateStatus();
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    void checkAlarm();
    void updateTimezone();
    void publishTick(); // НОВОЕ

    // === ОТПРАВКА СОБЫТИЙ (ВАША, РАСШИРЕНА) ===
    void publishSyncEvent(uint32_t timestamp, bool fromHardware, bool success, const char* source);
    void publishStatusEvent();
    void publishErrorEvent(const char* errorCode);
    void publishTimeChangedEvent();
    void publishAlarmEvent();
    void publishTickEvent(const SystemTime& time); // НОВОЕ

    // === ОБРАБОТЧИКИ СОБЫТИЙ (ВАШИ) ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ДАННЫЕ (ВАШИ, РАСШИРЕНЫ) ===
    mutable RTC_DS3231 _rtc;
    uint8_t _sdaPin = 32;
    uint8_t _sclPin = 33;
    uint32_t _moduleId = MODULE_ID_RTC;

    volatile bool _rtcFound = false;
    volatile bool _rtcAlert = false;
    volatile bool _isSynced = false;
    bool _autoSync = true;
    bool _initialized = false;
    bool _publishTick = true; // НОВОЕ
    int _timezoneOffset = 0;
    uint32_t _lastSyncMs = 0;
    uint32_t _lastI2cCheckMs = 0;
    uint32_t _syncIntervalMs = 3600000;
    uint32_t _i2cCheckIntervalMs = 30000;
    uint32_t _syncRetryCount = 0;
    uint32_t _syncFailCount = 0;
    uint32_t _lastAlarmCheckMs = 0;
    uint32_t _lastTickPublishMs = 0;
    uint32_t _tickIntervalMs = 1000;
    bool _beginInProgress = false;
    int32_t _driftCompensationPpm = 0; // НОВОЕ
    uint32_t _totalTicks = 0; // НОВОЕ

    // NTP (НОВОЕ)
    char _ntpServer[32] = "pool.ntp.org";
    uint32_t _ntpSyncIntervalMs = 3600000;
    uint32_t _lastNtpSyncMs = 0;
    uint32_t _ntpSyncAttempts = 0;
    uint32_t _failedNtpSyncs = 0;

    SemaphoreHandle_t _rtcMutex = nullptr;
    RtcAlarmConfig _alarm;

    // === КОЛБЭКИ (ВАШИ) ===
    OnTimeSyncCallback _onTimeSync = nullptr;
    OnRTCStatusCallback _onRTCStatus = nullptr;
    OnRTCErrorCallback _onRTCError = nullptr;
    OnAlarmTriggerCallback _onAlarmTrigger = nullptr;

    // === КОНСТАНТЫ (ВАШИ) ===
    static constexpr uint8_t RTC_I2C_ADDR = 0x68;
    static constexpr uint32_t MIN_VALID_TIMESTAMP = 1735689600UL;
    static constexpr uint32_t MAX_VALID_TIMESTAMP = 4102444800UL;
    static constexpr uint32_t SYNC_RETRY_DELAY_MS = 5000;
    static constexpr uint8_t MAX_SYNC_RETRIES = 3;
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
    static constexpr uint32_t I2C_RECOVERY_DELAY_MS = 100;

    // === СТАТИЧЕСКИЕ МАССИВЫ (ВАШИ) ===
    static const char* WEEKDAY_NAMES[7];
    static const char* MONTH_NAMES[12];
    static const char* WEEKDAY_NAMES_RU[7];
};