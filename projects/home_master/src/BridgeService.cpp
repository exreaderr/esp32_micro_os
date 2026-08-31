// ============================================================================
// BridgeService.cpp — мост M2 (см. шапку .h: дизайн и гарантии против петли)
// ============================================================================
#include "BridgeService.h"
#include <services/ConfigService.h>
#include <services/MqttTransport.h>
#include <services/NetworkManager.h>
#include <services/MqttOutbox.h>   // MQTT_OB_BODY_LEN — бюджет офлайн-слота

BridgeService& BridgeService::getInstance() {
    static BridgeService instance;
    return instance;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void BridgeService::init() {
    _enabled  = cfgGetBool("bridge.enabled", true);
    _synthCfg = cfgGetBool("bridge.synth_offline", true);
    cfgGetStr("mqtt.prefix", _prefix, sizeof(_prefix), "microos");
    cfgGetStr("bridge.down_extra", _downExtra, sizeof(_downExtra), "");

    char mode[16];
    cfgGetStr("master.mode", mode, sizeof(mode), "auto");

    // Активен только когда есть что сшивать: брокер слушает И режим не solo.
    // Связь с верхом — не условие: офлайн-политика (outbox/drop) в forwardUp.
    _active = _enabled &&
              strcmp(mode, "solo") != 0 &&
              BrokerService::getInstance().running();

    if (!_active) {
        log(LogLevel::Info, "bridge: НЕАКТИВЕН (enabled=%d, mode=%s, broker=%s)",
            _enabled ? 1 : 0, mode,
            BrokerService::getInstance().running() ? "on" : "off");
        _initialized = true;
        return;
    }
    log(LogLevel::Info, "bridge: init ok (mode=%s, prefix=%s, synth_offline=%s)",
        mode, _prefix, _synthCfg ? "on" : "off");
    _initialized = true;
}

void BridgeService::start() {
    _started = true;
    if (!_active) return;

    BrokerService::getInstance().addEventHook(&BridgeService::onBrokerEvent);

    char filter[MQTT_TOPIC_LEN];
    snprintf(filter, sizeof(filter), "%s/#", _prefix);
    if (!MqttTransport::getInstance().subscribeExternal(
            filter, &BridgeService::onUpstreamMessage)) {
        log(LogLevel::Error, "bridge: subscribeExternal(%s) FAILED — DOWN нет", filter);
        publishError("bridge_sub");
        return;
    }
    // Whitelist вниз (5.5.12): каждый топик из bridge.down_extra — отдельная
    // подписка наверху (типично лежит ВНЕ <prefix>/#, как smart_lock/weather).
    if (_downExtra[0] != '\0') {
        char list[48];
        safeStrCopy(list, sizeof(list), _downExtra);
        char* save = nullptr;
        for (char* t = strtok_r(list, ",", &save); t; t = strtok_r(nullptr, ",", &save)) {
            while (*t == ' ') t++;
            if (*t == '\0') continue;
            if (MqttTransport::getInstance().subscribeExternal(
                    t, &BridgeService::onUpstreamMessage)) {
                log(LogLevel::Info, "bridge: whitelist вниз +%s", t);
            } else {
                log(LogLevel::Error, "bridge: subscribeExternal(%s) FAILED — whitelist мимо", t);
                publishError("bridge_sub_wl");
            }
        }
    }
    log(LogLevel::Info,
        "bridge: МОСТ M2 АКТИВЕН — вверх %s/# + homeassistant/#, "
        "вниз */set + */cmd/#%s (in-proc, 0 сокетов)", _prefix,
        _downExtra[0] ? " + whitelist" : "");
}

void BridgeService::stop() {
    BrokerService::getInstance().removeEventHook(&BridgeService::onBrokerEvent);
    _active = false;
    _started = false;
}

// ============================================================================
// UP: устройства локального брокера → вышестоящий
// ============================================================================
void BridgeService::onBrokerEvent(const BrokerEventInfo& ev) {
    BridgeService& self = getInstance();
    if (!self._active) return;

    if (ev.type == BrokerEventInfo::Publish) {
        if (ev.truncated) {
            // Тело не влезло в бюджет события — честная потеря, НЕ усечение.
            self._dropped++;
            self.log(LogLevel::Warning,
                     "bridge: drop UP (payload > %u B): %s",
                     (unsigned)(BROKER_EVENT_PAYLOAD_LEN - 1), ev.topic);
            return;
        }
        // 5.5.3: учим availability-топик автора (retained ".../state"=online).
        // Отпечаток строгий: форма <prefix>/<1 сегмент>/state — телеметрия
        // и discovery-конфиги сюда не попадают.
        if (ev.retain && ev.clientId[0] != '\0' &&
            strcmp(ev.payload, "online") == 0 && self.isAvailTopic(ev.topic)) {
            self.learnAvail(ev.clientId, ev.topic);
        }
        self.forwardUp(ev.topic, ev.payload, ev.retain);
        return;
    }

    // RemoveClient: синтез offline (sMQTT LWT не исполняет — хоронит мост).
    if (ev.type == BrokerEventInfo::RemoveClient && self._synthCfg &&
        ev.clientId[0] != '\0') {
        // Страж: мастер не хоронит сам себя (локальный клиент с hostname
        // мастера — аномалия, но fake offline мастера наверху недопустим).
        if (strcmp(ev.clientId,
                   NetworkService::getInstance().hostname()) == 0) return;
        // Страж гонки переподключения (урок 31.08.2026, репорт профильной
        // ветки): при жёстком обрыве (power cycle, watchdog) sMQTT
        // обнаруживает мёртвый TCP с задержкой — RemoveClient СТАРОЙ
        // сессии приходит ПОСЛЕ того, как устройство переподключилось и
        // опубликовало retained online. Безусловный synth offline затирал
        // живой state наверху навсегда. Есть активная сессия с тем же
        // clientId — не хороним (обратный порядок самолечится: offline
        // перезаписывается штатным online новой сессии).
        if (BrokerService::getInstance().hasClient(ev.clientId)) {
            self.log(LogLevel::Info,
                     "bridge: synth offline пропущен — '%s' уже переподключился",
                     ev.clientId);
            return;
        }
        char topic[MQTT_TOPIC_LEN];
        const char* learned = self.findAvail(ev.clientId);
        if (learned != nullptr) {
            safeStrCopy(topic, sizeof(topic), learned);
        } else {
            // Fallback 5.5.0: угадайка по client_id. Подходит устройствам,
            // у которых client_id == топиковый id; для пары hostname/MAC-id
            // (smart_lock) промахивается — потому и warning, а не тишина.
            snprintf(topic, sizeof(topic), "%s/%s/state", self._prefix,
                     ev.clientId);
            self.log(LogLevel::Warning,
                     "bridge: avail-топик клиента '%s' не выучен, synth в "
                     "угадайку %s", ev.clientId, topic);
        }
        if (MqttTransport::getInstance().publishRaw(topic, "offline", true)) {
            self._synth++;
            self._up++;
            self.log(LogLevel::Info, "bridge: synth offline -> %s", topic);
        } else {
            self._dropped++;
        }
    }
}

// ============================================================================
// УЧЕБНАЯ ТАБЛИЦА AVAILABILITY (5.5.3)
// ============================================================================
bool BridgeService::isAvailTopic(const char* topic) const {
    size_t n = strlen(_prefix);
    if (strncmp(topic, _prefix, n) != 0 || topic[n] != '/') return false;
    const char* rest = topic + n + 1;                    // "<seg>/state"
    const char* slash = strchr(rest, '/');
    if (slash == nullptr || slash == rest) return false; // сегмент непуст
    if (strchr(slash + 1, '/') != nullptr) return false; // ровно один сегмент
    return strcmp(slash + 1, "state") == 0;
}

void BridgeService::learnAvail(const char* clientId, const char* topic) {
    uint8_t freeSlot = AVAIL_MAX;
    for (uint8_t i = 0; i < AVAIL_MAX; ++i) {
        if (_avail[i].clientId[0] == '\0') {
            if (freeSlot == AVAIL_MAX) freeSlot = i;
            continue;
        }
        if (strcmp(_avail[i].clientId, clientId) == 0) {
            safeStrCopy(_avail[i].topic, sizeof(_avail[i].topic), topic);
            return;   // перезапись при reconnect — штатно
        }
    }
    if (freeSlot == AVAIL_MAX) return;   // таблица полна — synth уйдёт в fallback
    safeStrCopy(_avail[freeSlot].clientId, sizeof(_avail[freeSlot].clientId),
                clientId);
    safeStrCopy(_avail[freeSlot].topic, sizeof(_avail[freeSlot].topic), topic);
    log(LogLevel::Info, "bridge: avail %s -> %s", clientId, topic);
}

const char* BridgeService::findAvail(const char* clientId) const {
    for (uint8_t i = 0; i < AVAIL_MAX; ++i) {
        if (_avail[i].clientId[0] != '\0' &&
            strcmp(_avail[i].clientId, clientId) == 0) {
            return _avail[i].topic;
        }
    }
    return nullptr;
}

void BridgeService::forwardUp(const char* topic, const char* payload,
                              bool retain) {
    if (!upFilter(topic)) return;

    MqttTransport& up = MqttTransport::getInstance();
    if (!up.isConnected()) {
        // Офлайн-политика (утверждена в дизайне M2):
        //  · без retain — честный drop (телеметрия QoS0: устройство всё равно
        //    публикует локально, наверху устаревшее не нужно);
        //  · retained влезающий в outbox-слот — ядерный outbox (dedup
        //    last-wins — идеальная семантика состояний);
        //  · retained длиннее слота — drop: outbox усечёт молча, а битый
        //    JSON наверху хуже отсутствующего (устройство ре-анонсит при
        //    своём reconnect; shadow-replay — бэклог v2).
        size_t plen = strlen(payload);
        if (!retain || plen >= MQTT_OB_BODY_LEN) {
            _dropped++;
            if (retain)
                log(LogLevel::Warning,
                    "bridge: drop UP retained (офлайн, %u B > outbox %u B): %s",
                    (unsigned)plen, (unsigned)(MQTT_OB_BODY_LEN - 1), topic);
            return;
        }
    }
    if (up.publishRaw(topic, payload, retain)) _up++;
    else _dropped++;
}

bool BridgeService::upFilter(const char* topic) const {
    size_t n = strlen(_prefix);
    if (strncmp(topic, _prefix, n) == 0 && topic[n] == '/') return true;
    return strncmp(topic, "homeassistant/", 14) == 0;
}

// ============================================================================
// DOWN: вышестоящий → устройства локального брокера
// ============================================================================
void BridgeService::onUpstreamMessage(const char* topic, const char* payload) {
    BridgeService& self = getInstance();
    if (!self._active) return;
    if (!self.downShape(topic)) return;   // эхо состояний/телеметрии/discovery — мимо

    // Команды retained не бывают — инжектируем без retain.
    if (BrokerService::getInstance().publishLocal(topic, payload, false)) {
        self._down++;
        self.log(LogLevel::Info, "bridge: DOWN %s", topic);
    } else {
        self._dropped++;
    }
}

bool BridgeService::downShape(const char* topic) const {
    if (strstr(topic, "/cmd/") != nullptr) return true;
    size_t n = strlen(topic);
    if (n >= 4 && strcmp(topic + n - 4, "/set") == 0) return true;
    return downListed(topic);
}

// Точное совпадение с записью whitelist (записи через запятую, пробелы после
// запятой допустимы). Никаких масок — только осознанно перечисленные топики.
bool BridgeService::downListed(const char* topic) const {
    if (_downExtra[0] == '\0') return false;
    const char* p = _downExtra;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char* end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len > 0 && p[len - 1] == ' ') len--;   // хвостовые пробелы
        if (len > 0 && strlen(topic) == len && strncmp(topic, p, len) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
