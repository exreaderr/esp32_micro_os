// ============================================================================
// HomeMasterApp.h — ПОЛИТИКА МАСТЕРА УМНОГО ДОМА (M1)
// ============================================================================
// «Голова» профиля: инжектирует расширения в ядро (конфиг-схему, веб-лицо,
// ПАЗ), владеет режимом мастера (auto/solo/bridge — оживут с мостом M2),
// объявляет устройство в Home Assistant (discovery к вышестоящему брокеру)
// и публикует профильный снимок состояния (SD/брокер) в MQTT.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>

/// Режим работы мастера (концепция §3). Значения конфига master.mode.
enum class MasterMode : uint8_t {
    Auto   = 0,   // детект вышестоящего брокера (M2), сейчас = Solo
    Solo   = 1,   // всегда свой брокер (брокер — M1)
    Bridge = 2,   // всегда транслятор на upstream (мост — M2)
};

class HomeMasterApp : public ModuleBase {
public:
    static HomeMasterApp& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "HomeMasterApp"; }
    const char* getVersion() const override { return "0.6.0"; }         // 0.6.0: M3.3 BackupAggregator (снимки парка на SD, ядро 5.8.5 export/import); 0.5.2: UI по правилу 23
    ModuleId getModuleId() const override { return 0x1102; }   // 0x1101=SdService, 0x1103=BrokerService, 0x1104=BridgeService
    void registerExtensions() override;
    void init() override;
    void start() override;
    void stop() override { _started = false; }
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    /// Режим из конфига (строка master.mode -> enum; неизвестное = Auto).
    MasterMode mode() const { return _mode; }
    const char* modeStr() const;

private:
    // --- HA discovery + профильное состояние (M1) -------------------------
    /// Объявление сущностей в HA (retained-конфиги, паттерн smart_lock).
    void publishHaDiscovery();
    /// Снимок профиля в <prefix>/<id>/hm/state (retained JSON для HA и
    /// будущего флот-дашборда). Офлайн — копится/дедупится outbox'ом ядра.
    void publishHmState();
    /// Команды из брокера, неизвестные ядру (cmd/sd_remount).
    static bool onMqttCmd(const char* verb, const char* body);

    static constexpr uint32_t HM_STATE_PERIOD_MS = 30000;  // период снимка

    MasterMode _mode = MasterMode::Auto;
    uint32_t   _lastStateMs = 0;
};
