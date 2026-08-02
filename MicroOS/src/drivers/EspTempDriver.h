// ============================================================================
// EspTempDriver.h — ДАТЧИК ТЕМПЕРАТУРЫ КРИСТАЛЛА ESP32
// ============================================================================
// Фаза 1. ПЕРВЫЙ драйвер МикроОС 5.0 — прогоняет всю цепочку:
//   драйвер -> poll() -> EventBus -> подписчики (HealthMonitor, Telemetry).
//
// Принципиально важно (базовая архитектура, П2): в v4.2.2 температуру
// кристалла читали ДВА модуля (TempSensorManager и PazManager) с разными
// порогами. Здесь — ЕДИНСТВЕННЫЙ источник: драйвер публикует факт
// (температуру), а пороги и вердикты — дело сервисов-подписчиков.
// Драйвер лишь помогает гистерезисом, чтобы не спамить событиями.
//
// Железо: внутренний сенсор ESP32 (ESP-IDF temperature_sensor).
// ============================================================================
#pragma once

#include "../core/IDeviceDriver.h"
#include "../core/EventBus.h"

// Период и пороги по умолчанию (Phase 2: переопределяются полями
// конфигурации sys.temp_*). Пороги — из монолита v2.5.0 (ПАЗ температуры).
constexpr uint32_t ESP_TEMP_POLL_MS      = 5000;   // опрос раз в 5 с
constexpr float    ESP_TEMP_WARN_C       = 60.0f;  // предупреждение
constexpr float    ESP_TEMP_CRIT_C       = 75.0f;  // критическая
constexpr float    ESP_TEMP_HYSTERESIS_C = 2.0f;   // гистерезис возврата

/// Состояния по порогам — драйвер публикует только ПЕРЕХОДЫ, не каждый poll.
enum class TempState : uint8_t { Normal, Warning, Critical };

class EspTempDriver : public IDeviceDriver {
public:
    static EspTempDriver& getInstance();

    // --- IDeviceDriver ---------------------------------------------------
    const char* driverName() const override { return "esp_temp"; }
    bool init() override;
    void poll() override;
    uint32_t getPollIntervalMs() const override { return ESP_TEMP_POLL_MS; }
    bool isHealthy() const override { return _healthy; }

    // --- СИНХРОННЫЙ ДОСТУП (для сервисов: HealthMonitor, /api/system) ------
    /// Последнее измерение, °C. NAN — сенсор недоступен.
    float getTemperature() const { return _lastTempC; }
    TempState getState() const { return _state; }

    /// Переопределение порогов (sys.temp_warn_c/sys.temp_crit_c — Phase 2,
    /// вызывает TelemetryService из конфига при старте). Санитизация:
    /// warn ∈ [30, 100], crit ∈ [warn+5, 125] — иначе игнор (умолчания).
    void setThresholds(float warnC, float critC) {
        if (warnC >= 30.0f && warnC <= 100.0f &&
            critC >= warnC + 5.0f && critC <= 125.0f) {
            _warnC = warnC;
            _critC = critC;
        }
    }

private:
    EspTempDriver() = default;

    /// Публикация перехода состояния (WARNING/CRITICAL/возврат в Normal).
    void publishTransition(TempState newState, float tempC);

    float     _lastTempC = NAN;
    TempState _state = TempState::Normal;
    bool      _healthy = false;
    // Пороги — поля (умолчания = константы выше; см. setThresholds)
    float     _warnC = ESP_TEMP_WARN_C;
    float     _critC = ESP_TEMP_CRIT_C;
    // Хэндла сенсора нет: чтение через Arduino API temperatureRead()
    // (новый ESP-IDF tsens-драйвер не собран для классического ESP32 —
    // выявлено линковкой на core 3.3.11).
};
