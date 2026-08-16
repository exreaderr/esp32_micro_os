// ============================================================================
// BridgeService.h — МОСТ M2: локальный брокер ↔ вышестоящий брокер
// ============================================================================
// Дизайн (концепция, блок M2, утверждён владельцем 07.08.2026):
//   · ТРАНСПОРТ — in-proc, ноль сокетов (loopback-клиент не влезал бы в
//     лимит 16 сокетов lwIP: 12+2+listen+HTTP+up = 17 > 16).
//   · UP (устройства → верх): хук BrokerService (Public: topic/payload/
//     retain) → publishRaw наверх. Фильтр: <prefix>/# + homeassistant/#.
//     Retain as-is. Офлайн-политика: без retain — честный drop со счётчиком;
//     retained ≤ outbox-бюджета (255 Б) — outbox ядра (dedup last-wins);
//     retained длиннее — drop + счётчик (усечённое НЕ транслируем никогда;
//     устройство ре-анонсит при своём reconnect, shadow-replay — v2).
//   · DOWN (верх → устройства): subscribeExternal(<prefix>/#) + форма
//     топика «*/set» или «*/cmd/#» → publishLocal в локальный брокер.
//     Состояния/телеметрия/discovery вниз не текут НИКОГДА. Исключение —
//     bridge.down_extra (5.5.12): точный whitelist топиков через запятую
//     (напр. smart_lock/weather) — подписка наверху + пропуск вниз;
//     каждая запись — осознанное решение оператора (вопрос M3 стенда:
//     погода замка мёрла на кэше, теперь доезжает).
//   · ПЕТЛИ НЕТ ПО ПОСТРОЕНИЮ: broker-originated publish() события Public
//     не вызывает (проверено в коде движка) — вколоченное вниз не уедет
//     наверх. Вторая линия — форма DOWN-фильтра (свои UP-состояния эхом
//     не возвращаются).
//   · LWT-СИНТЕЗ: sMQTT LWT не исполняет → по RemoveClient(client_id)
//     публикуем наверх retained offline. ТОПИК — ВЫУЧЕННЫЙ (5.5.3, стенд
//     08.08): client_id устройства = hostname, а топиковая идентичность =
//     MAC-id («connecting as smart_lock» vs microos/d4e9.../state) —
//     угадайка <prefix>/<clientId>/state хоронила в ЧУЖОЙ топик, HA
//     «Недоступно» не видел. Мост учит по Publish-событиям пару
//     client_id → availability-топик (retained <prefix>/<seg>/state
//     = "online") и хоронит точно в него; fallback — старая угадайка
//     с warning'ом. Страж: свой hostname пропускаем (мастер не должен
//     хоронить сам себя).
//   · РЕЖИМЫ: master.mode = auto/bridge — мост при живом upstream;
//     solo — выкл. Выключенный брокер → мост неактивен (нечего сшивать).
//
// Модель исполнения: кооперативная, как всё ядро — обработчики вызываются
// из tick() (поток loopTask), своих задач нет, тяжёлое/FS запрещено.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include "BrokerService.h"   // BrokerEventInfo

class BridgeService : public ModuleBase {
public:
    static BridgeService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "BridgeService"; }
    const char* getVersion() const override { return "0.1.2-m2"; }   // 0.1.2: down_extra whitelist (погода замка вниз)
    ModuleId getModuleId() const override { return 0x1104; }   // 0x1101=Sd, 0x1102=App, 0x1103=Broker
    void init() override;
    void start() override;
    void stop() override;
    void tick() override {}
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- Состояние (для /api/dev/hm/info и панели) ------------------------
    bool     active()       const { return _active; }   // мост сшивает прямо сейчас
    uint32_t bridgedUp()    const { return _up; }       // сообщений устройств ушло наверх
    uint32_t bridgedDown()  const { return _down; }     // команд с верха ушло устройствам
    uint32_t dropped()      const { return _dropped; }  // честные потери (офлайн/бюджет)
    uint32_t synthOffline() const { return _synth; }    // синтезированных offline

    // --- Точки входа (статические трамплины для C-колбэков) ----------------
    static void onBrokerEvent(const BrokerEventInfo& ev);       // UP + LWT-синтез
    static void onUpstreamMessage(const char* topic, const char* payload);  // DOWN

private:
    BridgeService() = default;

    void forwardUp(const char* topic, const char* payload, bool retain);
    bool upFilter(const char* topic) const;          // <prefix>/ или homeassistant/
    bool downShape(const char* topic) const;         // «*/set» | «*/cmd/#» | down_extra
    bool downListed(const char* topic) const;        // точное попадание в bridge.down_extra

    // Учебная таблица availability (5.5.3): client_id → топик <prefix>/<id>/state
    bool        isAvailTopic(const char* topic) const;   // <prefix>/<1 сегмент>/state
    void        learnAvail(const char* clientId, const char* topic);
    const char* findAvail(const char* clientId) const;   // nullptr — не выучен

    static constexpr uint8_t AVAIL_MAX = 12;   // потолок = ёмкость брокера (M1)
    struct AvailSlot {
        char clientId[48];
        char topic[MQTT_TOPIC_LEN];
    };

    bool     _enabled  = false;
    bool     _active   = false;
    bool     _synthCfg = true;
    char     _prefix[48] = "microos";   // CFG_VALUE_LEN
    char     _downExtra[48] = "";       // CFG_VALUE_LEN: whitelist вниз, через запятую
    uint32_t _up = 0, _down = 0, _dropped = 0, _synth = 0;
    AvailSlot _avail[AVAIL_MAX] = {};
};
