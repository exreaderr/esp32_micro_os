// ============================================================================
// PazManager.h - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Противоаварийная защита (ПАЗ) - мониторинг и управление
// критическими параметрами системы. Все события публикуются в шину событий.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНА работа с ping (используется esp_ping)
// - ИСПРАВЛЕНА инициализация I2C (исправлен синтаксис)
// - ИСПРАВЛЕНА работа с температурой (использован temperatureRead)
// - ИСПРАВЛЕНА ошибка в restoreI2cBus
// - ДОБАВЛЕНА полная потокобезопасность
// - ДОБАВЛЕНА защита от повторного входа
// - УЛУЧШЕНА обработка ошибок
// ============================================================================
#pragma once

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <Wire.h>
#include <ETH.h>
#include <ping/ping_sock.h>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

// Только IModule, без AppCore!
#include "core/IModule.h"

// ============================================================================
// 1. КОНСТАНТЫ
// ============================================================================
#define PAZ_DEFAULT_WDT_TIMEOUT_MS 10000
#define PAZ_DEFAULT_PING_COUNT 3
#define PAZ_DEFAULT_PING_TIMEOUT_MS 3000
#define PAZ_DEFAULT_RECOVERY_TIMEOUT_MS 30000
#define PAZ_DEFAULT_TICK_INTERVAL_MS 250
#define PAZ_DEFAULT_I2C_CHECK_INTERVAL_MS 30000
#define PAZ_DEFAULT_MEMORY_CHECK_INTERVAL_MS 60000
#define PAZ_DEFAULT_PING_CHECK_INTERVAL_MS 45000
#define PAZ_DEFAULT_STEP_TIMEOUT_MS 120000
#define PAZ_DEFAULT_MIN_SAFE_HEAP 24576
#define PAZ_MAX_PANIC_HISTORY 10
#define PAZ_MAX_LOG_ENTRIES 20
#define PAZ_ACTUATOR_SAFETY_MARGIN_MS 2000
#define PAZ_TEMP_WARNING_THRESHOLD 70
#define PAZ_TEMP_CRITICAL_THRESHOLD 80
#define PAZ_TEMP_PANIC_THRESHOLD 90
#define PAZ_TEMP_CHECK_INTERVAL_MS 5000

// ============================================================================
// 2. СОБЫТИЯ PAZ MANAGER
// ============================================================================
enum PazEvents : int32_t {
    SH_EVENT_PAZ_PANIC = SH_EVENT_USER_BASE + 0x0A00,
    SH_EVENT_PAZ_PANIC_CLEARED = SH_EVENT_USER_BASE + 0x0A01,
    SH_EVENT_PAZ_STATUS_CHANGED = SH_EVENT_USER_BASE + 0x0A02,
    SH_EVENT_PAZ_WARNING = SH_EVENT_USER_BASE + 0x0A03,
    SH_EVENT_PAZ_ERROR = SH_EVENT_USER_BASE + 0x0A04,
    SH_EVENT_PAZ_CRITICAL = SH_EVENT_USER_BASE + 0x0A05,
    SH_EVENT_PAZ_ACTUATOR_STUCK = SH_EVENT_USER_BASE + 0x0A06,
    SH_EVENT_PAZ_SENSOR_STUCK = SH_EVENT_USER_BASE + 0x0A07,
    SH_EVENT_PAZ_SENSOR_NAN = SH_EVENT_USER_BASE + 0x0A08,
    SH_EVENT_PAZ_I2C_ERROR = SH_EVENT_USER_BASE + 0x0A09,
    SH_EVENT_PAZ_NETWORK_DOWN = SH_EVENT_USER_BASE + 0x0A0A,
    SH_EVENT_PAZ_MEMORY_CRITICAL = SH_EVENT_USER_BASE + 0x0A0B,
    SH_EVENT_PAZ_WDT_RESET = SH_EVENT_USER_BASE + 0x0A0C,
    SH_EVENT_PAZ_RECOVERY_START = SH_EVENT_USER_BASE + 0x0A0D,
    SH_EVENT_PAZ_RECOVERY_SUCCESS = SH_EVENT_USER_BASE + 0x0A0E,
    SH_EVENT_PAZ_RECOVERY_FAILED = SH_EVENT_USER_BASE + 0x0A0F,
    SH_EVENT_PAZ_LOG = SH_EVENT_USER_BASE + 0x0A10
};

// ============================================================================
// 3. ТИПЫ И СТРУКТУРЫ
// ============================================================================
enum class PazStatus : uint8_t {
    OK = 0,
    WARNING = 1,
    ERROR = 2,
    CRITICAL = 3,
    PANIC = 4
};

enum class PazDiagnosticCode : uint8_t {
    OK = 0,
    ACTUATOR_STICK = 1,
    MEMORY_LEAK = 2,
    SENSOR_STUCK = 3,
    SENSOR_NAN = 4,
    I2C_BUS_ERROR = 5,
    NETWORK_DOWN = 6,
    NETWORK_DEGRADATION = 7,
    WDT_RESET = 8,
    PANIC_TRIGGERED = 9
};

struct PazStatusEvent {
    uint8_t status;
    uint8_t code;
    uint32_t timestamp;
    char message[48];
    char details[48];
    bool panicActive;
    uint32_t panicCount;
};

struct PazPanicEvent {
    char reason[32];
    uint32_t timestamp;
    uint32_t uptime;
    uint32_t freeHeap;
    uint32_t freePsram;
    bool networkHealthy;
    uint8_t netStep;
};

struct PazLogEntry {
    uint32_t timestamp;
    PazDiagnosticCode code;
    char message[48];
    char details[48];
};

struct PanicHistoryEntry {
    uint32_t timestamp;
    char reason[32];
    uint32_t uptime;
    uint32_t freeHeap;
    uint32_t freePsram;
    bool networkHealthy;
};

// ============================================================================
// 4. ОСНОВНОЙ КЛАСС
// ============================================================================
/**
 * @brief Противоаварийная защита (ПАЗ)
 *
 * Синглтон. Обеспечивает:
 * - Мониторинг критических параметров системы
 * - Контроль залипания актуаторов
 * - Контроль залипания датчиков
 * - Контроль I2C шины
 * - Сетевую диагностику (ping)
 * - Мониторинг памяти
 * - Автоматическое восстановление
 * - Режим паники
 * - Полную потокобезопасность
 */
class PazManager : public IModule {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(const char* reason)> OnPazPanicCallback;
    typedef std::function<void(PazStatus status, PazDiagnosticCode code)> OnPazStatusChangeCallback;
    typedef std::function<void(const PazLogEntry& entry)> OnPazLogCallback;

    // === СИНГЛТОН ===
    static PazManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    PazManager();
    ~PazManager();

    // Запрещаем копирование
    PazManager(const PazManager&) = delete;
    PazManager& operator=(const PazManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "PazManager"; }
    const char* getVersion() const override { return "4.2.2"; }
    uint32_t getModuleId() const override { return MODULE_ID_PAZ; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ЖИЗНЕННЫЙ ЦИКЛ ===
    void begin(uint8_t sdaPin, uint8_t sclPin, uint32_t wdtTimeoutMs = PAZ_DEFAULT_WDT_TIMEOUT_MS);
    void end();
    void reset();

    // === WDT ===
    void resetWdt();
    void forceReset();

    // === РЕГИСТРАЦИЯ ПЕРИФЕРИИ ===
    void registerActuator(uint8_t pin, bool activeState, uint32_t maxActiveMs);
    void notifyActuatorSafeClose();
    void registerCriticalSensor(float* sensorPtr, uint32_t maxStuckTimeMs);
    void notifySensorUpdated();

    // === ОБРАТНАЯ СВЯЗЬ ОТ СКУД ===
    void notifyCardScanned();
    void notifyRelayActivated();
    void notifyRelayDeactivated();
    void notifyDoorOpened();
    void notifyDoorClosed();

    // === ГЕТТЕРЫ ===
    bool isPanicTripped() const { return _panicTripped; }
    bool isRelayAlert() const { return _relayAlert; }
    bool isI2cAlert() const { return _i2cAlert; }
    bool isMemoryAlert() const { return _memoryAlert; }
    bool isSensorAlert() const { return _sensorAlert; }
    bool isNetworkHealthy() const;
    uint8_t getNetStep() const;
    PazStatus getPazStatus() const { return _currentStatus; }
    PazDiagnosticCode getLastCode() const { return _lastCode; }
    const char* getStatusString() const;
    const char* getCodeString(PazDiagnosticCode code) const;
    uint32_t getPanicCount() const { return _panicCount; }
    uint32_t getWdtResets() const { return _wdtResets; }
    uint32_t getLastPanicTime() const { return _lastPanicTime; }
    const char* getLastPanicReason() const { return _lastPanicReason; }
    std::vector<PanicHistoryEntry> getPanicHistory(size_t limit = 10) const;

    // === КОЛБЭКИ ===
    void onPanic(OnPazPanicCallback cb) { _panicCb = cb; }
    void onStatusChange(OnPazStatusChangeCallback cb) { _statusCb = cb; }
    void onLog(OnPazLogCallback cb) { _logCb = cb; }

    // === НАСТРОЙКИ ===
    void setPanicOnWdt(bool enable) { _panicOnWdt = enable; }
    void setAutoRecovery(bool enable) { _autoRecovery = enable; }
    void setRecoveryTimeout(uint32_t ms) { _recoveryTimeoutMs = ms; }
    void setNetworkCheckInterval(uint32_t ms) { _pingCheckIntervalMs = ms; }
    void setMemoryThreshold(size_t bytes) { _minSafeHeapBytes = bytes; }
    void setTickInterval(uint32_t ms) { _pazTickIntervalMs = ms; }
    void setTemperatureThresholds(uint32_t warning, uint32_t critical, uint32_t panic) {
        _tempWarningThreshold = warning;
        _tempCriticalThreshold = critical;
        _tempPanicThreshold = panic;
    }

    // === ТЕМПЕРАТУРА ===
    float getCpuTemperature() const { return _cpuTemperature; }
    float getMaxCpuTemperature() const { return _maxCpuTemperature; }
    void resetMaxTemperature() { _maxCpuTemperature = _cpuTemperature; }

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    void runActuatorProtection();
    void runMemoryAndFsProtection();
    void runSensorProtection();
    void runI2cRecovery();
    void restoreI2cBus();
    void runNetworkDiagnostics();
    void triggerPanic(const char* reason);
    void updateStatus(PazStatus newStatus, PazDiagnosticCode code, const char* msg = "");
    void addLog(PazDiagnosticCode code, const char* msg, const char* details = "");
    void addPanicHistory(const char* reason);
    void checkRecovery();
    void performRecovery();
    void checkCpuTemperature();
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    bool isInitializedAndReady() const;

    // === ОТПРАВКА СОБЫТИЙ ===
    void publishStatusEvent(PazStatus status, PazDiagnosticCode code);
    void publishPanicEvent(const char* reason);
    void publishLogEvent(const PazLogEntry& entry);
    void publishErrorEvent(const char* errorCode);

    // === ОБРАБОТЧИКИ СОБЫТИЙ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === СТАТИЧЕСКИЙ КОЛБЭК ДЛЯ LWIP ===
    static void pingEndCallback(esp_ping_handle_t hdl, void* args);

    // === ДАННЫЕ ===
    uint8_t _sdaPin = 0;
    uint8_t _sclPin = 0;
    uint8_t _actuatorPin = 255;
    bool _actuatorActiveState = HIGH;
    uint32_t _maxActiveMs = 0;
    uint32_t _openedTimestamp = 0;
    uint32_t _actuatorCheckCount = 0;
    float* _criticalSensorPtr = nullptr;
    float _lastSensorValue = 0.0f;
    uint32_t _maxSensorStuckTimeMs = 0;
    uint32_t _lastSensorChangeTimestamp = 0;
    uint32_t _sensorCheckCount = 0;

    // === ДАТЧИК ТЕМПЕРАТУРЫ КРИСТАЛЛА ===
    float _cpuTemperature = 0.0f;
    float _maxCpuTemperature = 0.0f;
    uint32_t _lastTempCheckMs = 0;
    uint32_t _tempCheckIntervalMs = PAZ_TEMP_CHECK_INTERVAL_MS;
    uint32_t _tempWarningThreshold = PAZ_TEMP_WARNING_THRESHOLD;
    uint32_t _tempCriticalThreshold = PAZ_TEMP_CRITICAL_THRESHOLD;
    uint32_t _tempPanicThreshold = PAZ_TEMP_PANIC_THRESHOLD;

    // === АТОМАРНЫЕ ФЛАГИ ===
    volatile bool _networkHealthy = true;
    volatile uint8_t _netStep = 0;
    volatile uint32_t _stepTimerMs = 0;
    uint32_t _lastPingCheckMs = 0;
    uint32_t _pingSuccessCount = 0;
    uint32_t _pingFailCount = 0;
    uint32_t _lastPazTicksMs = 0;
    uint32_t _lastI2cCheckMs = 0;
    uint32_t _lastMemoryCheckMs = 0;
    uint32_t _lastRecoveryAttemptMs = 0;
    uint32_t _startTimeMs = 0;
    uint32_t _lastCardScanTime = 0;
    uint32_t _lastRelayActivationTime = 0;
    uint32_t _lastDoorEventTime = 0;
    uint32_t _moduleId = MODULE_ID_PAZ;

    // === СИСТЕМНЫЕ ФЛАГИ ===
    volatile bool _relayAlert = false;
    volatile bool _i2cAlert = false;
    volatile bool _memoryAlert = false;
    volatile bool _sensorAlert = false;
    volatile bool _panicTripped = false;
    bool _wdtEnabled = false;
    bool _initialized = false;
    bool _recoveryInProgress = false;
    bool _panicOnWdt = true;
    bool _autoRecovery = true;
    bool _initInProgress = false;

    PazStatus _currentStatus = PazStatus::OK;
    PazDiagnosticCode _lastCode = PazDiagnosticCode::OK;
    uint32_t _panicCount = 0;
    uint32_t _wdtResets = 0;
    uint32_t _lastPanicTime = 0;
    char _lastPanicReason[32] = "none";

    std::vector<PanicHistoryEntry> _panicHistory;
    std::vector<PazLogEntry> _logEntries;

    // === НАСТРОЙКИ ===
    uint32_t _recoveryTimeoutMs = PAZ_DEFAULT_RECOVERY_TIMEOUT_MS;
    uint32_t _pazTickIntervalMs = PAZ_DEFAULT_TICK_INTERVAL_MS;
    uint32_t _i2cCheckIntervalMs = PAZ_DEFAULT_I2C_CHECK_INTERVAL_MS;
    uint32_t _memoryCheckIntervalMs = PAZ_DEFAULT_MEMORY_CHECK_INTERVAL_MS;
    uint32_t _pingCheckIntervalMs = PAZ_DEFAULT_PING_CHECK_INTERVAL_MS;
    uint32_t _stepTimeoutMs = PAZ_DEFAULT_STEP_TIMEOUT_MS;
    size_t _minSafeHeapBytes = PAZ_DEFAULT_MIN_SAFE_HEAP;
    uint8_t _pingCount = PAZ_DEFAULT_PING_COUNT;
    uint32_t _pingTimeoutMs = PAZ_DEFAULT_PING_TIMEOUT_MS;

    // === ПИНГ ===
    esp_ping_handle_t _pendingPingHandle = nullptr;
    volatile bool _pingDone = false;

    // === МЬЮТЕКС (РЕКУРСИВНЫЙ!) ===
    SemaphoreHandle_t _pazMutex = nullptr;
    static portMUX_TYPE _pazIsrMux;

    // === КОЛБЭКИ ===
    OnPazPanicCallback _panicCb = nullptr;
    OnPazStatusChangeCallback _statusCb = nullptr;
    OnPazLogCallback _logCb = nullptr;

    // === КОНСТАНТЫ ===
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
};

// #endif // PAZMANAGER_H