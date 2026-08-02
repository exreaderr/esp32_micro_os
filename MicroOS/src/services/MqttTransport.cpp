// ============================================================================
// MqttTransport.cpp — реализация MQTT-моста экосистемы (E2)
// ============================================================================
#include "MqttTransport.h"
#include "ConfigService.h"
#include "NetworkManager.h"
#include "TelemetryService.h"
#include "UpdateService.h"
#include "../core/Events.h"
#include <mqtt_client.h>           // встроенный IDF esp-mqtt

// ============================================================================
// ТРАМПЛИН СОБЫТИЙ esp-mqtt (mqtt-задача -> экземпляр)
// ============================================================================
// Обработчики вызываются из задачи mqtt-клиента IDF. publish/subscribe из
// этого контекста — канонический паттерн esp-mqtt (outbox-очередь,
// thread-safe); тяжёлых/блокирующих вызовов здесь нет.
// ============================================================================
static void mqttEventTrampoline(void* arg, esp_event_base_t /*base*/,
                                int32_t id, void* eventData) {
    auto* self = static_cast<MqttTransport*>(arg);
    auto* ev   = static_cast<esp_mqtt_event_handle_t>(eventData);
    if (self == nullptr || ev == nullptr) return;

    // NB: сравниваем с идентификаторами esp-mqtt (MQTT_EVENT_* = 0..10),
    // а НЕ с константами нашей шины SH_EVENT_MQTT_* (0x0210+) — иначе
    // CONNECTED/DISCONNECTED не совпадут никогда: _mqttUp не взводится,
    // устройство молчит (только LWT "offline" от брокера). Найдено по
    // симптому «брокер видит сессию, но online/телеметрии нет».
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:    self->onMqttConnected();    break;
        case MQTT_EVENT_DISCONNECTED: self->onMqttDisconnected(); break;
        case MQTT_EVENT_DATA:
            self->onMqttMessage(ev->topic, ev->topic_len,
                                ev->data, ev->data_len);
            break;
        default: break;   // SUBSCRIBED/PUBLISHED/ERROR — не нужны
    }
}

MqttTransport& MqttTransport::getInstance() {
    static MqttTransport instance;
    return instance;
}

// ============================================================================
// КОНФИГ-СХЕМА (миграция v2.5.0: mq_ip -> mqtt.host уже в таблице ConfigService)
// ============================================================================
void MqttTransport::registerExtensions() {
    ConfigService::getInstance().addFields("MQTT", {
        { "mqtt.enabled", ConfigType::BOOL, "false", 0, 0, CFG_CRITICAL,
          "MQTT", "MQTT-мост включён" },
        { "mqtt.host",    ConfigType::STRING, "", 0, 0, CFG_CRITICAL,
          "MQTT", "Адрес брокера" },
        { "mqtt.port",    ConfigType::UINT, "1883", 1, 65535, CFG_CRITICAL,
          "MQTT", "Порт брокера" },
        { "mqtt.user",    ConfigType::STRING, "", 0, 0, CFG_CRITICAL,
          "MQTT", "Логин (пусто — анонимно)" },
        { "mqtt.pass",    ConfigType::SECRET, "", 0, 0, CFG_SECRET | CFG_CRITICAL,
          "MQTT", "Пароль (только NVS)" },
        { "mqtt.prefix",  ConfigType::STRING, "microos", 0, 0, CFG_CRITICAL,
          "MQTT", "Префикс топиков экосистемы" },
        { "mqtt.pub_events", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "MQTT", "Зеркалировать события в брокер" },
        { "mqtt.ha_discovery", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "MQTT", "Авто-объявление в Home Assistant" },
    });
}

void MqttTransport::init() {
    _initialized = true;
    log(LogLevel::Info, "init: bridge %s",
        cfgGetBool("mqtt.enabled", false) ? "ENABLED" : "disabled");
}

// ============================================================================
// START: подписки шины — зеркало событий (E2) и телеметрия (B1)
// ============================================================================
void MqttTransport::start() {
    EventBus& bus = EventBus::getInstance();
    static const int32_t MIRRORED[] = {
        ACCESS_EVENT_GRANTED, ACCESS_EVENT_DENIED,
        ACCESS_EVENT_LOCKED, ACCESS_EVENT_UNLOCKED,
        AUTH_EVENT_LOGIN, AUTH_EVENT_LOCKED_OUT,
        SH_EVENT_DEGRADED_LEVEL, SH_EVENT_SAFE_MODE_ENTERED,
        SH_EVENT_BOOTLOOP_DETECTED,
        OTA_EVENT_STARTED, OTA_EVENT_SUCCESS, OTA_EVENT_FAILED,
        OTA_EVENT_ROLLBACK,
        DRV_EVENT_TEMP_CRITICAL, HEALTH_EVENT_CRITICAL,
        HEALTH_EVENT_WDT_REBOOT, TEL_EVENT_SNAPSHOT,
    };
    for (size_t i = 0; i < sizeof(MIRRORED) / sizeof(MIRRORED[0]); ++i) {
        bus.subscribe(MIRRORED[i], this);
    }
    _started = true;
}

void MqttTransport::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    clientStop();
    _started = false;
}

// ============================================================================
// TICK: стейт-машина соединения + отложенный reboot + события в шину
// ============================================================================
void MqttTransport::tick() {
    // --- Разрешение на работу: конфиг + сеть --------------------------------
    // NB: mqtt.host проверяем ЗДЕСЬ, а не только в clientStart — иначе
    // при пустом хосте стейт-машина будет дёргать clientStart каждый tick.
    char host[CFG_VALUE_LEN];
    cfgGetStr("mqtt.host", host, sizeof(host), "");
    bool want = cfgGetBool("mqtt.enabled", false) && host[0] != '\0' &&
                NetworkService::getInstance().isConnected();
    if (want != _wantRun) {
        _wantRun = want;
        if (want) clientStart(); else clientStop();
    }

    // --- Переходы MQTT-соединения -> события шины (из нашего потока) -------
    static bool prevUp = false;
    bool up = _mqttUp;
    if (up != prevUp) {
        prevUp = up;
        ShEventData d; d.clear();
        postEvent(up ? SH_EVENT_MQTT_CONNECTED : SH_EVENT_MQTT_DISCONNECTED, &d);
    }

    // --- Отложенный reboot по команде (ack уже ушёл в брокер) --------------
    if (_restartAtMs != 0 && (int32_t)(millis() - _restartAtMs) >= 0) {
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), "mqtt");
        postEvent(SH_EVENT_SHUTDOWN, &d);
        delay(100);            // дать шине разнести прощание
        ESP.restart();
    }

    // --- Диспетчер внешних подписок (НАШ поток — колбэки безопасны) --------
    for (uint8_t i = 0; i < _extSubCount; ++i) {
        ExtSub& s = _extSubs[i];
        if (!s.dirty) continue;
        char pl[MQTT_BODY_LEN];
        memcpy(pl, s.payload, sizeof(pl));
        pl[sizeof(pl) - 1] = '\0';
        s.dirty = false;
        if (s.handler != nullptr) s.handler(s.topic, pl);
    }
}

// ============================================================================
// ВНЕШНИЕ ПОДПИСКИ ПРОФИЛЕЙ
// ============================================================================
bool MqttTransport::subscribeExternal(const char* topic,
                                      MqttSubHandler handler) {
    if (topic == nullptr || topic[0] == '\0' || handler == nullptr ||
        _extSubCount >= MQTT_EXT_SUB_MAX) {
        return false;
    }
    ExtSub& s = _extSubs[_extSubCount++];
    safeStrCopy(s.topic, sizeof(s.topic), topic);
    s.handler = handler;
    s.payload[0] = '\0';
    s.dirty = false;
    log(LogLevel::Info, "ext sub: %s", s.topic);
    if (_mqttUp) {   // уже подключены — подписываем сразу
        esp_mqtt_client_subscribe((esp_mqtt_client_handle_t)_client,
                                  s.topic, 0);
    }
    return true;
}

// ============================================================================
// ЗАПУСК/ОСТАНОВКА КЛИЕНТА
// ============================================================================
void MqttTransport::clientStart() {
    if (_client != nullptr) return;

    char host[CFG_VALUE_LEN], user[CFG_VALUE_LEN], pass[CFG_VALUE_LEN];
    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.host", host, sizeof(host), "");
    cfgGetStr("mqtt.user", user, sizeof(user), "");
    cfgGetStr("mqtt.pass", pass, sizeof(pass), "");
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    // Пустой хост отсечён стейт-машиной tick() до вызова — здесь его нет.

    const char* id = NetworkService::getInstance().deviceId();
    snprintf(_topicState, sizeof(_topicState), "%s/%s/state", prefix, id);
    snprintf(_topicCmdOwn, sizeof(_topicCmdOwn), "%s/%s/cmd/#", prefix, id);
    snprintf(_topicCmdAll, sizeof(_topicCmdAll), "%s/all/cmd/#", prefix);
    snprintf(_topicEventsAll, sizeof(_topicEventsAll),
             "%s/+/events/#", prefix);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.hostname  = host;
    cfg.broker.address.port      = cfgGetUInt("mqtt.port", 1883);
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    if (user[0]) cfg.credentials.username = user;
    if (pass[0]) cfg.credentials.authentication.password = pass;
    cfg.credentials.client_id    = NetworkService::getInstance().hostname();
    cfg.session.keepalive        = MQTT_KEEPALIVE_SEC;
    // LWT: брокер сам объявит "offline" при внезапной смерти устройства
    cfg.session.last_will.topic    = _topicState;
    cfg.session.last_will.msg      = "offline";
    cfg.session.last_will.qos      = 1;
    cfg.session.last_will.retain   = 1;

    _client = esp_mqtt_client_init(&cfg);
    if (_client == nullptr) {
        publishError("MQTT_INIT");
        return;
    }
    esp_mqtt_client_register_event((esp_mqtt_client_handle_t)_client,
                                   (esp_mqtt_event_id_t)MQTT_EVENT_ANY,
                                   mqttEventTrampoline, this);
    esp_mqtt_client_start((esp_mqtt_client_handle_t)_client);
    log(LogLevel::Info, "connecting to %s:%lu as %s",
        host, (unsigned long)cfg.broker.address.port,
        cfg.credentials.client_id);
}

void MqttTransport::clientStop() {
    if (_client == nullptr) return;
    publishState("offline", true);   // штатное прощание (LWT — для аварий)
    esp_mqtt_client_stop((esp_mqtt_client_handle_t)_client);
    esp_mqtt_client_destroy((esp_mqtt_client_handle_t)_client);
    _client = nullptr;
    _mqttUp = false;
}

// ============================================================================
// КОЛБЭКИ esp-mqtt (контекст mqtt-задачи!)
// ============================================================================
void MqttTransport::onMqttConnected() {
    _mqttUp = true;
    // Сессия: online + подписки (канонический паттерн — прямо из колбэка)
    publishState("online", true);
    auto h = (esp_mqtt_client_handle_t)_client;
    esp_mqtt_client_subscribe(h, _topicCmdOwn, 1);
    esp_mqtt_client_subscribe(h, _topicCmdAll, 1);
    esp_mqtt_client_subscribe(h, _topicEventsAll, 0);
    // Внешние подписки профилей — переподписываем при каждом CONNECT
    for (uint8_t i = 0; i < _extSubCount; ++i) {
        esp_mqtt_client_subscribe(h, _extSubs[i].topic, 0);
    }
    publishTelemetry();   // свежий снимок — сразу после подключения
}

void MqttTransport::onMqttDisconnected() { _mqttUp = false; }

void MqttTransport::onMqttMessage(const char* topic, int topicLen,
                                  const char* data, int dataLen) {
    // Копии с явной терминацией (поля esp-mqtt НЕ нуль-терминированы)
    char tbuf[MQTT_TOPIC_LEN];
    char bbuf[MQTT_BODY_LEN];
    int tl = topicLen < (int)sizeof(tbuf) - 1 ? topicLen : (int)sizeof(tbuf) - 1;
    int bl = dataLen  < (int)sizeof(bbuf) - 1 ? dataLen  : (int)sizeof(bbuf) - 1;
    memcpy(tbuf, topic, tl); tbuf[tl] = '\0';
    memcpy(bbuf, data, bl);  bbuf[bl] = '\0';

    // Внешние подписки профилей — точное совпадение топика, без разбора
    // схемы <prefix>/... (погода и прочие чужие топики живут вне её)
    for (uint8_t i = 0; i < _extSubCount; ++i) {
        if (strcmp(tbuf, _extSubs[i].topic) == 0) {
            memcpy(_extSubs[i].payload, bbuf, sizeof(_extSubs[i].payload) - 1);
            _extSubs[i].payload[sizeof(_extSubs[i].payload) - 1] = '\0';
            _extSubs[i].dirty = true;    // разбор — в tick(), не здесь
            return;
        }
    }

    // Разбор: <prefix>/<seg1>/<seg2...>
    char* slash1 = strchr(tbuf, '/');
    if (slash1 == nullptr) return;
    char* rest = slash1 + 1;                    // "<seg1>/<seg2...>"
    char* slash2 = strchr(rest, '/');
    if (slash2 == nullptr) return;
    *slash2 = '\0';
    const char* seg1 = rest;                    // deviceId | "all"
    const char* seg2 = slash2 + 1;              // "cmd/<verb>" | "events/<NAME>"

    const char* ownId = NetworkService::getInstance().deviceId();

    if (strncmp(seg2, "cmd/", 4) == 0) {
        // Своя команда или broadcast ("all" выполняют все, включая нас)
        if (strcmp(seg1, ownId) != 0 && strcmp(seg1, "all") != 0) return;
        if (!cmdRateOk()) { _cmdDropped++; return; }   // C3
        handleCommand(seg2 + 4, bbuf);
    } else if (strncmp(seg2, "events/", 7) == 0) {
        // Чужое событие экосистемы -> локальная шина. СВОЁ — отбрасываем,
        // иначе зеркало замкнётся в петлю (мы же его и опубликовали).
        if (strcmp(seg1, ownId) == 0) return;
        ShEventData d; d.clear();
        snprintf(d.payload, sizeof(d.payload), "%s/%s|%s",
                 seg1, seg2 + 7, bbuf);   // "originId/NAME|body" (<=47)
        d.code = dataLen;
        postEvent(SH_EVENT_MQTT_MESSAGE, &d);
    }
}

// ============================================================================
// КОМАНДЫ ИЗ БРОКЕРА
// ============================================================================
void MqttTransport::handleCommand(const char* verb, const char* body) {
    log(LogLevel::Info, "cmd '%s' body '%s'", verb, body);

    if (strcmp(verb, "reboot") == 0) {
        _restartAtMs = millis() + MQTT_RESTART_DELAY_MS;
    } else if (strcmp(verb, "state") == 0) {
        publishTelemetry();
        publishState("online", true);
    } else if (strcmp(verb, "set") == 0) {
        // body: "key=value" -> ConfigService (валидация по схеме внутри)
        char kv[MQTT_BODY_LEN];
        safeStrCopy(kv, sizeof(kv), body);
        char* eq = strchr(kv, '=');
        bool ok = false;
        if (eq != nullptr) {
            *eq = '\0';
            ok = ConfigService::getInstance().set(kv, eq + 1);
        }
        char res[MQTT_BODY_LEN];
        snprintf(res, sizeof(res), "%s=%s", kv, ok ? "ok" : "rejected");
        char prefix[CFG_VALUE_LEN];
        cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
        char topic[MQTT_TOPIC_LEN];
        snprintf(topic, sizeof(topic), "%s/%s/cmd/result",
                 prefix, NetworkService::getInstance().deviceId());
        esp_mqtt_client_publish((esp_mqtt_client_handle_t)_client,
                                topic, res, 0, 0, 0);
    } else if (strcmp(verb, "ota") == 0) {
        // body: URL прошивки -> UpdateService (A1: PENDING_VERIFY + откат)
        if (!UpdateService::getInstance().startHttpUpdate(body)) {
            log(LogLevel::Warning, "ota command rejected");
        }
    } else {
        // Неизвестную ядру команду пробуем отдать профилю (замок: "open")
        bool handled = (_profileCmd != nullptr) && _profileCmd(verb, body);
        if (handled) {
            char res[MQTT_BODY_LEN];
            snprintf(res, sizeof(res), "%s=ok", verb);
            char prefix[CFG_VALUE_LEN];
            cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
            char topic[MQTT_TOPIC_LEN];
            snprintf(topic, sizeof(topic), "%s/%s/cmd/result",
                     prefix, NetworkService::getInstance().deviceId());
            esp_mqtt_client_publish((esp_mqtt_client_handle_t)_client,
                                    topic, res, 0, 0, 0);
        } else {
            log(LogLevel::Warning, "unknown cmd '%s'", verb);
        }
    }
}

bool MqttTransport::cmdRateOk() {
    // Окно 1 с / 10 команд (C3). Писатель — только mqtt-задача, гонок нет.
    uint32_t now = millis();
    if (now - _cmdWindowStartMs >= 1000) {
        _cmdWindowStartMs = now;
        _cmdInWindow = 0;
    }
    return ++_cmdInWindow <= MQTT_CMD_RATE;
}

// ============================================================================
// ЗЕРКАЛО СОБЫТИЙ ШИНЫ -> БРОКЕР (E2)
// ============================================================================
const char* MqttTransport::mirrorName(int32_t eventId) {
    switch (eventId) {
        case ACCESS_EVENT_GRANTED:       return "ACCESS_GRANTED";
        case ACCESS_EVENT_DENIED:        return "ACCESS_DENIED";
        case ACCESS_EVENT_LOCKED:        return "LOCKED";
        case ACCESS_EVENT_UNLOCKED:      return "UNLOCKED";
        case AUTH_EVENT_LOGIN:           return "AUTH_LOGIN";
        case AUTH_EVENT_LOCKED_OUT:      return "AUTH_LOCKOUT";
        case SH_EVENT_DEGRADED_LEVEL:    return "DEGRADED";
        case SH_EVENT_SAFE_MODE_ENTERED: return "SAFE_MODE";
        case SH_EVENT_BOOTLOOP_DETECTED: return "BOOTLOOP";
        case OTA_EVENT_STARTED:          return "OTA_START";
        case OTA_EVENT_SUCCESS:          return "OTA_SUCCESS";
        case OTA_EVENT_FAILED:           return "OTA_FAILED";
        case OTA_EVENT_ROLLBACK:         return "OTA_ROLLBACK";
        case DRV_EVENT_TEMP_CRITICAL:    return "TEMP_CRITICAL";
        case HEALTH_EVENT_CRITICAL:      return "HEALTH_CRITICAL";
        case HEALTH_EVENT_WDT_REBOOT:    return "WDT_REBOOT";
        default:                         return nullptr;
    }
}

bool MqttTransport::canHandleEvent(int32_t eventId) const {
    return eventId == TEL_EVENT_SNAPSHOT || mirrorName(eventId) != nullptr;
}

void MqttTransport::onEvent(int32_t eventId, const ShEventData* data) {
    if (!_mqttUp) return;   // нет соединения — outbox не копим
    if (eventId == TEL_EVENT_SNAPSHOT) {
        publishTelemetry();
        return;
    }
    if (cfgGetBool("mqtt.pub_events", true)) {
        mirrorEvent(eventId, data);
    }
}

void MqttTransport::mirrorEvent(int32_t eventId, const ShEventData* data) {
    const char* name = mirrorName(eventId);
    if (name == nullptr) return;

    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    char topic[MQTT_TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/%s/events/%s",
             prefix, NetworkService::getInstance().deviceId(), name);

    char body[MQTT_BODY_LEN];
    snprintf(body, sizeof(body), "%ld|%s",
             (long)(data ? data->code : 0),
             (data && data->payload[0]) ? data->payload : "");

    esp_mqtt_client_publish((esp_mqtt_client_handle_t)_client,
                            topic, body, 0, 1, 0);
}

// ============================================================================
// ПУБЛИКАЦИИ
// ============================================================================
void MqttTransport::publishState(const char* state, bool retained) {
    if (_client == nullptr) return;
    esp_mqtt_client_publish((esp_mqtt_client_handle_t)_client,
                            _topicState, state, 0, 1, retained ? 1 : 0);
}

// ============================================================================
// ПУБЛИЧНЫЕ ПРИМИТИВЫ ДЛЯ ПРОФИЛЕЙ (E2, HA discovery)
// ============================================================================
bool MqttTransport::publishRaw(const char* fullTopic, const char* payload,
                               bool retained) {
    if (_client == nullptr || !_mqttUp || fullTopic == nullptr) return false;
    // Из mqtt-задачи и из диспетчера шины вызывать одинаково безопасно
    // (esp-mqtt thread-safe); из панели результат виден по возврату false.
    return esp_mqtt_client_publish((esp_mqtt_client_handle_t)_client,
                                   fullTopic, payload, 0, 1,
                                   retained ? 1 : 0) >= 0;
}

bool MqttTransport::publishStateSuffix(const char* suffix, const char* payload,
                                       bool retained) {
    if (suffix == nullptr) return false;
    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    char topic[MQTT_TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/%s/%s",
             prefix, NetworkService::getInstance().deviceId(), suffix);
    return publishRaw(topic, payload, retained);
}

void MqttTransport::publishTelemetry() {
    if (_client == nullptr || !_mqttUp) return;
    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    char topic[MQTT_TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/%s/telemetry",
             prefix, NetworkService::getInstance().deviceId());

    char json[MQTT_BODY_LEN];
    TelemetryService::getInstance().toJson(json, sizeof(json));
    // retain: УД видит последний снимок даже после рестарта подписчика
    esp_mqtt_client_publish((esp_mqtt_client_handle_t)_client,
                            topic, json, 0, 0, 1);
}
