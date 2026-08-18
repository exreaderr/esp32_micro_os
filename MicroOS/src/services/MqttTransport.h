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
// Реализация — встроенный IDF-клиент esp-mqtt (thread-safe, авто-реконнект):
// publish можно звать из контекста диспетчера шины, внешних библиотек не
// требуется. Внутренний outbox esp-mqtt работает ТОЛЬКО на живом соединении;
// разрыв покрывает собственный outbox 5.1.0 (MqttOutbox.h): зеркальные
// события и publishRaw при офлайне копятся в RAM-кольце и уходят replay'ем
// при reconnect, потери посчитаны (outboxDropped()).
//
// Защита от шторма (C3): входящие команды ограничены окном 10 шт/сек,
// излишек отбрасывается со счётчиком (брокер аутентифицирует отправителя,
// но завалить устройство может и «свой» — ошибочный скрипт УД).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "MqttOutbox.h"

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
static_assert(MQTT_TOPIC_LEN == MQTT_OB_TOPIC_LEN &&
              MQTT_BODY_LEN == MQTT_OB_BODY_LEN,
              "бюджеты outbox разъехались с бюджетами MQTT");
constexpr uint8_t  MQTT_EXT_SUB_MAX   = 4;    // внешних подписок профилей
constexpr uint32_t MQTT_KEEPALIVE_SEC = 30;
constexpr uint8_t  MQTT_CMD_RATE      = 10;   // команд в секунду (C3)
constexpr uint32_t MQTT_RESTART_DELAY_MS = 1500;  // пауза перед reboot (ack уйдёт)

class MqttTransport : public ModuleBase {
public:
    static MqttTransport& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "MqttTransport"; }
    const char* getVersion() const override { return "5.5.1"; }   // 5.5.1: handler получает реальный топик (фикс DOWN моста); 5.5.0: wildcard-матч внешних подписок (M2)
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

    // --- OUTBOX 5.1.0 (диагностика для панели/телеметрии) --------------------
    uint8_t  outboxSize()    const { return _outbox ? _outbox->size() : 0; }
    uint32_t outboxDropped() const { return _outbox ? _outbox->dropped : 0; }

    // --- ПУБЛИЧНЫЕ ПРИМИТИВЫ ДЛЯ ПРОФИЛЕЙ (E2, HA discovery) -----------------
    /// Публикация в ПОЛНЫЙ топик (вне схемы <prefix>/...): discovery-конфиги
    /// Home Assistant и т.п. 5.1.0: при офлайне сообщение копится в outbox
    /// (mqtt.outbox) и уйдёт replay'ем при reconnect — возврат true означает
    /// «принято к доставке» (опубликовано ИЛИ поставлено в очередь);
    /// false — только жёсткие отказы (нет клиента, пустой/длинный топик,
    /// outbox выключен в конфиге).
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
    void drainOutbox();              // replay накопленного после CONNECT
    void handleCommand(const char* verb, const char* body);
    bool cmdRateOk();                // окно 10 шт/с (C3)

    /// Имя события для зеркалирования (кураторский список E2).
    static const char* mirrorName(int32_t eventId);

    void*    _client = nullptr;      // esp_mqtt_client_handle_t (opaque)
    volatile bool _mqttUp   = false; // MQTT_EVENT_CONNECTED пришёл
    bool     _wantRun  = false;      // конфиг+сеть разрешают работу
    bool     _bootEventSent = false; // 5.8.0: BOOT с reset reason — раз за старт

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
        char           topic[MQTT_TOPIC_LEN];    // фильтр подписки (может быть wildcard)
        MqttSubHandler handler;
        char           payload[MQTT_BODY_LEN];
        // 5.5.1: реальный топик ПОСЛЕДНЕГО пришедшего сообщения. Без него
        // wildcard-подписчик (мост M2 на "<prefix>/#") получал в handler
        // фильтр вместо топика и не мог разобрать адресата — команды
        // «сверху вниз» молча умирали (стенд 08.08: Вниз 0, Потери 0).
        char           msgTopic[MQTT_TOPIC_LEN];
        volatile bool  dirty = false;
    };
    ExtSub  _extSubs[MQTT_EXT_SUB_MAX];
    uint8_t _extSubCount = 0;

    MqttCmdHandler _profileCmd = nullptr;   // проброс cmd/<verb> в профиль

    // Outbox 5.1.0: RAM-кольцо «принято к доставке» на время офлайна.
    // Писатели: onEvent (диспетчер шины) и publishRaw (поток профиля/веб);
    // дренаж: onMqttConnected (mqtt-задача). Гонок нет: push и drain
    // разнесены по состоянию _mqttUp (офлайн — только push, онлайн —
    // только drain до опустошения).
    // NB: кольцо — ЕДИНСТВЕННЫЙ heap-блок, выделяется один раз в init()
    // (до фрагментации) и никогда не освобождается. В BSS его 2.8 КБ не
    // влезли: dram0_0_seg overflow 1832 байта на линковке 5.1.0 (урок:
    // «фиксированные бюджеты BSS» — про отказ от churn String/vector,
    // а не про запрет одного статического heap-блока). nullptr — памяти
    // не хватило при init: outbox молча выключен, мост работает как 5.0.
    mqtt_ob::Outbox* _outbox = nullptr;
    uint32_t _outboxDroppedSeen = 0;        // для троттлированного лога
};
