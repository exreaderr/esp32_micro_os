// ============================================================================
// WeatherMirror.h — ПОГОДНОЕ ЗЕРКАЛО (W4): показания weather_gate на мастере
// ============================================================================
// Зачем (концепция W4, открыта 18.08.2026): глубокая история погоды живёт
// там, где есть SD, — на мастере (ярусы DataLog: сырые 6 ч → часы → сутки).
// Шлюз НЕ меняется: самодостаточность («ничего нет — сам всё делаю»),
// зеркало — чистый потребитель существующего контракта.
//
// Контракт (weather_gate_v5.2, WeatherGateApp::weatherJson, retained):
//   {"valid":1,"temp":-12.34,"feels_like":-15.67,"state":"snowy",
//    "humidity":82.0,"wind":3.45,"gust":6.70,"dir":225,
//    "rain":0.40,"press":975.12|"press":null,"press_sea":..., 
//    "rssi":-61,"batt":1,"age_s":34}
// Топик: <prefix>/<id>/weather (по умолчанию microos/weather_gate/weather).
//
// ДВА КАНАЛА ПРИЁМА (по режимам контура, оба — ноль новых сокетов):
//   · НОРМА (вышестоящий брокер жив): subscribeExternal на томе же
//     транспорте мастера — ровно механика smart_lock (lock.weather_topic).
//   · АВТОНОМИЯ (верх пал, парк на локальном брокере): хук BrokerService
//     in-proc (как мост M2 и журнал M3) — публикации шлюза видны без сети.
//   Каналы не дублируют: шлюз публикует ровно в один брокер (куда
//   подключён его транспорт). Страж на переходный период — дедуп по
//   идентичному телу (retained-дубликат при reconnect тоже отсекается).
//
// Модель исполнения: кооперативная. Оба колбэка — контекст tick()
// (MqttTransport: «НИКОГДА из mqtt-задачи»; хук брокера — синхронно из
// fireHooks). Тяжёлого нет: DataLog::logPoint пишет в RAM-кольцо (2.9 КБ),
// слив на SD — отдельной фазой сервиса. Парсер — мини-JSON по strstr
// (паттерн SmartLockApp::parseWeather, без библиотек, без кучи).
//
// Конфиг (конец схемы, группа «Погодное зеркало»):
//   wx.mirror_topic  STRING ""      — топик; пусто = зеркало выключено
//   wx.mirror_local  BOOL   true    — слушать и локальный брокер (in-proc)
//   wx.mirror_dlog   BOOL   true    — каналы wx_* в DataLog на SD
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <services/MqttTransport.h>   // MQTT_TOPIC_LEN / MQTT_BODY_LEN
#include "BrokerService.h"            // BrokerEventInfo

class WeatherMirror : public ModuleBase {
public:
    static WeatherMirror& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "WeatherMirror"; }
    const char* getVersion() const override { return "0.1.0-w4"; }
    ModuleId getModuleId() const override { return 0x1105; }   // hm: 0x1101=Sd, 0x1102=App, 0x1103=Broker, 0x1104=Bridge, 0x1106=Journal; дубль 0x1104 закрыт 04.09.2026
    void init() override;
    void start() override;
    void stop() override;
    void tick() override {}
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- Состояние (будущая карточка панели / /api) ------------------------
    bool     enabled()     const { return _enabled; }
    bool     valid()       const { return _valid; }    // было хоть одно валидное
    float    tempC()       const { return _temp; }
    float    feelsLikeC()  const { return _feel; }
    float    humidityPct() const { return _hum; }
    float    pressureHpa() const { return _press; }    // NAN, если шлюз шлёт null
    float    windMs()      const { return _wind; }
    float    rainMmPh()    const { return _rain; }
    const char* state()    const { return _state; }    // код HA: rainy/sunny/...
    uint32_t lastRxMs()    const { return _rxMs; }
    uint32_t rxTotal()     const { return _rx; }       // принято новых
    uint32_t rxDup()       const { return _dup; }      // отсеяно дедупом

    // --- Точки входа (статические трамплины для C-колбэков) -----------------
    static void onMqtt(const char* topic, const char* payload);       // НОРМА
    static void onBrokerEvent(const BrokerEventInfo& info);           // АВТОНОМИЯ

private:
    WeatherMirror() = default;

    void parse(const char* payload);      // мини-JSON, незнакомые ключи — мимо
    void logChannels();                   // точки wx_* в DataLog

    // Каналы DataLog — те же имена, что у шлюза (wx_ot/wx_oh/wx_p/wx_w/wx_r):
    // график панели одинаково читает историю с любого устройства парка.
    int8_t _chT = -1, _chH = -1, _chP = -1, _chW = -1, _chR = -1;

    bool     _enabled = false;
    bool     _local   = false;   // слушать локальный брокер
    bool     _dlog    = false;   // писать DataLog
    bool     _valid   = false;
    char     _topic[MQTT_TOPIC_LEN] = "";
    char     _last[MQTT_BODY_LEN] = "";   // дедуп: последнее принятое тело
    float    _temp = 0, _feel = 0, _hum = 0, _press = 0, _wind = 0, _rain = 0;
    char     _state[16] = "";
    uint32_t _rxMs = 0;
    uint32_t _rx = 0, _dup = 0;
};
