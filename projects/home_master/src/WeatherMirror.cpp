// ============================================================================
// WeatherMirror.cpp — погодное зеркало (W4). Дизайн — в шапке .h.
// ============================================================================
#include "WeatherMirror.h"
#include <services/ConfigService.h>
#include <services/DataLogService.h>
#include <cstdlib>
#include <cstring>
#include <cmath>

WeatherMirror& WeatherMirror::getInstance() {
    static WeatherMirror inst;
    return inst;
}

void WeatherMirror::init() {
    char topic[MQTT_TOPIC_LEN];
    cfgGetStr("wx.mirror_topic", topic, sizeof(topic), "");
    if (topic[0] == '\0') {
        log(LogLevel::Info, "weather-mirror: OFF (wx.mirror_topic пуст)");
        return;   // пустой топик = осознанно выключено, каналы не создаём
    }
    strncpy(_topic, topic, sizeof(_topic) - 1);
    _topic[sizeof(_topic) - 1] = '\0';
    _local = cfgGetBool("wx.mirror_local", true);
    _dlog  = cfgGetBool("wx.mirror_dlog",  true);

    // Каналы DataLog — имена как у шлюза: история читается одинаково.
    if (_dlog) {
        DataLogService& dl = DataLogService::getInstance();
        _chT = dl.registerChannel("wx_ot", "Улица, температура", "°C");
        _chH = dl.registerChannel("wx_oh", "Улица, влажность", "%");
        _chP = dl.registerChannel("wx_p",  "Улица, давление", "гПа");
        _chW = dl.registerChannel("wx_w",  "Улица, ветер", "м/с");
        _chR = dl.registerChannel("wx_r",  "Улица, дождь", "мм/ч");
    }
    _enabled = true;
}

void WeatherMirror::start() {
    if (!_enabled) return;
    // НОРМА: подписка на вышестоящем транспорте (механика smart_lock).
    MqttTransport::getInstance().subscribeExternal(_topic, &WeatherMirror::onMqtt);
    // АВТОНОМИЯ: in-proc хук локального брокера (слот рядом с мостом/журналом).
    if (_local)
        BrokerService::getInstance().addEventHook(&WeatherMirror::onBrokerEvent);
    log(LogLevel::Info, "weather-mirror: ON | топик \"%s\" | local=%s | dlog=%s",
        _topic, _local ? "on" : "off", _dlog ? "on" : "off");
}

void WeatherMirror::stop() {
    if (!_enabled) return;
    if (_local)
        BrokerService::getInstance().removeEventHook(&WeatherMirror::onBrokerEvent);
    // subscribeExternal парного unsubscribe не имеет (как и у smart_lock):
    // при _enabled=false колбэк отсечётся первой же проверкой.
    _enabled = false;
}

// --- Вход НОРМА: колбэк транспорта, контекст tick нашего потока. -------------
void WeatherMirror::onMqtt(const char* topic, const char* payload) {
    (void)topic;   // топик один — дискриминация не нужна (паттерн smart_lock)
    WeatherMirror& self = getInstance();
    if (!self._enabled) return;
    self.parse(payload);
}

// --- Вход АВТОНОМИЯ: хук брокера, СИНХРОННО из fireHooks (tick). -------------
void WeatherMirror::onBrokerEvent(const BrokerEventInfo& info) {
    WeatherMirror& self = getInstance();
    if (!self._enabled || !self._local) return;
    if (info.type != BrokerEventInfo::Publish) return;
    if (info.truncated) return;                       // усечённое — никогда
    if (strcmp(info.topic, self._topic) != 0) return; // точное совпадение
    self.parse(info.payload);
}

// --- Мини-JSON (паттерн SmartLockApp::parseWeather): strstr + strtof, ------
// --- незнакомые ключи игнорируются, битое значение = поле не обновится. -----
namespace {

// "key":<float> с проверкой диапазона; "key":null честно пропускается.
bool wxFloat(const char* js, const char* key, float lo, float hi, float& out) {
    const char* p = strstr(js, key);
    if (p == nullptr) return false;
    p = strchr(p, ':');
    if (p == nullptr) return false;
    ++p;
    while (*p == ' ') ++p;
    if (*p == 'n') return false;                      // null (press без BME280)
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    float v = strtof(p, nullptr);
    if (v < lo || v > hi) return false;               // вне диапазона — мусор
    out = v;
    return true;
}

} // namespace

void WeatherMirror::parse(const char* payload) {
    if (payload == nullptr) return;

    // Дедуп: идентичное тело = retained-дубликат при reconnect (или гонка
    // каналов на переходе режимов). Тело целиком — age_s делает каждую
    // свежую публикацию уникальной, повторы бывают только у ретрансляций.
    if (_valid && strncmp(payload, _last, sizeof(_last)) == 0) {
        ++_dup;
        return;
    }

    // Шлюз без данных публикует {"valid":0} — зеркало честно молчит.
    if (strstr(payload, "\"valid\":0") != nullptr) return;

    float v;
    if (wxFloat(payload, "\"temp\"",     -80.0f,  80.0f, v)) _temp  = v;
    if (wxFloat(payload, "\"feels_like\"",-80.0f, 80.0f, v)) _feel  = v;
    if (wxFloat(payload, "\"humidity\"",   0.0f, 100.0f, v)) _hum   = v;
    if (wxFloat(payload, "\"wind\"",       0.0f,  80.0f, v)) _wind  = v;
    if (wxFloat(payload, "\"rain\"",       0.0f, 500.0f, v)) _rain  = v;
    if (wxFloat(payload, "\"press\"",    300.0f,1200.0f, v)) _press = v;
    // NB: "\"press\"" ищется раньше "\"press_sea\"" и не матчит её:
    // strstr ищет с кавычкой-ключом — "press_sea" не содержит "press\"".

    const char* s = strstr(payload, "\"state\"");
    if (s != nullptr) {
        s = strchr(s, ':');
        if (s != nullptr) {
            while (*s && *s != '"') ++s;
            if (*s == '"') {
                ++s;
                size_t i = 0;
                while (*s && *s != '"' && i < sizeof(_state) - 1)
                    _state[i++] = *s++;
                _state[i] = '\0';
            }
        }
    }

    strncpy(_last, payload, sizeof(_last) - 1);
    _last[sizeof(_last) - 1] = '\0';
    _rxMs = millis();
    ++_rx;

    const bool first = !_valid;
    _valid = true;
    logChannels();
    if (first)
        log(LogLevel::Info, "weather-mirror: первая погода T=%.1f H=%.0f P=%.1f",
            (double)_temp, (double)_hum, (double)_press);
}

void WeatherMirror::logChannels() {
    if (!_dlog) return;
    DataLogService& dl = DataLogService::getInstance();
    if (_chT >= 0) dl.logPoint(_chT, _temp);
    if (_chH >= 0) dl.logPoint(_chH, _hum);
    if (_chP >= 0) dl.logPoint(_chP, _press);
    if (_chW >= 0) dl.logPoint(_chW, _wind);
    if (_chR >= 0) dl.logPoint(_chR, _rain);
}
