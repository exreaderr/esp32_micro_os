// ============================================================================
// BrokerService.h — ВСТРОЕННЫЙ MQTT-БРОКЕР МАСТЕРА (M1, spike)
// ============================================================================
// Роль (концепция §3): home_master — резервный/локальный брокер параноидаль-
// ного контура. Пока жив HAOS — устройства ходят на него; упал — мастер
// принимает трафик парка у себя. Spike M1 ПРИНЯТ (бенч 07.08.2026): движок
// выдержал всё, ёмкость контура зафиксирована на 12 клиентах — стена не
// брокерская, а 16 сокетов lwIP стокового ядра (13 + 3 служебных; 12 с
// резервом под HTTP-сессии панели).
//
// Движок: vendored sMQTTBroker (src/third_party/smqtt, MIT, MQTT 3.1.1,
// QoS 0/1, retained полноценно, LWT не исполняется — компенсируется
// retained-heartbeat'ами устройств). Патчи и ограничения — в VENDORED.md.
//
// Модель исполнения: кооперативная, как всё ядро — tick() каждую итерацию
// loop(), без своих задач FreeRTOS. Сокеты — NetworkServer/NetworkClient
// ядра 3.x поверх lwIP: принимают соединения с ЛЮБОГО интерфейса, радио
// при этом не инициализируется (параноидальный контур не нарушается).
//
// ЗАМЕРЫ SPIKE'А (главное!): на каждое подключение/отключение в лог
// пишется free heap и min-free-heap watermark — стоимость одного клиента
// читается прямо из журнала стенда. Раз в минуту при живых клиентах —
// строка телеметрии для soak-теста.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <services/MqttTransport.h>   // MQTT_TOPIC_LEN — общий бюджет топика

// --- Событие брокера для моста M2 (хук, M2) ----------------------------------
// Отдаётся синхронно из handleBrokerEvent (контекст tick, поток loopTask).
// Бюджеты фиксированные, без кучи: payload шире транспортного (discovery-
// конфиги HA ~600 Б), но ограничен — длиннее буфера помечается truncated
// и мостом отбрасывается (никогда не транслируем усечённое).
constexpr uint16_t BROKER_EVENT_PAYLOAD_LEN = 768;
struct BrokerEventInfo {
    enum Type : uint8_t { Publish, RemoveClient } type;
    char topic[MQTT_TOPIC_LEN];                 // Publish
    char payload[BROKER_EVENT_PAYLOAD_LEN];     // Publish (см. truncated)
    bool retain   = false;                      // Publish (патч движка #3)
    bool truncated = false;                     // Publish: тело не влезло
    char clientId[48] = "";                     // RemoveClient
};
typedef void (*BrokerEventHook)(const BrokerEventInfo&);

class BrokerService : public ModuleBase {
public:
    static BrokerService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "BrokerService"; }
    const char* getVersion() const override { return "0.1.6-m3"; }   // 0.1.6: hasClient — страж гонки synth offline (M2); 0.1.5: multi-hook (слоты под мост M2 + журнал M3); 0.1.4: clientId в Publish-хуке; 0.1.2: ёмкость 12 (бенч M1); 0.1.3: хук для моста M2
    ModuleId getModuleId() const override { return 0x1103; }   // 0x1101=SdService, 0x1102=HomeMasterApp
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- Состояние (для UI/ПАЗ/журнала) -----------------------------------
    bool     enabled()  const { return _enabled; }   // broker.enabled=true в конфиге
    bool     running()  const { return _running; }   // сокет слушает порт
    uint16_t port()     const { return _port; }
    uint16_t clients()  const { return _clients; }   // активных MQTT-сессий
    uint16_t maxClients() const { return _maxClients; }
    uint32_t retained() const;                        // retained-топиков (у движка)
    uint32_t rxTotal()  const { return _rxTotal; }    // принято PUBLISH от клиентов
    uint32_t rejected() const { return _rejected; }   // отказов (auth/cap)

    /// true, если у брокера есть АКТИВНАЯ сессия с таким clientId.
    /// Страж гонки переподключения для моста M2 (урок 31.08.2026, репорт
    /// профильной ветки): RemoveClient старой (мёртвой) сессии может
    /// прийти ПОСЛЕ того, как устройство уже переподключилось — synth
    /// offline в этот момент хоронить нельзя.
    bool hasClient(const char* clientId) const;

    /// Публикация из ядра в брокер (фундамент моста M2: локальные события
    /// парка → подписчики мастера). false — брокер не запущен.
    bool publishLocal(const char* topic, const char* payload, bool retain = false);

    // --- Мост к движку (вызывается ТОЛЬКО из MicroOsBroker в .cpp) --------
    /// Возврат false на NewClient = отказ клиенту (CONNACK + разрыв).
    bool handleBrokerEvent(class sMQTTEvent* event);

    /// Хуки событий брокера: Public (topic/payload/retain/clientId) и
    /// RemoveClient (clientId) из принятых сессий. До BROKER_HOOK_MAX
    /// слушателей (M3.1: было один слот — мост M2; добавился журнал).
    /// Вызываются синхронно из tick — тяжёлое в хуке запрещено (SD, сеть).
    static constexpr uint8_t BROKER_HOOK_MAX = 4;
    bool addEventHook(BrokerEventHook h);
    void removeEventHook(BrokerEventHook h);

private:
    BrokerService() = default;

    void logHeap(const char* what) const;   // замер spike'а одной строкой

    static constexpr uint32_t STATS_INTERVAL_MS = 60000;  // soak-телеметрия

    // Принятые сессии (для корректного декремента: RemoveClient приходит
    // и за отвергнутых, их в таблице нет). Фикс. таблица, без кучи.
    static constexpr uint8_t ACCEPT_TABLE_MAX = 64;

    bool     _enabled = false;
    bool     _running = false;
    uint16_t _port = 1883;
    uint16_t _maxClients = 12;   // бенч M1: 16 сокетов lwIP = 12 + 4 служебных
    uint16_t _clients = 0;
    uint32_t _rxTotal = 0;
    uint32_t _rejected = 0;
    uint32_t _lastStatsMs = 0;
    char     _user[48] = "";   // CFG_VALUE_LEN
    char     _pass[48] = "";
    void*    _accepted[ACCEPT_TABLE_MAX] = {};
    BrokerEventHook _hooks[BROKER_HOOK_MAX] = {};   // мост M2 + журнал M3 + ...
    void fireHooks(const BrokerEventInfo& info) {   // синхронно, контекст tick
        for (uint8_t i = 0; i < BROKER_HOOK_MAX; ++i)
            if (_hooks[i] != nullptr) _hooks[i](info);
    }
};
