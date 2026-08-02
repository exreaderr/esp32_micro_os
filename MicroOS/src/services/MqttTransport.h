// ============================================================================
// MqttTransport.h — MQTT-МОСТ ЭКОСИСТЕМЫ (E2: «межвидовое» общение)
// ============================================================================
// Фаза 3, порция 2. Превращает одинокое устройство в командного игрока:
// шина событий МикроОС <-> брокер умного дома.
//
// Что умеет мост:
//   · СОСТОЯНИЕ:  <prefix>/<id>/state = online/offline (LWT + retain) —
//                 УД видит живость устройства даже при внезапной смерти;
//   · ТЕЛЕМЕТРИЯ: <prefix>/<id>/telemetry = JSON-снимок B1 (retain);
//   · СОБЫТИЯ:    <prefix>/<id>/events/<NAME> — кураторское зеркало шины
//                 (доступ, тревоги, деградация, OTA) — то, что интересно
//                 ДРУГИМ устройствам и УД;
//   · КОМАНДЫ:    <prefix>/<id>/cmd/<verb> и broadcast <prefix>/all/cmd/<verb>
//                 (reboot, state, set key=value, ota <url>);
//   · ЧУЖИЕ СОБЫТИЯ: подписка <prefix>/+/events/# — события других
//                 устройств экосистемы прилетают в локальную шину как
//                 MQTT_EVENT_MESSAGE -> профили реагируют на соседей
//                 (тревога одного устройства -> сирена другого).
//
// Реализация — встроенный IDF-клиент esp-mqtt (thread-safe, авто-реконнект,
// outbox-очередь): publish можно звать из контекста диспетчера шины,
// внешних библиотек не требуется.
//
// Защита от шторма (C3): входящие команды ограничены окном 10 шт/сек,
// излишек отбрасывается со счётчиком (брокер аутентифицирует отправителя,
// но завалить устройство может и «свой» — ошибочный скрипт УД).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"

// Бюджеты
// ПОСТМОРТЕМ 5.0.x: MQTT_BODY_LEN был uint8_t — 256 обернулось в 0, все
// буферы тел сообщений скомпилировались нулевой длины: publish(len=0) ->
// esp-mqtt strlen() по мусору стека -> порча кучи -> паника в планировщике
// FreeRTOS (LoadProhibited, pvOwner==NULL), а onMqttMessage ждал
// memcpy(..., (size_t)-1). Бомба дремала, пока трамплин не доставлял
// CONNECTED. Тип — uint16_t, static_assert ниже — прививка от регресса.
constexpr uint8_t  MQTT_TOPIC_LEN     = 96;   // полный топик
constexpr uint16_t MQTT_BODY_LEN      = 256;  // тело сообщения (telemetry JSON)
static_assert(MQTT_TOPIC_LEN == 96 && MQTT_BODY_LEN == 256,
              "бюджеты MQTT обрезаны типом константы");
constexpr uint8_t  MQTT_EXT_SUB_MAX   = 4;    // внешних подписок профилей
constexpr uint32_t MQTT_KEEPALIVE_SEC = 30;
constexpr uint8_t  MQTT_CMD_RATE      = 10;   // команд в секунду (C3)
constexpr uint32_t MQTT_RESTART_DELAY_MS = 1500;  // пауза перед reboot (ack уйдёт)

class MqttTransport : public ModuleBase {
public:
    static MqttTransport& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "MqttTransport"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0101; }   // транспорт

    void registerExtensions() override;   // схема mqtt.*
    void init() override;
    void start() override;                // подписки шины (зеркало событий)
    void stop() override;
    void tick() override;                 // стейт-машина соединения
    uint32_t getTickIntervalMs() const override { return 500; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- ТОЧКИ ВХОДА КОЛБЭКОВ esp-mqtt (НЕ для прикладного кода!) --------
    /// Вызываются из mqtt-задачи IDF: только флаги/копии в статику,
    /// разбор — в tick()/onEvent нашего потока.
    void onMqttConnected();
    void onMqttDisconnected();
    void onMqttMessage(const char* topic, int topicLen,
                       const char* data, int dataLen);

    // --- СОСТОЯНИЕ -----------------------------------------------------------
    bool isConnected() const { return _mqttUp; }

    // --- ПУБЛИЧНЫЕ ПРИМИТИВЫ ДЛЯ ПРОФИЛЕЙ (E2, HA discovery) -----------------
    /// Публикация в ПОЛНЫЙ топик (вне схемы <prefix>/...): discovery-конфиги
    /// Home Assistant и т.п. false — нет соединения (ничего не накопится).
    bool publishRaw(const char* fullTopic, const char* payload, bool retained);

    /// Публикация в <prefix>/<id>/<suffix> (состояния сущностей HA, retained).
    bool publishStateSuffix(const char* suffix, const char* payload, bool retained);

    /// Проброс неизвестных ядру команд <prefix>/<id>/cmd/<verb> в профиль
    /// (замок: «open»). Хендлер возвращает true — команда принята.
    typedef bool (*MqttCmdHandler)(const char* verb, const char* body);
    void setCmdHandler(MqttCmdHandler h) { _profileCmd = h; }

    // --- ВНЕШНИЕ ПОДПИСКИ (профили: погода/данные УД) ------------------------
    /// Подписка на произвольный топик (вне схемы <prefix>/...). Колбэк
    /// вызывается из tick() нашего потока — НИКОГДА из mqtt-задачи.
    /// Переподписка при reconnect — автоматическая. До MQTT_EXT_SUB_MAX штук.
    typedef void (*MqttSubHandler)(const char* topic, const char* payload);
    bool subscribeExternal(const char* topic, MqttSubHandler handler);

private:
    MqttTransport() = default;

    // --- ВНУТРЕННЯЯ КУХНЯ ---------------------------------------------------
    void clientStart();              // создание + запуск IDF-клиента
    void clientStop();               // остановка и уничтожение
    void applySession();             // подписки + online после CONNECT
    void publishState(const char* state, bool retained);
    void publishTelemetry();         // JSON B1 -> telemetry
    void mirrorEvent(int32_t eventId, const ShEventData* data);
    void handleCommand(const char* verb, const char* body);
    bool cmdRateOk();                // окно 10 шт/с (C3)

    /// Имя события для зеркалирования (кураторский список E2).
    static const char* mirrorName(int32_t eventId);

    void*    _client = nullptr;      // esp_mqtt_client_handle_t (opaque)
    volatile bool _mqttUp   = false; // MQTT_EVENT_CONNECTED пришёл
    bool     _wantRun  = false;      // конфиг+сеть разрешают работу

    // Топики (буферы-члены: живут, пока жив клиент)
    char     _topicState[MQTT_TOPIC_LEN];     // <prefix>/<id>/state (LWT)
    char     _topicCmdOwn[MQTT_TOPIC_LEN];    // <prefix>/<id>/cmd/#
    char     _topicCmdAll[MQTT_TOPIC_LEN];    // <prefix>/all/cmd/#
    char     _topicEventsAll[MQTT_TOPIC_LEN]; // <prefix>/+/events/#

    // C3: окно ограничения входящих команд (писатель — только mqtt-задача)
    uint8_t  _cmdInWindow = 0;
    uint32_t _cmdWindowStartMs = 0;
    uint32_t _cmdDropped = 0;

    uint32_t _restartAtMs = 0;       // отложенный reboot по команде

    // Внешние подписки профилей (погода и т.п.). Писатель payload/dirty —
    // mqtt-задача, читатель/диспетчер — наш tick (паттерн входящих команд).
    struct ExtSub {
        char           topic[MQTT_TOPIC_LEN];
        MqttSubHandler handler;
        char           payload[MQTT_BODY_LEN];
        volatile bool  dirty = false;
    };
    ExtSub  _extSubs[MQTT_EXT_SUB_MAX];
    uint8_t _extSubCount = 0;

    MqttCmdHandler _profileCmd = nullptr;   // проброс cmd/<verb> в профиль
};
