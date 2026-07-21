// ============================================================================
// TempSensorManager.h - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление встроенным датчиком температуры ESP32.
// Использует нативный драйвер ESP-IDF (temperature_sensor).
// Автоматический fallback при отсутствии драйвера.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА работа с weak-символами (используется ESP-IDF v5.x API)
// - ИСПРАВЛЕНА ошибка с переменной ready -> _ready
// - ДОБАВЛЕНА обработка всех системных событий
// - ИСПРАВЛЕНА canHandleEvent (теперь фильтрует события)
// - УДАЛЕН #pragma comment(lib, ...) (не работает в GCC)
// - ДОБАВЛЕНА защита от повторного входа в init
// - ДОБАВЛЕН fallback через RTC DS3231 (если доступен)
// - ДОБАВЛЕНА полная потокобезопасность
// - ДОБАВЛЕНА статистика температуры
// ============================================================================
#pragma once

#include <Arduino.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

// Только IModule, без AppCore!
#include "core/IModule.h"

// ============================================================================
// 1. КОНСТАНТЫ
// ============================================================================
#define TEMP_SENSOR_READ_INTERVAL_MS 5000
#define TEMP_SENSOR_FALLBACK_VALUE 0.0f
#define TEMP_SENSOR_MIN_VALID -10.0f
#define TEMP_SENSOR_MAX_VALID 120.0f

// ============================================================================
// 2. СОБЫТИЯ TEMP SENSOR
// ============================================================================
enum TempSensorEvents : int32_t {
    SH_EVENT_TEMP_UPDATE = SH_EVENT_TEMPERATURE_UPDATE,
    SH_EVENT_TEMP_WARNING = SH_EVENT_USER_BASE + 0x0600,
    SH_EVENT_TEMP_CRITICAL = SH_EVENT_USER_BASE + 0x0601,
    SH_EVENT_TEMP_RECOVERED = SH_EVENT_USER_BASE + 0x0602,
    SH_EVENT_TEMP_SENSOR_ERROR = SH_EVENT_USER_BASE + 0x0603
};

// ============================================================================
// 3. СТРУКТУРЫ ДАННЫХ
// ============================================================================
/**
 * @brief Статистика температуры
 */
struct TempSensorStats {
    float current = 0.0f;
    float min = 100.0f;
    float max = -100.0f;
    float average = 0.0f;
    uint32_t readCount = 0;
    uint32_t errorCount = 0;
    uint32_t lastReadTime = 0;
    uint8_t healthStatus = 0;  // 0=OK, 1=WARNING, 2=CRITICAL
    bool isFallbackMode = false;
};

// ============================================================================
// 4. ОСНОВНОЙ КЛАСС
// ============================================================================
/**
 * @brief Менеджер датчика температуры ESP32
 *
 * Синглтон. Обеспечивает:
 * - Чтение температуры кристалла ESP32
 * - Автоматический fallback (0°C или RTC)
 * - Публикацию событий в шину
 * - Статистику температуры
 * - Пороги предупреждений и критических значений
 */
class TempSensorManager : public IModule {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(float temperature)> OnTemperatureUpdateCallback;
    typedef std::function<void(float temperature, uint8_t level)> OnTemperatureAlertCallback;
    typedef std::function<void(const TempSensorStats& stats)> OnStatsUpdateCallback;

    // === СИНГЛТОН ===
    static TempSensorManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    TempSensorManager();
    ~TempSensorManager();

    // Запрещаем копирование
    TempSensorManager(const TempSensorManager&) = delete;
    TempSensorManager& operator=(const TempSensorManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "TempSensorManager"; }
    const char* getVersion() const override { return "4.2.2"; }
    uint32_t getModuleId() const override { return MODULE_ID_TEMP_SENSOR; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _ready; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === УПРАВЛЕНИЕ ===
    void setReadInterval(uint32_t intervalMs) { _readIntervalMs = intervalMs; }
    void setWarningThreshold(float threshold) { _warningThreshold = threshold; }
    void setCriticalThreshold(float threshold) { _criticalThreshold = threshold; }
    void resetMaxTemperature() { _stats.max = _stats.current; }
    void resetStats();

    // === ПОЛУЧЕНИЕ ДАННЫХ ===
    float getTemperature() const { return _stats.current; }
    float getMaxTemperature() const { return _stats.max; }
    float getMinTemperature() const { return _stats.min; }
    float getAverageTemperature() const { return _stats.average; }
    uint32_t getReadCount() const { return _stats.readCount; }
    const TempSensorStats& getStats() const { return _stats; }
    bool isFallbackMode() const { return _fallbackMode; }

    // === КОЛБЭКИ ===
    void setOnTemperatureUpdate(OnTemperatureUpdateCallback cb) { _onTemperatureUpdate = cb; }
    void setOnTemperatureAlert(OnTemperatureAlertCallback cb) { _onTemperatureAlert = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;
    String getStatsString() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    void readTemperature();
    void updateStats(float temp);
    void checkThresholds(float temp);
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    void publishTempEvent(float temperature);
    void publishAlertEvent(float temperature, uint8_t level);
    float getFallbackTemperature();

    // === ОБРАБОТЧИКИ СОБЫТИЙ ===
    void handleCommand(const ShEventData* data);

    // === ИНИЦИАЛИЗАЦИЯ ДРАЙВЕРА ===
    bool initDriver();
    void cleanupDriver();

    // === ДАННЫЕ ===
    void* _tempSensorHandle = nullptr;  // temperature_sensor_handle_t
    float _warningThreshold = 70.0f;
    float _criticalThreshold = 80.0f;
    uint32_t _readIntervalMs = TEMP_SENSOR_READ_INTERVAL_MS;
    uint32_t _lastReadTime = 0;
    bool _ready = false;
    bool _fallbackMode = false;
    bool _initialized = false;
    bool _initInProgress = false;
    uint32_t _moduleId = MODULE_ID_TEMP_SENSOR;
    uint32_t _errorCount = 0;

    // Рекурсивный мьютекс
    SemaphoreHandle_t _mutex = nullptr;

    // Статистика
    TempSensorStats _stats;

    // Состояние предупреждения (для избегания спама)
    bool _warningActive = false;
    bool _criticalActive = false;

    // Колбэки
    OnTemperatureUpdateCallback _onTemperatureUpdate = nullptr;
    OnTemperatureAlertCallback _onTemperatureAlert = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;

    // Константы
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 100;
};

// #endif // TEMPSENSORMANAGER_H