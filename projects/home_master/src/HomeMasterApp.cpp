// ============================================================================
// HomeMasterApp.cpp — политика мастера (M1: HA discovery + снимок состояния;
// M2: мост к вышестоящему брокеру — BridgeService)
// ============================================================================
#include "HomeMasterApp.h"
#include "HomeMasterConfig.h"
#include "HomeMasterUi.h"
#include "HomeMasterHealthChecks.h"
#include "SdService.h"
#include "BrokerService.h"
#include "BridgeService.h"
#include <core/EventBus.h>
#include <core/Events.h>
#include <core/Version.h>
#include <services/HttpService.h>
#include <services/ConfigService.h>
#include <services/MqttTransport.h>
#include <services/NetworkManager.h>
#include <services/UpdateService.h>

HomeMasterApp& HomeMasterApp::getInstance() {
    static HomeMasterApp instance;
    return instance;
}

const char* HomeMasterApp::modeStr() const {
    switch (_mode) {
        case MasterMode::Auto:   return "auto";
        case MasterMode::Solo:   return "solo";
        case MasterMode::Bridge: return "bridge";
    }
    return "unknown";
}

// ============================================================================
// РАСШИРЕНИЯ (точки инжекции профиля)
// ============================================================================
void HomeMasterApp::registerExtensions() {
    registerHomeMasterConfig();                       // поля master.*, sd.*

    static HomeMasterUi ui;                           // веб-лицо мастера
    HttpService::getInstance().setUiProvider(&ui);

    registerHomeMasterHealthChecks();                 // доменный ПАЗ (hm.sd)
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void HomeMasterApp::init() {
    // Параноидальный контур (решение владельца 05.08.2026): только провода.
    // Радио физически ВЫКЛЮЧЕНО: RF-блок S3 не запитан, пока кто-то не
    // вызовет esp_wifi_init / инициализацию BT-контроллера, а ни один модуль
    // ядра и профиля этого не делает (ядро трогает лишь WiFi.onEvent —
    // регистрацию колбэков, не радио). Явные вызовы WiFi.mode/esp_bt_*
    // НЕ добавляем: они тянут в прошивку весь WiFi/BT-стек (+337 КБ —
    // измерено на сборке 5.3.2) и пробивают бюджет 1,2 МБ. Лучшая глушилка —
    // отсутствие кода, который мог бы радио включить.
    log(LogLevel::Info, "radio: Wi-Fi/BT OFF (RF не запитан, стек не инициализируется — M0.5)");

    char mode[CFG_VALUE_LEN];
    cfgGetStr("master.mode", mode, sizeof(mode), "auto");
    if (strcmp(mode, "solo") == 0)        _mode = MasterMode::Solo;
    else if (strcmp(mode, "bridge") == 0) _mode = MasterMode::Bridge;
    else                                  _mode = MasterMode::Auto;

    _initialized = true;
    log(LogLevel::Info,
        "HomeMaster M2: mode=%s, sd=%s, broker=%s, bridge=%s",
        modeStr(), SdService::getInstance().stateStr(),
        BrokerService::getInstance().running() ? "listening" : "off",
        BridgeService::getInstance().active() ? "on" : "off");
}

void HomeMasterApp::start() {
    EventBus& bus = EventBus::getInstance();
    bus.subscribe(NET_EVENT_IP_CHANGED, this);
    bus.subscribe(OTA_EVENT_SUCCESS, this);
    bus.subscribe(SH_EVENT_MQTT_CONNECTED, this);   // discovery + свежий снимок
    MqttTransport::getInstance().setCmdHandler(&HomeMasterApp::onMqttCmd);
    _started = true;
    log(LogLevel::Info, "HomeMasterApp started (МикроОС %s)", MICROOS_VERSION);
}

void HomeMasterApp::tick() {
    // Периодический профильный снимок. Офлайн — publishRaw кладёт его
    // в outbox ядра, где retained-слот hm/state просто обновляется
    // (дедупликация по топику), переполнения кольца нет.
    uint32_t now = millis();
    if (now - _lastStateMs >= HM_STATE_PERIOD_MS) {
        _lastStateMs = now;
        publishHmState();
    }
}

// ============================================================================
// СОБЫТИЯ
// ============================================================================
bool HomeMasterApp::canHandleEvent(int32_t eventId) const {
    return eventId == NET_EVENT_IP_CHANGED || eventId == OTA_EVENT_SUCCESS ||
           eventId == SH_EVENT_MQTT_CONNECTED;
}

void HomeMasterApp::onEvent(int32_t eventId, const ShEventData* data) {
    switch (eventId) {
        case NET_EVENT_IP_CHANGED:
            log(LogLevel::Info, "network: IP %s", data ? data->payload : "?");
            break;
        case OTA_EVENT_SUCCESS:
            log(LogLevel::Info, "OTA success, reboot scheduled");
            break;
        case SH_EVENT_MQTT_CONNECTED:
            // (Пере)подключение к вышестоящему брокеру: объявляем сущности
            // (retained-конфиги) и сразу публикуем свежий снимок профиля.
            publishHaDiscovery();
            publishHmState();
            break;
        default: break;
    }
}

// ============================================================================
// HOME ASSISTANT DISCOVERY (паттерн smart_lock: устройство объявляет себя само)
// ============================================================================
// Конфиги retained + availability на ядерном LWT (<prefix>/<id>/state):
// HA видит устройство offline при внезапной смерти контроллера.
// Модель сущностей консервативная: действия — кнопками (импульс), всё
// состояние — read-only сенсорами; «умных» writable-сущностей нет.
// Публикуется ТОЛЬКО на SH_EVENT_MQTT_CONNECTED (онлайн): 15 retained-
// конфигов офлайн заняли бы 15 слотов outbox'а — не копим мусор.
// ============================================================================
void HomeMasterApp::publishHaDiscovery() {
    if (!cfgGetBool("mqtt.ha_discovery", true)) return;
    MqttTransport& mqtt = MqttTransport::getInstance();

    const char* id = NetworkService::getInstance().deviceId();
    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");

    // Общий хвост: availability + карточка устройства.
    // sw — из UpdateService (там core/Version.h): карточка в HA показывает
    // РЕАЛЬНУЮ версию прошивки (урок 5.0.10).
    char dev[288];
    snprintf(dev, sizeof(dev),
        ",\"avty_t\":\"%s/%s/state\",\"pl_avail\":\"online\","
        "\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"],"
        "\"name\":\"%s\",\"mf\":\"MicroOS\",\"mdl\":\"home_master\","
        "\"sw\":\"%s\"}",
        prefix, id, id, NetworkService::getInstance().hostname(),
        UpdateService::getInstance().firmwareVersion());

    char topic[MQTT_TOPIC_LEN];
    char cfg[768];

    // --- Кнопка «Перезагрузка» (ядерный verb reboot) --------------------------
    snprintf(topic, sizeof(topic), "homeassistant/button/%s_reboot/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Перезагрузка\",\"uniq_id\":\"%s_reboot\","
        "\"dev_cla\":\"restart\",\"cmd_t\":\"%s/%s/cmd/reboot\"%s}",
        id, prefix, id, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Кнопка «Перемонтировать SD» (профильный verb sd_remount) ------------
    snprintf(topic, sizeof(topic), "homeassistant/button/%s_sd_remount/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Перемонтировать SD\",\"uniq_id\":\"%s_sd_remount\","
        "\"cmd_t\":\"%s/%s/cmd/sd_remount\"%s}", id, prefix, id, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Проблема SD (no_card/failed; disabled — осознанно выключена, не ЧП) --
    char hm[MQTT_TOPIC_LEN];
    snprintf(hm, sizeof(hm), "%s/%s/hm/state", prefix, id);

    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/%s_sd_problem/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Проблема SD\",\"uniq_id\":\"%s_sd_problem\","
        "\"dev_cla\":\"problem\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ 'ON' if value_json.sd in ['no_card','failed'] "
        "else 'OFF' }}\"%s}", id, hm, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Телеметрия ядра (JSON-снимок <prefix>/<id>/telemetry) ----------------
    char tel[MQTT_TOPIC_LEN];
    snprintf(tel, sizeof(tel), "%s/%s/telemetry", prefix, id);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_uptime/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Аптайм\",\"uniq_id\":\"%s_uptime\",\"dev_cla\":\"duration\","
        "\"unit_of_meas\":\"s\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.uptime }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_heap/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Свободная память\",\"uniq_id\":\"%s_heap\","
        "\"unit_of_meas\":\"B\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.heap }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_temp/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Температура CPU\",\"uniq_id\":\"%s_temp\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.cpu_t }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_degrad/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Деградация сети\",\"uniq_id\":\"%s_degrad\","
        "\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.degradation }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_gwrtt/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"RTT до шлюза\",\"uniq_id\":\"%s_gwrtt\","
        "\"unit_of_meas\":\"ms\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.gw_rtt }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Профильное состояние (JSON-снимок <prefix>/<id>/hm/state) ------------
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_mode/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Режим мастера\",\"uniq_id\":\"%s_mode\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.mode }}\"%s}", id, hm, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_sd_state/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"SD состояние\",\"uniq_id\":\"%s_sd_state\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.sd }}\"%s}", id, hm, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_sd_mb/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"SD объём\",\"uniq_id\":\"%s_sd_mb\","
        "\"unit_of_meas\":\"MB\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.sd_mb }}\"%s}", id, hm, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_sd_used/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"SD занято\",\"uniq_id\":\"%s_sd_used\","
        "\"unit_of_meas\":\"MB\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.sd_used_mb }}\"%s}", id, hm, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_br_clients/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Брокер: клиенты\",\"uniq_id\":\"%s_br_clients\","
        "\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.br_clients }}\"%s}", id, hm, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_br_retained/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Брокер: retained\",\"uniq_id\":\"%s_br_retained\","
        "\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.br_retained }}\"%s}", id, hm, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_br_rx/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Брокер: принято сообщений\",\"uniq_id\":\"%s_br_rx\","
        "\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.br_rx }}\"%s}", id, hm, dev);
    mqtt.publishRaw(topic, cfg, true);

    log(LogLevel::Info, "HA discovery: 15 entities announced");
}

// ============================================================================
// ПРОФИЛЬНЫЙ СНИМОК СОСТОЯНИЯ (retained JSON для HA и флот-дашборда M5)
// ============================================================================
void HomeMasterApp::publishHmState() {
    SdService& sd = SdService::getInstance();
    BrokerService& br = BrokerService::getInstance();
    char json[192];
    snprintf(json, sizeof(json),
        "{\"sd\":\"%s\",\"sd_mb\":%llu,\"sd_used_mb\":%llu,"
        "\"br_on\":%u,\"br_clients\":%u,\"br_retained\":%lu,"
        "\"br_rx\":%lu,\"mode\":\"%s\"}",
        sd.stateStr(),
        (unsigned long long)sd.sizeMb(),
        (unsigned long long)sd.usedMb(),
        br.running() ? 1u : 0u,
        (unsigned)br.clients(),
        (unsigned long)br.retained(),
        (unsigned long)br.rxTotal(),
        modeStr());
    MqttTransport::getInstance().publishStateSuffix("hm/state", json, true);
}

// ============================================================================
// ПРОФИЛЬНЫЕ MQTT-КОМАНДЫ (verb'ы, неизвестные ядру)
// ============================================================================
bool HomeMasterApp::onMqttCmd(const char* verb, const char* /*body*/) {
    if (strcmp(verb, "sd_remount") == 0) {
        bool ok = SdService::getInstance().tryMount();
        getInstance().log(LogLevel::Info, "cmd sd_remount -> %s",
                          ok ? "ok" : "rejected");
        getInstance().publishHmState();   // свежий снимок — сразу после команды
        return true;
    }
    return false;
}
