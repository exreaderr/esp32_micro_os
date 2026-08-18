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
// конфигурации sys.temp_*). Пороги +15 к монолиту v2.5.0 (решение
// владельца 14.08.2026: датчик меряет кристалл, WT32 в корпусе с PHY
// законно живёт ~79 °C — ложный «критикал» обесценивал ПАЗ).
constexpr uint32_t ESP_TEMP_POLL_MS      = 5000;   // опрос раз в 5 с
constexpr float    ESP_TEMP_WARN_C       = 75.0f;  // предупреждение
constexpr float    ESP_TEMP_CRIT_C       = 90.0f;  // критическая
constexpr float    ESP_TEMP_PANIC_C      = 105.0f; // ТЕРМИЧЕСКАЯ ПАНИКА
                                                   // (залежь №2: «кристалл
                                                   // варится» — последний рубеж)
constexpr float    ESP_TEMP_HYSTERESIS_C = 2.0f;   // гистерезис возврата
// Бут-грейс переходов (урок ночи 14→15.08.2026: пороги из конфига
// применяет TelemetryService в init-фазе, а первый poll драйвера мог
// опубликовать переход с compile-time умолчаниями РАНЬШЕ — ложный
// TEMP_CRITICAL 79,4 °C при NVS-пороге 85 °C у владельца). В грейс-окне
// стейт-машина работает молча: переход не теряется, а откладывается —
// после окна poll опубликует актуальное состояние сам.
constexpr uint32_t ESP_TEMP_BOOT_GRACE_MS  = 15000;

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

    /// Пульс чтений: инкремент на КАЖДОМ успешном чтении сенсора.
    /// Дежурный HealthMonitor смотрит сюда, а не на значение (урок ночи
    /// 14→15.08.2026: кристалл в закрытом корпусе в термодинамическом
    /// равновесии часами сидит на одной десятой градуса — «значение не
    /// меняется» для температуры НОРМАЛЬНО, а watch по значению давал
    /// 11 ложных STUCK в час). Чтения остановились (NaN/драйвер умер) —
    /// пульс встаёт — вот это настоящий stuck.
    uint16_t getReadSeq() const { return _readSeq; }

    /// Переопределение порогов (sys.temp_warn_c/sys.temp_crit_c — Phase 2,
    /// вызывает TelemetryService из конфига при старте). Санитизация:
    /// warn ∈ [30, 100], crit ∈ [warn+5, 125] — иначе игнор (умолчания).
    /// 5.8.1 (урок №22): гистерезис стал ручкой sys.temp_hyst_c — жарким
    /// днём кристалл качается 83↔85,5 °C и каждое касание порога давало
    /// TEMP_CRITICAL (5 событий за день 17.08); ширина перевзвода — выбор
    /// владельца, не константа (правило 10). hyst ∈ [0, 10] — иначе игнор.
    void setThresholds(float warnC, float critC,
                       float hystC = ESP_TEMP_HYSTERESIS_C) {
        if (warnC >= 30.0f && warnC <= 100.0f &&
            critC >= warnC + 5.0f && critC <= 125.0f) {
            _warnC = warnC;
            _critC = critC;
        }
        if (hystC >= 0.0f && hystC <= 10.0f) _hystC = hystC;
    }

    /// Порог ТЕРМИЧЕСКОЙ ПАНИКИ (sys.temp_panic_c — залежь №2). Санитизация:
    /// panic ∈ [crit+5, 125] — иначе игнор (умолчание 90 °C).
    void setPanic(float panicC) {
        if (panicC >= _critC + 5.0f && panicC <= 125.0f) _panicC = panicC;
    }

private:
    EspTempDriver() = default;

    /// Публикация перехода состояния (WARNING/CRITICAL/возврат в Normal).
    void publishTransition(TempState newState, float tempC);

    float     _lastTempC = NAN;
    TempState _state = TempState::Normal;
    bool      _healthy = false;
    uint16_t  _readSeq = 0;           // пульс чтений (см. getReadSeq)
    uint32_t  _bootGraceUntilMs = 0;  // до этой метки переходы молчат
    // Пороги — поля (умолчания = константы выше; см. setThresholds/setPanic)
    float     _warnC = ESP_TEMP_WARN_C;
    float     _critC = ESP_TEMP_CRIT_C;
    float     _panicC = ESP_TEMP_PANIC_C;
    float     _hystC = ESP_TEMP_HYSTERESIS_C;  // 5.8.1: ручка sys.temp_hyst_c
    bool      _panicActive = false;   // паника — одноразовый фронт, не спам
    // Хэндла сенсора нет: чтение через Arduino API temperatureRead()
    // (новый ESP-IDF tsens-драйвер не собран для классического ESP32 —
    // выявлено линковкой на core 3.3.11).
};
