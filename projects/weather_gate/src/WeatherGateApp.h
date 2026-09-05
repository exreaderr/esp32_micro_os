// ============================================================================
// WeatherGateApp.h — ПРИКЛАДНОЙ МОДУЛЬ WEATHER_GATE (стадия W3: полный профиль)
// ============================================================================
// W1: конфиг-схема датчика, минимальный UI, conformance-стенд.
// W2: радиотракт CC1101 + декодер Fine Offset, endpoint /api/dev/radio.
// W3: агрегация уличных показаний (пакет -> снимок + производные:
//     feels-like, интенсивность дождя, код состояния HA), weather-JSON
//     для smart_lock (HTTP /api/dev/weather + MQTT <prefix>/<id>/weather,
//     retained), каналы DataLog (графики uPlot), ПАЗ-проверки домена
//     (датчик давления, молчание эфира), авто-высота по координатам
//     при FULL-сети (разовая запись wx.altitude_m).
// Замбретти — W5.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <services/IUiProvider.h>
#include <drivers/WeatherCore.h>
#include "WgScanCore.h"   // W3.3: чистая логика сканера частоты (0.5.0)

// ============================================================================
// UI-ПРОВАЙДЕР ПРОФИЛЯ
// ============================================================================
class WeatherGateUi : public IUiProvider {
public:
    static WeatherGateUi& getInstance() {
        static WeatherGateUi instance;
        return instance;
    }

    const char* uiTitle() const override { return "weather_gate"; }

    /// Публичная карточка на "/" (бюджет ~2 КБ): улица + давление + эфир.
    size_t renderPublicHtml(char* buf, size_t bufSize) override;

    bool handleApi(const char* pathTail, const ShUiRequest& req,
                   char* responseBuf, size_t bufSize,
                   int& statusCode) override;

private:
    WeatherGateUi() = default;
};

// ============================================================================
// МОДУЛЬ
// ============================================================================
class WeatherGateApp : public ModuleBase {
public:
    static WeatherGateApp& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "WeatherGateApp"; }
    const char* getVersion() const override { return "0.5.4"; } // edgesDroppedTotal: в Радио темп потерь фронтов (+Δ за окно, N/мин) вместо пугающего абсолюта; всего — вторично (дорожная карта 05.09)
    ModuleId getModuleId() const override { return 0x1000; }      // приложения

    void registerExtensions() override;   // конфиг wx.*, UI, ПАЗ-проверки
    void init() override;
    void start() override;                // подписки, DataLog, conformance
    void stop() override;
    void tick() override;                 // 1 с: пакеты, лог, публикации
    uint32_t getTickIntervalMs() const override { return 1000; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- СНИМОК УЛИЦЫ (для UI и weather-JSON) ------------------------------
    struct Outdoor {
        bool     valid = false;     // был хотя бы один валидный пакет
        uint32_t rxMs = 0;          // millis() приёма
        float    tempC = 0;
        float    humidityPct = 0;
        float    windMs = 0;
        float    gustMs = 0;
        float    rainMmPh = 0;      // интенсивность за часовое окно
        uint16_t dirDeg = 0;
        bool     batteryLow = false;
        uint8_t  deviceId = 0;
    };
    const Outdoor& outdoor() const { return _out; }

    float       feelsLikeC() const;   // wxc::feelsLikeC от снимка
    const char* weatherState() const; // код HA (wxc::weatherState)

    /// Контракт smart_lock {"temp","feels_like","state"} + расширения
    /// (humidity, wind, gust, dir, rain, press_sea, press, rssi, age_s,
    /// batt). smart_lock читает свои три поля — расширения безопасны.
    /// Возвращает длину; если улица невалидна — {"valid":0} (публикация
    /// в MQTT при этом НЕ делается — см. tick).
    size_t weatherJson(char* buf, size_t bufSize) const;

    /// Полная версия для HTTP /api/dev/weather (буфер ядра большой):
    /// компактный контракт + статистика 24 ч из DataLog (tmin24/tmax24/
    /// pmin24/pmax24/wmax24), баротренд и температура BME280 (котельная).
    /// В MQTT НЕ уходит — там бюджет MQTT_BODY_LEN=256 (только компакт).
    size_t weatherJsonFull(char* buf, size_t bufSize) const;

    // --- ДИАГНОСТИКА (W3.2-diag1, конфиг wx.diag) ---------------------------
    /// Мост к protected ModuleBase::log для свободных функций UI-провайдера
    /// (там членства в ModuleBase нет). body — готовая строка (≤90 симв.,
    /// бюджет LOG_BODY_LEN=96); тег модуля подставит ядро. Вызывать ТОЛЬКО
    /// под условием wx.diag — см. s_diagLog() в .cpp.
    void logDiag(const char* body);

    // --- СКАНЕР ЧАСТОТЫ (W3.3, 0.5.0) ---------------------------------------
    // Подбор лучшей частоты приёма по измеренному уровню: сетка ±0.20 МГц
    // вокруг рабочей (окно монолита v5.2), на точке — окно наблюдения,
    // метрики pkt/RSSI/шум. Применение результата — только оператором.
    // receive-only: перестройка = FREQ-регистры + вход в RX (STX нет).
    bool scanActive() const { return _scan.active; }
    /// Старт прогона. stepX100: 2|5 (0.02|0.05 МГц), dwellS: 30..120.
    /// false + err — отказ (уже идёт / драйвер нездоров / параметры).
    bool   scanStart(uint16_t stepX100, uint16_t dwellS, char* err, size_t errSize);
    void   scanAbort();                 // отмена: возврат на рабочую частоту
    /// Живая перестройка вне скана (клик по графику/«Применить»): границы
    /// схемы 914–916, readback в лог. Конфиг не пишет — только эфир.
    bool   scanTune(float mhz);
    /// Статус для UI: state/прогресс/таблица точек/рекомендация.
    size_t scanStatusJson(char* buf, size_t bufSize) const;

private:
    WeatherGateApp() = default;

    void scanTick();                    // из tick(): шум, пакеты, перестройка

    void onNewRadioPacket();          // пакет -> снимок, лог, MQTT, событие
    void publishWeatherMqtt();        // retained <prefix>/<id>/weather (+ зеркало)
    void publishHaDiscovery();        // retained homeassistant/... (E2, 0.3.8)
    void refreshStats24();            // мин/макс 24ч + баротренд из DataLog

    // --- АВТО-ВЫСОТА (wx.lat/wx.lon -> wx.altitude_m, разово) ---------------
    void maybeRequestAltitude();      // условия + постановка флага
    static void altitudeTask(void*);  // одноразовая задача: HTTP GET,
                                      // парсинг, ConfigService::set

    Outdoor          _out;
    wxc::RainTracker _rain;
    uint32_t _seenPktSeq     = 0;
    uint32_t _lastPubMs      = 0;
    uint32_t _lastPressLogMs = 0;
    bool     _mqttPubOk      = true;  // последний результат publish (лог по фронту)

    // Статистика 24 ч (кэш; пишется ТОЛЬКО из tick — читатели без гонок)
    float    _mnT24 = 0, _mxT24 = 0, _mnP24 = 0, _mxP24 = 0, _mxW24 = 0;
    int8_t   _trend = 0;            // wxc::baroTrend3h
    bool     _statsValid = false;
    uint32_t _lastStatsMs = 0;

    // Каналы DataLog (-1 — не зарегистрирован)
    int8_t _chOutT = -1, _chOutH = -1, _chPress = -1, _chWind = -1, _chRain = -1;

    // Состояние сканера частоты (W3.3). Точки пишутся только из scanTick
    // (task-контекст), читатели HTTP — копию через scanStatusJson.
    struct Scan {
        bool     active = false;
        bool     done   = false;        // прогон завершён штатно (есть таблица)
        uint16_t stepX100 = 2;
        uint16_t dwellS   = 60;
        uint32_t homeX100 = 91500;      // рабочая частота на момент старта
        uint8_t  idx    = 0;
        uint8_t  count  = 0;
        uint32_t pointStartMs = 0;
        uint32_t lastPktSeq   = 0;
        wgs::ScanPoint pts[wgs::SCAN_MAX_POINTS];
    } _scan;

    // Авто-высота
    bool _altRequested  = false;      // есть повод попробовать
    bool _altDone       = false;      // успешно записана (больше не лезем)
    uint32_t _altNextRetryMs = 0;     // неудача -> повтор не раньше часа
    static volatile bool s_altTaskRunning;
};
