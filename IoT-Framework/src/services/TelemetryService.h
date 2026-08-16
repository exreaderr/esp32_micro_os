// ============================================================================
// TelemetryService.h — СИСТЕМНЫЕ МЕТРИКИ (B1)
// ============================================================================
// Фаза 3, порция 1. Принцип: "аварии видны заранее". Монолит v4.2.2 узнавал
// о проблемах постфактум (устройство зависло -> WDT -> ребут). Здесь —
// периодический снимок здоровья системы ОДНИМ модулем (в монолите
// метрики собирались в трёх местах: DiagnosticManager, PazManager,
// TempSensorManager).
//
// Что собираем (всё — из существующих владельцев, без дублирования):
//   · heap: свободно сейчас / минимум за сессию (ESP32 SDK);
//   · шина событий: потеряно / high-watermark (EventBus, B2);
//   · температура кристалла (EspTempDriver — единственный источник);
//   · сеть: уровень деградации, RTT шлюза (NetworkService, A3);
//   · bootloop-счётчик (Kernel RTC RAM, A1);
//   · аудит: потери очереди (AuditService);
//   · uptime.
//
// Куда девается снимок:
//   · TEL_EVENT_SNAPSHOT в шину -> MqttTransport публикует в УД (порция 2);
//   · toJson() -> /api/system веб-сервера (порция 3).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"

struct TelemetrySnapshot {
    uint32_t uptimeSec;        // аптайм, с
    uint32_t heapFree;         // свободная heap, байт
    uint32_t heapMin;          // минимум heap за сессию (watermark)
    uint32_t busDropped;       // потеряно событий шины за сессию
    uint16_t busHighWater;     // пиковая глубина очереди шины
    int16_t  cpuTenths;        // температура кристалла x10 (45.1 -> 451)
    int16_t  cpuSeq;           // пульс чтений драйвера (getReadSeq) — для
                               // дежурного HealthMonitor: значение может
                               // стоять часами (равновесие), пульс — нет
    uint8_t  degradation;      // DegradationLevel (A3)
    uint32_t gatewayRttMs;     // RTT шлюза (0 — не измерен)
    uint8_t  bootloopCount;    // нестабильных стартов подряд (A1)
    uint32_t auditOverflows;   // потери очереди аудита (B3)
};

class TelemetryService : public ModuleBase {
public:
    static TelemetryService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "TelemetryService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x000C; }

    void registerExtensions() override;   // схема tel.*
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 1000; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t) const override { return false; }

    // --- ДОСТУП ------------------------------------------------------------
    const TelemetrySnapshot& snapshot() const { return _snap; }
    /// Снимок в JSON (для /api/system и MQTT-публикации).
    size_t toJson(char* buf, size_t bufSize) const;

private:
    TelemetryService() = default;
    void collect();               // сбор снимка у владельцев метрик
    /// Пороги ПАЗ температуры — из конфига в драйвер (init + CFG_EVENT_CHANGED,
    /// чтобы правка полей sys.temp_* в панели применялась без рестарта).
    void applyTempThresholds();

    TelemetrySnapshot _snap = {};
    uint32_t _lastCollectMs = 0;
};
