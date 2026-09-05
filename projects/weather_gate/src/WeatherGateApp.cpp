// ============================================================================
// WeatherGateApp.cpp — реализация прикладного модуля (стадия W3)
// ============================================================================
#include "WeatherGateApp.h"
#include "WeatherGateProfile.h"
#include "WeatherGateEvents.h"
#include <core/Kernel.h>
#include <core/ConformanceTest.h>
#include <core/EventBus.h>
#include <core/Events.h>
#include <services/ConfigService.h>
#include <services/HttpService.h>
#include <services/HealthMonitor.h>
#include <services/MqttTransport.h>
#include <services/DataLogService.h>
#include <services/TimeService.h>
#include <services/NetworkManager.h>
#include <services/UpdateService.h>   // sw-версия в карточке устройства HA
#include <drivers/Bme280Driver.h>
#include <drivers/Cc1101Driver.h>
#include "WxTrend.h"
#include <HTTPClient.h>              // авто-высота (одноразовая задача)
#include <WiFiClient.h>
#include <cstdarg>                   // s_diagLog (W3.2-diag1)

// ============================================================================
// ПАЗ-ПРОВЕРКИ ДОМЕНА (IHealthCheck; HealthMonitor владеет механизмом,
// профиль — содержимым. Объекты статические: ПАЗ их не удаляет).
// ============================================================================

// Датчик давления: потерян (не отвечает после init) -> CRITICAL,
// чтения старше 2 минут -> WARNING. Доменные события — на переходах.
class WgSensorCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "wg.bme280"; }
    uint32_t intervalMs() const override { return 5000; }
    HealthResult run() override {
        const Bme280Driver& d = Bme280Driver::getInstance();
        if (!d.isHealthy()) {
            if (!_lost) {
                _lost = true;
                EventBus::getInstance().post(wg_ev::sensorLost());
            }
            return HealthResult::critical("BME280_LOST");
        }
        if (_lost) {
            _lost = false;
            EventBus::getInstance().post(wg_ev::sensorRestored());
        }
        if (d.lastReadMs() == 0) return HealthResult::ok();  // ещё не читал
        if (millis() - d.lastReadMs() > 120000UL)
            return HealthResult::warning("BME280_STALE");
        return HealthResult::ok();
    }
private:
    bool _lost = false;
};

// Радиотракт: чип потерян -> CRITICAL; тишина эфира дольше wx.silence_min ->
// WARNING, дольше двух таймаутов -> CRITICAL. После boot — льгота 5 минут
// (станция шлёт раз в ~48 с; первый пакет может опоздать).
class WgRadioCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "wg.radio"; }
    uint32_t intervalMs() const override { return 10000; }
    HealthResult run() override {
        const Cc1101Driver& r = Cc1101Driver::getInstance();
        if (!r.isHealthy()) return HealthResult::critical("CC1101_LOST");

        // W3.3: во время прогона сканера частота уходит с рабочей точки —
        // тишина на чужих точках сетки ожидаема, сторож на паузе (0.5.0).
        if (WeatherGateApp::getInstance().scanActive())
            return HealthResult::ok();

        uint32_t silenceMs =
            cfgGetUInt("wx.silence_min", 15) * 60000UL;
        if (r.lastPacketMs() == 0) {
            // Ни одного пакета со старта
            if (millis() > 5UL * 60 * 1000) {
                markSilent();
                return HealthResult::warning("RADIO_NO_PACKETS");
            }
            return HealthResult::ok();
        }
        uint32_t age = millis() - r.lastPacketMs();
        if (age > silenceMs * 2) {
            markSilent();
            return HealthResult::critical("RADIO_DARK");
        }
        if (age > silenceMs) {
            markSilent();
            return HealthResult::warning("RADIO_SILENCE");
        }
        _silenced = false;
        return HealthResult::ok();
    }
private:
    void markSilent() {
        if (!_silenced) {
            _silenced = true;
            EventBus::getInstance().post(wg_ev::radioSilence());
        }
    }
    bool _silenced = false;
};

static WgSensorCheck s_sensorCheck;
static WgRadioCheck  s_radioCheck;

// ============================================================================
// UI-ПРОВАЙДЕР
// ============================================================================
size_t WeatherGateUi::renderPublicHtml(char* buf, size_t bufSize) {
    // Бюджет публичной секции ~2 КБ (правило ядра). Улица + давление + эфир.
    const WeatherGateApp& app = WeatherGateApp::getInstance();
    const Bme280Driver&   d   = Bme280Driver::getInstance();
    const Cc1101Driver&   r   = Cc1101Driver::getInstance();
    int n = snprintf(buf, bufSize, "<b>Погодный шлюз</b>");
    if (n <= 0) return 0;
    size_t pos = (size_t)n;

    const WeatherGateApp::Outdoor& o = app.outdoor();
    if (o.valid) {
        n = snprintf(buf + pos, bufSize - pos,
            "<p>На улице: %.1f&deg;C (ощущ. %.1f), влажн. %.0f%%, "
            "ветер %.1f м/с, %u&deg;, дождь %.1f мм/ч%s<br>"
            "<small>обновлено %lu с назад, RSSI %d дБм</small></p>",
            (double)o.tempC, (double)app.feelsLikeC(),
            (double)o.humidityPct, (double)o.windMs, (unsigned)o.dirDeg,
            (double)o.rainMmPh,
            o.batteryLow ? " <b>&#9888; батарея</b>" : "",
            (unsigned long)((millis() - o.rxMs) / 1000), (int)r.rssiDbm());
        if (n > 0) pos += (size_t)n;
    } else {
        n = snprintf(buf + pos, bufSize - pos,
                     "<p>Улица: пакетов ещё не было.</p>");
        if (n > 0) pos += (size_t)n;
    }
    if (d.isHealthy()) {
        n = snprintf(buf + pos, bufSize - pos,
            "<p>Давление: %.0f мм рт.ст. (%.1f гПа у.м.)</p>",
            (double)(d.pressureSeaHpa() * 0.750062f),
            (double)d.pressureSeaHpa());
        if (n > 0) pos += (size_t)n;
    }
    n = snprintf(buf + pos, bufSize - pos,
                 "<p><a href=\"/web/wx.html\">Панель погоды и графики</a></p>");
    if (n > 0) pos += (size_t)n;
    return pos;
}

// Рабочий буфер даталог-запросов — BSS файла, НЕ стек HTTP-задачи.
// UNION как в smart_lock: сырые точки и агрегаты в одном запросе не
// встречаются (постмортем: два раздельных буфера ломали линковку DRAM).
union WgDlogQueryBuf {
    DlogPoint raw[DLOG_RAW_CAP];
    DlogAggr  aggr[320];
};
static WgDlogQueryBuf s_dlogQ;

// W3.2-diag1: условный диагностический лог (конфиг wx.diag). Форматирует
// в локальный буфер (обрезка под LOG_BODY_LEN=96 — намеренно, сообщения
// короткие) и пишет через публичный мост модуля: ModuleBase::log —
// protected, а свободные функции UI-провайдера членства не имеют.
static void s_diagLog(const char* fmt, ...) {
    if (!cfgGetBool("wx.diag", true)) return;
    char body[92];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    WeatherGateApp::getInstance().logDiag(body);
}

static bool wgApiDlogChannels(char* buf, size_t size) {
    DataLogService& dl = DataLogService::getInstance();
    size_t pos = 0;
    int n = snprintf(buf, size, "{\"channels\":[");
    if (n < 0) return true;
    pos = (size_t)n;
    for (uint8_t i = 0; i < dl.channelCount(); ++i) {
        char id[12], name[28], unit[8];
        if (!dl.channelInfo(i, id, sizeof(id), name, sizeof(name),
                            unit, sizeof(unit))) continue;
        n = snprintf(buf + pos, size - pos,
            "%s{\"i\":%u,\"id\":\"%s\",\"name\":\"%s\",\"unit\":\"%s\"}",
            i ? "," : "", i, id, name, unit);
        if (n < 0 || (size_t)n >= size - pos) break;
        pos += (size_t)n;
    }
    snprintf(buf + pos, size - pos, "]}");
    return true;
}

static bool wgApiDlog(const ShUiRequest& req, char* buf, size_t size,
                      int& status) {
    DataLogService& dl = DataLogService::getInstance();
    const char* chArg = req.getArg("ch");
    uint8_t ch = chArg ? (uint8_t)atoi(chArg) : 0;
    // W3.2-diag1: фиксируем запрос ДО проверок (поймать no_channel)
    s_diagLog("dlog: ch='%s'->%u count=%u range='%s'",
              chArg != nullptr ? chArg : "null", (unsigned)ch,
              (unsigned)dl.channelCount(),
              req.getArg("range") != nullptr ? req.getArg("range") : "null");
    if (ch >= dl.channelCount()) {
        status = 404;
        snprintf(buf, size, "{\"err\":\"no_channel\"}");
        return true;
    }
    // Диапазон -> ярус и фильтр времени (паттерн smart_lock).
    const char* range = req.getArg("range");
    uint32_t now = (uint32_t)TimeService::getInstance().getUnixTime();
    uint32_t fromTs = 0;
    bool raw = true, daily = false;
    if (range == nullptr || strcmp(range, "6h") == 0) {
        fromTs = now > 6UL * 3600 ? now - 6UL * 3600 : 0;
    } else if (strcmp(range, "24h") == 0) {
        raw = false; fromTs = now > 24UL * 3600 ? now - 24UL * 3600 : 0;
    } else if (strcmp(range, "7d") == 0) {
        raw = false; fromTs = now > 7UL * 86400 ? now - 7UL * 86400 : 0;
    } else if (strcmp(range, "30d") == 0) {
        raw = false; fromTs = 0;
    } else if (strcmp(range, "1y") == 0) {
        raw = false; daily = true; fromTs = 0;
    } else {
        status = 400;
        snprintf(buf, size, "{\"err\":\"range: 6h|24h|7d|30d|1y\"}");
        return true;
    }

    size_t pos = 0;
    int n;
    if (raw) {
        uint16_t cnt = dl.getRaw(ch, s_dlogQ.raw, DLOG_RAW_CAP, fromTs);
        cnt = dlog::decimateRaw(s_dlogQ.raw, cnt, s_dlogQ.raw,
                                DLOG_JSON_POINTS);
        s_diagLog("dlog: raw ch=%u n=%u dropped=%lu", (unsigned)ch,
                  (unsigned)cnt, (unsigned long)dl.droppedPoints(ch));
        n = snprintf(buf, size, "{\"fmt\":\"raw\",\"n\":%u,\"ts\":[", cnt);
        if (n < 0) return true;
        pos = (size_t)n;
        for (uint16_t i = 0; i < cnt; ++i) {
            n = snprintf(buf + pos, size - pos, "%s%lu", i ? "," : "",
                         (unsigned long)s_dlogQ.raw[i].ts);
            if (n < 0 || (size_t)n >= size - pos) { pos = size - 8; break; }
            pos += (size_t)n;
        }
        n = snprintf(buf + pos, size - pos, "],\"v\":[");
        if (n > 0) pos += (size_t)n;
        for (uint16_t i = 0; i < cnt; ++i) {
            n = snprintf(buf + pos, size - pos, "%s%.1f", i ? "," : "",
                         (double)s_dlogQ.raw[i].v);
            if (n < 0 || (size_t)n >= size - pos) { pos = size - 8; break; }
            pos += (size_t)n;
        }
        snprintf(buf + pos, size - pos, "]}");
        return true;
    }

    uint16_t cnt = dl.getTier(ch, daily, s_dlogQ.aggr, 320, fromTs);
    cnt = dlog::decimateAggr(s_dlogQ.aggr, cnt, s_dlogQ.aggr,
                             DLOG_JSON_POINTS);
    s_diagLog("dlog: tier ch=%u daily=%d n=%u dropped=%lu", (unsigned)ch,
              daily ? 1 : 0, (unsigned)cnt,
              (unsigned long)dl.droppedPoints(ch));
    n = snprintf(buf, size, "{\"fmt\":\"aggr\",\"n\":%u,\"ts\":[", cnt);
    if (n < 0) return true;
    pos = (size_t)n;
    for (uint16_t i = 0; i < cnt; ++i) {
        n = snprintf(buf + pos, size - pos, "%s%lu", i ? "," : "",
                     (unsigned long)s_dlogQ.aggr[i].ts);
        if (n < 0 || (size_t)n >= size - pos) { pos = size - 16; break; }
        pos += (size_t)n;
    }
    static const char* KEYS[3] = { "\"avg\":[", "\"min\":[", "\"max\":[" };
    for (uint8_t k = 0; k < 3; ++k) {
        n = snprintf(buf + pos, size - pos, "],%s", KEYS[k]);
        if (n > 0) pos += (size_t)n;
        for (uint16_t i = 0; i < cnt; ++i) {
            float v = k == 0 ? s_dlogQ.aggr[i].avg :
                      k == 1 ? s_dlogQ.aggr[i].mn  : s_dlogQ.aggr[i].mx;
            n = snprintf(buf + pos, size - pos, "%s%.1f", i ? "," : "",
                         (double)v);
            if (n < 0 || (size_t)n >= size - pos) { pos = size - 16; break; }
            pos += (size_t)n;
        }
    }
    snprintf(buf + pos, size - pos, "]}");
    return true;
}

bool WeatherGateUi::handleApi(const char* pathTail, const ShUiRequest& req,
                              char* responseBuf, size_t bufSize,
                              int& statusCode) {
    // W3.2-diag5: безусловный лог ВХОДА (ловим висячий tail из ядра:
    // если pathTail мусорный — увидим его здесь дословно).
    if (strncmp(pathTail, "dlog", 4) == 0 || strncmp(pathTail, "weather", 7) != 0)
        s_diagLog("api hit: tail='%.*s' len=%u tok=%s", 40, pathTail,
                  (unsigned)strlen(pathTail),
                  (req.token != nullptr && req.token[0] != '\0') ? "есть"
                                                                 : "НЕТ");
    if (strcmp(pathTail, "ping") == 0) {
        snprintf(responseBuf, bufSize, "{\"pong\":1,\"ms\":%lu}",
                 (unsigned long)millis());
        statusCode = 200;
        return true;
    }
    if (strcmp(pathTail, "wx") == 0) {
        // Текущие показания BME280 (диагностика датчика)
        const Bme280Driver& d = Bme280Driver::getInstance();
        char t[12] = "null", p[12] = "null", ps[12] = "null", h[12] = "null";
        if (d.isHealthy() && d.lastReadMs() != 0) {
            snprintf(t,  sizeof(t),  "%.2f", (double)d.temperatureC());
            snprintf(p,  sizeof(p),  "%.2f", (double)d.pressureHpa());
            snprintf(ps, sizeof(ps), "%.2f", (double)d.pressureSeaHpa());
            if (d.humidityValid())
                snprintf(h, sizeof(h), "%.1f", (double)d.humidityPct());
        }
        snprintf(responseBuf, bufSize,
                 "{\"model\":\"%s\",\"addr\":\"0x%02X\",\"temp\":%s,"
                 "\"press\":%s,\"press_sea\":%s,\"humidity\":%s,"
                 "\"age_ms\":%lu}",
                 d.model(), (unsigned)d.address(), t, p, ps, h,
                 d.lastReadMs() ? (unsigned long)(millis() - d.lastReadMs())
                                : 0UL);
        statusCode = 200;
        return true;
    }
    if (strcmp(pathTail, "radio") == 0) {
        // Статус приёмника CC1101 (диагностика эфира)
        Cc1101Driver& r = Cc1101Driver::getInstance();
        // W3.3 (0.5.0): сканер частоты. status — публичный (как probe);
        // start/abort/tune меняют эфир — только админ (паттерн dlog:
        // неадмину 404 ядра, существование путей не раскрываем).
        const char* scan = req.getArg("scan");
        if (scan != nullptr && scan[0] != '\0') {
            WeatherGateApp& app = WeatherGateApp::getInstance();
            if (strcmp(scan, "status") == 0) {
                app.scanStatusJson(responseBuf, bufSize);
                statusCode = 200;
                return true;
            }
            if (!HttpService::getInstance().isAdminToken(req.token))
                return false;   // 404 ядра
            if (strcmp(scan, "start") == 0) {
                const char* st = req.getArg("step");
                const char* dw = req.getArg("dwell");
                uint16_t stepX100 = (st != nullptr)
                    ? (uint16_t)(atof(st) * 100.0 + 0.5) : 2;
                uint16_t dwellS = (dw != nullptr)
                    ? (uint16_t)atoi(dw) : 60;
                char err[48] = "";
                if (app.scanStart(stepX100, dwellS, err, sizeof(err))) {
                    snprintf(responseBuf, bufSize, "{\"ok\":1}");
                } else {
                    snprintf(responseBuf, bufSize,
                             "{\"ok\":0,\"err\":\"%s\"}", err);
                    statusCode = 409;
                    return true;
                }
                statusCode = 200;
                return true;
            }
            if (strcmp(scan, "abort") == 0) {
                app.scanAbort();
                snprintf(responseBuf, bufSize, "{\"ok\":1}");
                statusCode = 200;
                return true;
            }
            if (strcmp(scan, "tune") == 0) {
                const char* fq = req.getArg("freq");
                float mhz = (fq != nullptr) ? (float)atof(fq) : 0.0f;
                if (app.scanTune(mhz)) {
                    snprintf(responseBuf, bufSize,
                             "{\"ok\":1,\"freq\":%.2f}", (double)r.freqMHz());
                    statusCode = 200;
                } else {
                    snprintf(responseBuf, bufSize,
                             "{\"ok\":0,\"err\":\"freq out of range or "
                             "radio unhealthy\"}");
                    statusCode = 400;
                }
                return true;
            }
            return false;   // неизвестный scan-аргумент -> 404 ядра
        }
        // 5.8.4-pre3: probe-контракт бенча (Issue #1, 27.08).
        //   ?probe=1 -> rssi_now = readRssiNow()
        //   ?probe=2 -> ручной перевход RX (setFreqMHz текущей) + rssi_now
        // 5.8.4-pre4: probe=3 -> readback конфиг-регистров во время RX
        //   (agcctrl1=0x1C, agcctrl2=0x1B) + rssi_now.
        // 5.8.4-pre5: marcstate (0x35) через публичный readMarcstate()
        //   (ожидание в RX: 0x0D).
        // Публичный эндпоинт, RSSI — не секрет; SPI только из task-контекста.
        bool hasNow = false;
        bool hasRegs = false;
        int16_t rssiNow = 0;
        uint8_t regAgc1 = 0, regAgc2 = 0, marc = 0;
        const char* probe = req.getArg("probe");
        if (probe != nullptr && probe[0] == '2') {
            r.setFreqMHz(r.freqMHz());      // SIDLE->SFRX->SRX: разморозка латча
            rssiNow = r.readRssiNow();
            hasNow = true;
        } else if (probe != nullptr && probe[0] == '1') {
            rssiNow = r.readRssiNow();
            hasNow = true;
        } else if (probe != nullptr && probe[0] == '3') {
            regAgc1 = r.readConfigReg(0x1C);
            regAgc2 = r.readConfigReg(0x1B);
            marc = r.readMarcstate();
            rssiNow = r.readRssiNow();
            hasNow = true;
            hasRegs = true;
        }
        int n = snprintf(responseBuf, bufSize,
                 "{\"healthy\":%d,\"freq\":%.2f,\"pkt\":%lu,\"dup\":%lu,"
                 "\"edges_dropped\":%lu,\"rssi\":%d,\"age_ms\":%lu,"
                 "\"rx_reenters\":%lu",
                 r.isHealthy() ? 1 : 0, (double)r.freqMHz(),
                 (unsigned long)r.packetSeq(), (unsigned long)r.dupSeq(),
                 (unsigned long)r.edgesDropped(), (int)r.rssiDbm(),
                 r.lastPacketMs() ? (unsigned long)(millis() - r.lastPacketMs())
                                  : 0UL,
                 (unsigned long)r.rxReenters());
        if (n > 0 && hasRegs)
            n += snprintf(responseBuf + n, bufSize - (size_t)n,
                          ",\"agcctrl1\":\"0x%02X\",\"agcctrl2\":\"0x%02X\","
                          "\"marcstate\":\"0x%02X\"",
                          regAgc1, regAgc2, marc);
        if (n > 0 && hasNow)
            snprintf(responseBuf + n, bufSize - (size_t)n,
                     ",\"rssi_now\":%d}", (int)rssiNow);
        else if (n > 0)
            snprintf(responseBuf + n, bufSize - (size_t)n, "}");
        statusCode = 200;
        return true;
    }
    if (strcmp(pathTail, "weather") == 0) {
        // Контракт smart_lock + расширения (публичный: погода — не секрет).
        // HTTP отдаёт ПОЛНУЮ версию (статистика 24ч, тренд, t° котельной);
        // MQTT-публикация остаётся компактной (бюджет MQTT_BODY_LEN=256).
        WeatherGateApp::getInstance().weatherJsonFull(responseBuf, bufSize);
        statusCode = 200;
        return true;
    }
    // Дальше — только админ (графики = история, паттерн smart_lock)
    if (!HttpService::getInstance().isAdminToken(req.token)) {
        if (strncmp(pathTail, "dlog", 4) == 0)   // W3.2-diag1
            s_diagLog("api '%s': tok=%s admin=NO -> false",
                      pathTail, (req.token != nullptr && req.token[0] != '\0')
                                ? "есть" : "НЕТ");
        return false;   // 404 ядра: не раскрываем существование путей
    }
    if (strncmp(pathTail, "dlog", 4) == 0)       // W3.2-diag1
        s_diagLog("api '%s': admin=ok", pathTail);
    if (strcmp(pathTail, "dlog/channels") == 0) {
        statusCode = 200;
        return wgApiDlogChannels(responseBuf, bufSize);
    }
    if (strcmp(pathTail, "dlog") == 0) {
        statusCode = 200;
        return wgApiDlog(req, responseBuf, bufSize, statusCode);
    }
    // W3.2-diag5: сюда попадает и МУСОРНЫЙ tail (висячий указатель ядра)
    s_diagLog("api miss: tail='%.*s' -> false (404 ядра)", 40, pathTail);
    return false;   // неизвестный профильный путь -> 404 ядра
}

// ============================================================================
// МОДУЛЬ
// ============================================================================
volatile bool WeatherGateApp::s_altTaskRunning = false;

WeatherGateApp& WeatherGateApp::getInstance() {
    static WeatherGateApp instance;
    return instance;
}

void WeatherGateApp::logDiag(const char* body) {
    log(LogLevel::Info, "DIAG %s", body != nullptr ? body : "?");
}

void WeatherGateApp::registerExtensions() {
    // Правило 23 (задание ядерной ветки 04.09.2026, 0.5.1): группы
    // «Планировщик», «Счётчики», «Звук» — сироты на этом профиле (ни
    // одного SCHED_EVENT, CounterService не используется, DFPlayer/звука
    // на шлюзе нет — все три сервиса регистрирует ядро безусловно).
    // Поля и сервисы живут полной программой, /admin показывает всё —
    // пометка лишь запрещает показ в профильных панелях (hg:1).
    ConfigService::getInstance().setHiddenGroups("Планировщик,Счётчики,Звук");

    // Конфиг-схема профиля. Новые поля — ТОЛЬКО в конец списка (правило
    // JSON-конфига: сдвиг индексов ломает сохранённые значения).
    bool ok = ConfigService::getInstance().addFields("Датчик BME280", {
        {"wx.i2c_addr",        ConfigType::STRING, "auto", 0, 0,
         CFG_CRITICAL, "Датчик BME280", "Адрес I2C (auto/0x76/0x77)"},
        {"wx.altitude_m",      ConfigType::FLOAT, "0", -500, 9000,
         CFG_NONE, "Датчик BME280", "Высота установки над у.м., м"},
        {"wx.press_offset_hpa", ConfigType::FLOAT, "0", -50, 50,
         CFG_NONE, "Датчик BME280", "Поправка давления, гПа"},
        {"wx.lat",             ConfigType::FLOAT, "0", -90, 90,
         CFG_NONE, "Датчик BME280", "Широта (для авто-высоты)"},
        {"wx.lon",             ConfigType::FLOAT, "0", -180, 180,
         CFG_NONE, "Датчик BME280", "Долгота (для авто-высоты)"},
    });
    if (!ok) {
        // fail-fast: схема не влезла/поле отвергнуто — стоп, смотреть
        // CFG_MAX_FIELDS и валидность описаний (правило руководства).
        log(LogLevel::Error, "addFields 'Датчик BME280' failed");
    }

    // W2: радиотракт CC1101.
    ok = ConfigService::getInstance().addFields("Радио CC1101", {
        {"wx.rf_freq_mhz",     ConfigType::FLOAT, "915.00", 914, 916,
         CFG_CRITICAL, "Радио CC1101", "Частота приёма, МГц"},
    });
    if (!ok) log(LogLevel::Error, "addFields 'Радио CC1101' failed");

    // W3: телеметрия и сторожа.
    ok = ConfigService::getInstance().addFields("Телеметрия и сторожа", {
        {"wx.mqtt_en",         ConfigType::BOOL, "1", 0, 0,
         CFG_NONE, "Телеметрия и сторожа", "Публикация weather-JSON в MQTT"},
        {"wx.pub_min",         ConfigType::UINT, "5", 1, 60,
         CFG_NONE, "Телеметрия и сторожа", "Период retained-публикации, мин"},
        // 0.3.8 (W4): зеркало weather-JSON в произвольный топик (тот же
        // JSON, retained). Пусто = выкл. Читается при каждой публикации,
        // поэтому CFG_NONE — применяется без ребута.
        {"wx.mirror_topic",    ConfigType::STRING, "", 0, 0,
         CFG_NONE, "Телеметрия и сторожа",
         "Зеркало weather-JSON в топик (пусто = выкл)"},
        {"wx.silence_min",     ConfigType::UINT, "15", 2, 120,
         CFG_NONE, "Телеметрия и сторожа", "Тишина эфира -> тревога, мин"},
        {"wx.autoalt_en",      ConfigType::BOOL, "1", 0, 0,
         CFG_NONE, "Телеметрия и сторожа",
         "Авто-высота по координатам (при FULL-сети)"},
        // W3.2-diag1: расширенный лог пути даталога (поймать 404 графиков
        // и заморозку RSSI). Включается из /admin без перепрошивки.
        // 0.5.2: дефолт погашен ("1"->"0") — расследования W3.2 закрыты,
        // soak 72ч зелёный; лог шумел. Поле и индекс сохранены (закон
        // флота), при надобности включается тумблером.
        {"wx.diag",            ConfigType::BOOL, "0", 0, 0,
         CFG_NONE, "Телеметрия и сторожа",
         "Расширенный лог диагностики (даталог, радио)"},
        // W3.2/5.8.4: сырой байт AGCCTRL1 (пороги Carrier Sense шторки GDO2).
        // Поле профиля (конец схемы!); драйвер ядра пишет регистр 0x1C,
        // если значение 1..255. 0 = регистр не трогаем.
        // Бит 6 (AGC_LNA_PRIORITY) держать выставленным — база 0x40.
        // Бенч 29.08 (стенд, pre5+0.3.6): ABS-ступень исчерпана без эффекта
        // до +7 дБ — дефолт схемы 71 (0x47, максимум ABS).
        {"wx.rf_agcctrl1",     ConfigType::INT, "71", 0, 255,
         CFG_CRITICAL, "Телеметрия и сторожа",
         "AGCCTRL1 raw (дефолт 71=0x47, макс. ABS-порог; 0=не писать)"},
        // 5.8.4-pre3: вторая ступень порога CS (регистр 0x1B: MAX_LNA_GAIN/
        // MAX_DVGA_GAIN/MAGN_TARGET). 0 = не писать (reset чипа 0x03:
        // MAGN_TARGET=33 дБ, усиления максимальные).
        // Бенч 29.08: шторм GDO0 умирает на 0x13 (LNA −6,1 дБ), приём
        // полный до −17,1 дБ; рабочая точка 27 (0x1B, −7,4 дБ) — +1 ступень
        // запаса над точкой подавления. Это дефолт схемы.
        {"wx.rf_agcctrl2",     ConfigType::INT, "27", 0, 255,
         CFG_CRITICAL, "Телеметрия и сторожа",
         "AGCCTRL2 raw (дефолт 27=0x1B: LNA −7,4 дБ; 0=не писать)"},
    });
    if (!ok) log(LogLevel::Error, "addFields 'Телеметрия и сторожа' failed");

    // ПАЗ-проверки домена (механизм — HealthMonitor, содержимое — профиль)
    HealthMonitor::getInstance().registerCheck(&s_sensorCheck);
    HealthMonitor::getInstance().registerCheck(&s_radioCheck);

    HttpService::getInstance().setUiProvider(&WeatherGateUi::getInstance());
}

void WeatherGateApp::init() {
    _initialized = true;   // ресурсов у модуля нет; железо — у драйверов
    log(LogLevel::Info, "init: profile weather_gate, stage W3");
}

void WeatherGateApp::start() {
    _started = true;

    // События инфраструктуры
    EventBus::getInstance().subscribe(SH_EVENT_DEGRADED_LEVEL, this);
    // 0.3.8: E2 — авто-объявление в HA при каждой MQTT-сессии (образец —
    // smart_lock); заодно свежая retained-погода для подписчиков после
    // рестарта брокера
    EventBus::getInstance().subscribe(SH_EVENT_MQTT_CONNECTED, this);

    // Каналы даталоггера (сервис пассивен — каналы объявляет профиль)
    DataLogService& dlog = DataLogService::getInstance();
    // Подписи — короче 28-байтного бюджета канала (кириллица 2 Б/символ;
    // «Улица, температура» резалась посередине символа — урок стенда 23.08)
    _chOutT  = dlog.registerChannel("wx_ot", "Улица, темп.", "°C");
    _chOutH  = dlog.registerChannel("wx_oh", "Улица, влажн.", "%");
    _chPress = dlog.registerChannel("wx_p",  "Давление у.м.", "гПа");
    _chWind  = dlog.registerChannel("wx_w",  "Ветер", "м/с");
    _chRain  = dlog.registerChannel("wx_r",  "Дождь", "мм/ч");
    // W3.2-diag1: результат регистрации (при -1 канала графики дадут 404)
    s_diagLog("dlog init: ids=%d,%d,%d,%d,%d count=%u heap=%lu",
              _chOutT, _chOutH, _chPress, _chWind, _chRain,
              (unsigned)dlog.channelCount(),
              (unsigned long)ESP.getFreeHeap());

    // Авто-высота: если сеть уже FULL (старт после стабильной линии)
    maybeRequestAltitude();

    // Стенд соответствия D1: манифест собираем заново из пинов профиля
    // (describeHardware чист — только константы, без железа).
    HardwareManifest m;
    WeatherGateProfile p;
    p.describeHardware(m);
    conformance::runAll("weather_gate", m);
}

void WeatherGateApp::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    _started = false;
    _initialized = false;
}

bool WeatherGateApp::canHandleEvent(int32_t id) const {
    return id == SH_EVENT_DEGRADED_LEVEL || id == SH_EVENT_MQTT_CONNECTED;
}

void WeatherGateApp::onEvent(int32_t eventId, const ShEventData* data) {
    if (eventId == SH_EVENT_DEGRADED_LEVEL) {
        // Сеть поднялась до FULL — повод для авто-высоты
        if (data != nullptr && data->code == (int32_t)DegradationLevel::Full)
            maybeRequestAltitude();
        return;
    }
    if (eventId == SH_EVENT_MQTT_CONNECTED) {
        // 0.3.8: устройство объявляет себя само (конфиги retained — HA
        // получит их даже после рестарта любой из сторон)
        publishHaDiscovery();
        // Свежая retained-погода сразу на новой сессии, не дожидаясь
        // следующего пакета/периода
        if (_out.valid && cfgGetBool("wx.mqtt_en", true))
            publishWeatherMqtt();
        return;
    }
}

// ============================================================================
// TICK (1 с): новые пакеты, даталог давления, периодическая публикация,
// запуск задачи авто-высоты. Бюджет 50 мс — всё короткое, HTTP уехал
// в отдельную задачу.
// ============================================================================
void WeatherGateApp::tick() {
    uint32_t now = millis();

    // Новый радиопакет?
    const Cc1101Driver& r = Cc1101Driver::getInstance();
    if (r.hasPacket() && r.packetSeq() != _seenPktSeq) {
        _seenPktSeq = r.packetSeq();
        onNewRadioPacket();
    }

    scanTick();   // W3.3: машина сканера частоты (нет активного — пустой)

    // Давление — по своему ритму (раз в минуту), независимо от эфира
    const Bme280Driver& d = Bme280Driver::getInstance();
    if (_chPress >= 0 && d.isHealthy() && d.lastReadMs() != 0 &&
        now - _lastPressLogMs >= 60000UL) {
        _lastPressLogMs = now;
        bool rcP = DataLogService::getInstance().logPoint(
            _chPress, d.pressureSeaHpa());
        if (!rcP)   // W3.2-diag1: молчаливый отказ — в лог
            s_diagLog("wr press: rc=0 ch=%d unix=%lu", _chPress,
                      (unsigned long)TimeService::getInstance().getUnixTime());
    }

    // Периодическая retained-публикация (давление дрейфует и без пакетов;
    // retained-топик должен быть свежим для подписчиков после ребута УД)
    uint32_t pubMs = cfgGetUInt("wx.pub_min", 5) * 60000UL;
    if (cfgGetBool("wx.mqtt_en", true) && _out.valid &&
        now - _lastPubMs >= pubMs) {
        publishWeatherMqtt();
    }

    // Кэш статистики 24 ч для публичной панели (раз в 5 мин, из tick —
    // единственный писатель; getTier часового яруса за сутки — ≤27 записей)
    if (now - _lastStatsMs >= 300000UL) {
        _lastStatsMs = now;
        refreshStats24();
    }

    // Авто-высота: запуск одноразовой задачи (loop не блокируем)
    if (_altRequested && !_altDone && !s_altTaskRunning &&
        now >= _altNextRetryMs) {
        _altRequested = false;
        xTaskCreate(&WeatherGateApp::altitudeTask, "wg_alt", 8192,
                    nullptr, 1, nullptr);
    }
}

// ============================================================================
// ОБРАБОТКА ПАКЕТА
// ============================================================================
void WeatherGateApp::onNewRadioPacket() {
    const Cc1101Driver& r = Cc1101Driver::getInstance();
    const fo::WeatherPacket& p = r.lastPacket();
    TimeService& ts = TimeService::getInstance();
    uint32_t unix = ts.isTimeValid() ? (uint32_t)ts.getUnixTime() : 0;

    _out.valid      = true;
    _out.rxMs       = millis();
    _out.tempC      = p.tempC;
    _out.humidityPct= p.humidity;
    _out.windMs     = p.windMs;
    _out.gustMs     = p.gustMs;
    _out.dirDeg     = p.dirDeg;
    _out.batteryLow = p.batteryLow;
    _out.deviceId   = p.deviceId;
    if (unix != 0) {
        _rain.add(p.rainRaw, unix);
        _out.rainMmPh = _rain.rateMmPh(unix);
    }

    // Даталог уличных каналов (logPoint сам отбросит точку без времени)
    DataLogService& dl = DataLogService::getInstance();
    bool rcT = false, rcH = false, rcW = false, rcR = false;
    if (_chOutT >= 0) rcT = dl.logPoint(_chOutT, p.tempC);
    if (_chOutH >= 0) rcH = dl.logPoint(_chOutH, p.humidity);
    if (_chWind >= 0) rcW = dl.logPoint(_chWind, p.windMs);
    if (_chRain >= 0) rcR = dl.logPoint(_chRain, _out.rainMmPh);
    // W3.2-diag1: успех записи + радио-счётчики (RSSI-заморозка, шторм фронтов)
    s_diagLog("wr: rc=%d%d%d%d unix=%lu pkt=%lu dup=%lu edges=%lu rssi=%d",
              rcT ? 1 : 0, rcH ? 1 : 0, rcW ? 1 : 0, rcR ? 1 : 0,
              (unsigned long)unix, (unsigned long)r.packetSeq(),
              (unsigned long)r.dupSeq(), (unsigned long)r.edgesDropped(),
              (int)r.rssiDbm());

    // MQTT + событие шины
    if (cfgGetBool("wx.mqtt_en", true)) publishWeatherMqtt();
    ShEventData ev;
    ev.clear();
    ev.sourceModule = getModuleId();
    snprintf(ev.payload, sizeof(ev.payload), "T=%.1f W=%.1f R=%.1f",
             (double)p.tempC, (double)p.windMs, (double)_out.rainMmPh);
    EventBus::getInstance().post(wg_ev::radioPacket(), &ev);
}

// ============================================================================
// СТАТИСТИКА 24 Ч + БАРОТРЕНД (W3.2; кэш для панели, писатель — только tick)
// ============================================================================
void WeatherGateApp::refreshStats24() {
    TimeService& ts = TimeService::getInstance();
    if (!ts.isTimeValid()) return;   // без времени ярусы недостоверны
    uint32_t unix   = (uint32_t)ts.getUnixTime();
    uint32_t fromTs = unix - 86400UL;
    DataLogService& dl = DataLogService::getInstance();
    DlogAggr a[32];   // часовой ярус за сутки: 24 закрытых + открытое ведро
    bool any = false;

    if (_chOutT >= 0) {
        uint16_t n = dl.getTier((uint8_t)_chOutT, false, a, 32, fromTs);
        if (n > 0) {
            float mn = a[0].mn, mx = a[0].mx;
            for (uint16_t i = 1; i < n; ++i) {
                if (a[i].mn < mn) mn = a[i].mn;
                if (a[i].mx > mx) mx = a[i].mx;
            }
            _mnT24 = mn; _mxT24 = mx; any = true;
        }
    }
    if (_chWind >= 0) {
        uint16_t n = dl.getTier((uint8_t)_chWind, false, a, 32, fromTs);
        if (n > 0) {
            float mx = a[0].mx;
            for (uint16_t i = 1; i < n; ++i) if (a[i].mx > mx) mx = a[i].mx;
            _mxW24 = mx; any = true;
        }
    }
    if (_chPress >= 0) {
        uint16_t n = dl.getTier((uint8_t)_chPress, false, a, 32, fromTs);
        if (n > 0) {
            float mn = a[0].mn, mx = a[0].mx;
            for (uint16_t i = 1; i < n; ++i) {
                if (a[i].mn < mn) mn = a[i].mn;
                if (a[i].mx > mx) mx = a[i].mx;
            }
            _mnP24 = mn; _mxP24 = mx; any = true;
            // Тренд: свежее (открытое) ведро против ведра ~3 ч назад.
            float nowP = a[n - 1].avg, refP = a[0].avg;
            for (uint16_t i = n; i-- > 0;) {
                if (a[i].ts <= unix - 10800UL) { refP = a[i].avg; break; }
            }
            _trend = wxc::baroTrend3h(nowP - refP);
        }
    }
    _statsValid = any;
}

// ============================================================================
// WEATHER-JSON (контракт smart_lock + расширения)
// ============================================================================
float WeatherGateApp::feelsLikeC() const {
    return wxc::feelsLikeC(_out.tempC, _out.humidityPct, _out.windMs);
}

const char* WeatherGateApp::weatherState() const {
    return wxc::weatherState(_out.rainMmPh, _out.windMs, _out.gustMs);
}

size_t WeatherGateApp::weatherJson(char* buf, size_t bufSize) const {
    if (!_out.valid) {
        int n = snprintf(buf, bufSize, "{\"valid\":0}");
        return n > 0 ? (size_t)n : 0;
    }
    const Bme280Driver& d = Bme280Driver::getInstance();
    const Cc1101Driver& r = Cc1101Driver::getInstance();
    char p[12] = "null", ps[12] = "null";
    if (d.isHealthy() && d.lastReadMs() != 0) {
        snprintf(p,  sizeof(p),  "%.2f", (double)d.pressureHpa());
        snprintf(ps, sizeof(ps), "%.2f", (double)d.pressureSeaHpa());
    }
    // Бюджет MQTT_BODY_LEN (256): строка ~190 байт с запасом.
    int n = snprintf(buf, bufSize,
        "{\"valid\":1,\"temp\":%.2f,\"feels_like\":%.2f,\"state\":\"%s\","
        "\"humidity\":%.1f,\"wind\":%.2f,\"gust\":%.2f,\"dir\":%u,"
        "\"rain\":%.2f,\"press\":%s,\"press_sea\":%s,"
        "\"rssi\":%d,\"batt\":%d,\"age_s\":%lu}",
        (double)_out.tempC, (double)feelsLikeC(), weatherState(),
        (double)_out.humidityPct, (double)_out.windMs, (double)_out.gustMs,
        (unsigned)_out.dirDeg, (double)_out.rainMmPh, p, ps,
        (int)r.rssiDbm(), _out.batteryLow ? 0 : 1,
        (unsigned long)((millis() - _out.rxMs) / 1000));
    return n > 0 ? (size_t)n : 0;
}

size_t WeatherGateApp::weatherJsonFull(char* buf, size_t bufSize) const {
    if (!_out.valid) {
        int n = snprintf(buf, bufSize, "{\"valid\":0}");
        return n > 0 ? (size_t)n : 0;
    }
    const Bme280Driver& d = Bme280Driver::getInstance();
    const Cc1101Driver& r = Cc1101Driver::getInstance();
    char p[12] = "null", ps[12] = "null", it[12] = "null";
    if (d.isHealthy() && d.lastReadMs() != 0) {
        snprintf(p,  sizeof(p),  "%.2f", (double)d.pressureHpa());
        snprintf(ps, sizeof(ps), "%.2f", (double)d.pressureSeaHpa());
        snprintf(it, sizeof(it), "%.1f", (double)d.temperatureC());
    }
    int n = snprintf(buf, bufSize,
        "{\"valid\":1,\"temp\":%.2f,\"feels_like\":%.2f,\"state\":\"%s\","
        "\"humidity\":%.1f,\"wind\":%.2f,\"gust\":%.2f,\"dir\":%u,"
        "\"rain\":%.2f,\"press\":%s,\"press_sea\":%s,"
        "\"rssi\":%d,\"batt\":%d,\"age_s\":%lu,\"in_temp\":%s",
        (double)_out.tempC, (double)feelsLikeC(), weatherState(),
        (double)_out.humidityPct, (double)_out.windMs, (double)_out.gustMs,
        (unsigned)_out.dirDeg, (double)_out.rainMmPh, p, ps,
        (int)r.rssiDbm(), _out.batteryLow ? 0 : 1,
        (unsigned long)((millis() - _out.rxMs) / 1000), it);
    // Статистика 24 ч — только если кэш валиден (поле просто отсутствует)
    if (n > 0 && (size_t)n < bufSize && _statsValid) {
        n += snprintf(buf + n, bufSize - (size_t)n,
            ",\"tmin24\":%.1f,\"tmax24\":%.1f,\"pmin24\":%.1f,"
            "\"pmax24\":%.1f,\"wmax24\":%.1f,\"trend\":%d",
            (double)_mnT24, (double)_mxT24, (double)_mnP24,
            (double)_mxP24, (double)_mxW24, (int)_trend);
    }
    if (n > 0 && (size_t)n < bufSize)
        n += snprintf(buf + n, bufSize - (size_t)n, "}");
    return n > 0 ? (size_t)n : 0;
}

void WeatherGateApp::publishWeatherMqtt() {
    if (!_out.valid) return;
    char js[MQTT_BODY_LEN];
    weatherJson(js, sizeof(js));
    // retained: подписчик (smart_lock, HA) получает свежую погоду сразу
    // после подписки, не дожидаясь следующего пакета
    bool rc =
        MqttTransport::getInstance().publishStateSuffix("weather", js, true);
    // 0.3.8 (W4): зеркало в произвольный топик (пусто = выкл)
    char mirror[CFG_VALUE_LEN];
    cfgGetStr("wx.mirror_topic", mirror, sizeof(mirror), "");
    if (mirror[0] != '\0')
        rc = MqttTransport::getInstance().publishRaw(mirror, js, true) && rc;
    // 0.3.8: учёт результата — молчаливый отказ publishRaw (нет клиента,
    // outbox выкл) раньше было не отличить от «опубликовано». Лог по
    // фронту: переходы ok<->fail, не каждый вызов (пакеты идут раз ~48 с).
    if (rc != _mqttPubOk) {
        _mqttPubOk = rc;
        log(rc ? LogLevel::Info : LogLevel::Warning,
            rc ? "mqtt: weather pub восстановлена"
               : "mqtt: weather pub ОТКАЗ (publishRaw=false)");
    }
    _lastPubMs = millis();
}

// ============================================================================
// HOME ASSISTANT DISCOVERY (E2, 0.3.8: устройство объявляет себя само)
// ============================================================================
// Образец — smart_lock. Конфиги retained + availability на ядерном LWT
// (<prefix>/<id>/state): HA видит устройство offline при внезапной смерти
// контроллера. Все погодные сущности читают ОДИН retained weather-JSON
// (<prefix>/<id>/weather) через val_tpl — новых топиков не плодим.
// Публикуется на SH_EVENT_MQTT_CONNECTED (каждая сессия — ре-анонс).
// ============================================================================
void WeatherGateApp::publishHaDiscovery() {
    if (!cfgGetBool("mqtt.ha_discovery", true)) return;
    MqttTransport& mqtt = MqttTransport::getInstance();

    const char* id = NetworkService::getInstance().deviceId();
    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");

    // Общий хвост: availability + карточка устройства (sw — реальная
    // версия прошивки, урок 5.0.10)
    char dev[288];
    snprintf(dev, sizeof(dev),
        ",\"avty_t\":\"%s/%s/state\",\"pl_avail\":\"online\","
        "\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"],"
        "\"name\":\"%s\",\"mf\":\"MicroOS\",\"mdl\":\"weather_gate\","
        "\"sw\":\"%s\"}",
        prefix, id, id, NetworkService::getInstance().hostname(),
        UpdateService::getInstance().firmwareVersion());

    // Все weather-сенсоры смотрят в один топик
    char wx[MQTT_TOPIC_LEN];
    snprintf(wx, sizeof(wx), "%s/%s/weather", prefix, id);
    char tel[MQTT_TOPIC_LEN];
    snprintf(tel, sizeof(tel), "%s/%s/telemetry", prefix, id);

    char topic[MQTT_TOPIC_LEN];
    char cfg[768];

    // --- Улица (из weather-JSON) ---------------------------------------------
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_temp/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Улица, температура\",\"uniq_id\":\"%s_temp\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.temp }}\"%s}", id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_feels/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Ощущается как\",\"uniq_id\":\"%s_feels\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.feels_like }}\"%s}", id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_hum/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Улица, влажность\",\"uniq_id\":\"%s_hum\","
        "\"dev_cla\":\"humidity\",\"unit_of_meas\":\"%%\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.humidity }}\"%s}", id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_wind/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Ветер\",\"uniq_id\":\"%s_wind\","
        "\"dev_cla\":\"wind_speed\",\"unit_of_meas\":\"m/s\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.wind }}\"%s}", id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_gust/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Порывы ветра\",\"uniq_id\":\"%s_gust\","
        "\"dev_cla\":\"wind_speed\",\"unit_of_meas\":\"m/s\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.gust }}\"%s}", id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_dir/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Направление ветра\",\"uniq_id\":\"%s_dir\","
        "\"unit_of_meas\":\"°\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.dir }}\"%s}", id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_rain/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Дождь\",\"uniq_id\":\"%s_rain\","
        "\"dev_cla\":\"precipitation_intensity\",\"unit_of_meas\":\"mm/h\","
        "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.rain }}\"%s}",
        id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    // Давление у.м. — BME280 (null, пока датчик не прочитан: HA покажет
    // «unknown», честнее выдуманного числа)
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_press/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Давление у.м.\",\"uniq_id\":\"%s_press\","
        "\"dev_cla\":\"atmospheric_pressure\",\"unit_of_meas\":\"hPa\","
        "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.press_sea }}\"%s}",
        id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_rssi/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Сигнал уличного блока\",\"uniq_id\":\"%s_rssi\","
        "\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\","
        "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.rssi }}\"%s}",
        id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    // Батарея уличного блока: в weather-JSON batt=1 норма / 0 низкий —
    // для binary_sensor.battery ON = «низкий»
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/%s_batt/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Батарея уличного блока\",\"uniq_id\":\"%s_batt\","
        "\"dev_cla\":\"battery\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ 'ON' if value_json.batt == 0 else 'OFF' }}\"%s}",
        id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_age/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Возраст пакета\",\"uniq_id\":\"%s_age\","
        "\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.age_s }}\"%s}", id, wx, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Сам контроллер (из ядерной telemetry) --------------------------------
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

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_cput/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Температура CPU\",\"uniq_id\":\"%s_cput\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.cpu_t }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    log(LogLevel::Info, "HA discovery: 14 entities announced");
}

// ============================================================================
// АВТО-ВЫСОТА (wx.lat/wx.lon -> wx.altitude_m)
// ============================================================================
// Одноразовый HTTP GET api.open-meteo.com/v1/elevation. Условия: сеть FULL,
// высота не задана (0), координаты заданы, не записывали ранее. Запись —
// через ConfigService::set: валидация диапазона + персистентность (после
// ребута повторный запрос не нужен); Bme280Driver читает поле каждый цикл
// и подхватывает значение мгновенно. Неудача -> повтор не раньше часа.
void WeatherGateApp::maybeRequestAltitude() {
    if (_altDone || !cfgGetBool("wx.autoalt_en", true)) return;
    if (cfgGetFloat("wx.altitude_m", 0.0f) != 0.0f) { _altDone = true; return; }
    if (cfgGetFloat("wx.lat", 0.0f) == 0.0f &&
        cfgGetFloat("wx.lon", 0.0f) == 0.0f) return;   // координат нет — ждём
    if (NetworkService::getInstance().degradationLevel() !=
        DegradationLevel::Full) return;
    _altRequested = true;
}

void WeatherGateApp::altitudeTask(void*) {
    s_altTaskRunning = true;
    WeatherGateApp& self = WeatherGateApp::getInstance();
    float lat = cfgGetFloat("wx.lat", 0.0f);
    float lon = cfgGetFloat("wx.lon", 0.0f);
    char url[160];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/elevation?latitude=%.4f&longitude=%.4f",
             (double)lat, (double)lon);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(4000);
    bool ok = false;
    if (http.begin(client, url)) {
        if (http.GET() == 200) {
            // Тело ~40 байт: {"elevation":[123.0]}. Буфер, не String
            // (правило платформы — без динамики даже в одноразовой задаче).
            char body[128];
            size_t got = http.getStream().readBytes(body, sizeof(body) - 1);
            body[got] = '\0';
            const char* p = strstr(body, "\"elevation\":[");
            if (p != nullptr) {
                float v = (float)atof(p + 13);
                if (v > -500.0f && v < 9000.0f) {
                    char val[16];
                    snprintf(val, sizeof(val), "%.0f", (double)v);
                    if (ConfigService::getInstance().set("wx.altitude_m", val)) {
                        self._altDone = true;
                        ok = true;
                        self.log(LogLevel::Info,
                                 "auto-altitude: %.0f m by %.4f,%.4f",
                                 (double)v, (double)lat, (double)lon);
                    }
                }
            }
        }
        http.end();
    }
    if (!ok) {
        self._altNextRetryMs = millis() + 3600000UL;   // повтор через час
        self.log(LogLevel::Warning, "auto-altitude: fetch failed, retry in 1h");
    }
    s_altTaskRunning = false;
    vTaskDelete(nullptr);
}

// ============================================================================
// СКАНЕР ЧАСТОТЫ (W3.3, 0.5.0)
// Машина состояний тикает из tick() (1 с): на точке сетки копим шум
// (1 выборка readRssiNow в секунду) и пакеты (дельта packetSeq — серия из
// 6 копий станции уже дедуплицирована драйвером). По окну dwell — перестройка
// на следующую точку; по завершении — возврат на рабочую частоту с readback
// (урок №23: подбор без readback — гадание). SPI — только из task-контекста
// tick (как probe-обработчики), HTTP лишь стартует/читает/отменяет.
// ============================================================================
bool WeatherGateApp::scanStart(uint16_t stepX100, uint16_t dwellS,
                               char* err, size_t errSize) {
    Cc1101Driver& r = Cc1101Driver::getInstance();
    if (_scan.active) {
        snprintf(err, errSize, "scan already running");
        return false;
    }
    if (!r.isHealthy()) {
        snprintf(err, errSize, "cc1101 not healthy");
        return false;
    }
    if (stepX100 < wgs::SCAN_STEP_MIN_X100) stepX100 = wgs::SCAN_STEP_MIN_X100;
    if (stepX100 > wgs::SCAN_STEP_MAX_X100) stepX100 = wgs::SCAN_STEP_MAX_X100;
    if (dwellS < wgs::SCAN_DWELL_MIN_S) dwellS = wgs::SCAN_DWELL_MIN_S;
    if (dwellS > wgs::SCAN_DWELL_MAX_S) dwellS = wgs::SCAN_DWELL_MAX_S;

    _scan.homeX100 = (uint32_t)(r.freqMHz() * 100.0f + 0.5f);
    uint32_t freqs[wgs::SCAN_MAX_POINTS];
    _scan.count = wgs::scanGrid(_scan.homeX100, stepX100, freqs,
                                wgs::SCAN_MAX_POINTS);
    for (uint8_t i = 0; i < _scan.count; i++) {
        _scan.pts[i] = wgs::ScanPoint{};
        _scan.pts[i].freqX100 = freqs[i];
    }
    _scan.stepX100 = stepX100;
    _scan.dwellS   = dwellS;
    _scan.idx      = 0;
    _scan.done     = false;
    _scan.lastPktSeq   = r.packetSeq();
    _scan.pointStartMs = millis();
    _scan.active = true;
    r.setFreqMHz(_scan.pts[0].freqX100 / 100.0f);
    log(LogLevel::Info,
        "scan: старт, %u точек x %u с, дом %.2f МГц (сторож тишины на паузе)",
        (unsigned)_scan.count, (unsigned)dwellS,
        (double)(_scan.homeX100 / 100.0f));
    return true;
}

void WeatherGateApp::scanAbort() {
    if (!_scan.active) return;
    Cc1101Driver& r = Cc1101Driver::getInstance();
    _scan.active = false;
    _scan.done   = false;
    r.setFreqMHz(_scan.homeX100 / 100.0f);
    log(LogLevel::Info, "scan: отменён оператором, возврат на %.2f МГц",
        (double)r.freqMHz());   // readback из драйвера
}

bool WeatherGateApp::scanTune(float mhz) {
    if (mhz < 914.0f || mhz > 916.0f) return false;   // границы схемы
    Cc1101Driver& r = Cc1101Driver::getInstance();
    if (!r.isHealthy()) return false;
    if (!r.setFreqMHz(mhz)) return false;
    log(LogLevel::Info, "scan: ручная перестройка -> %.2f МГц (readback)",
        (double)r.freqMHz());
    return true;
}

void WeatherGateApp::scanTick() {
    if (!_scan.active) return;
    Cc1101Driver& r = Cc1101Driver::getInstance();
    if (!r.isHealthy()) {           // чип потерян посреди прогона — домой
        _scan.active = false;
        log(LogLevel::Warning, "scan: CC1101 потерян, прогон прерван");
        return;
    }
    wgs::ScanPoint& p = _scan.pts[_scan.idx];

    wgs::scanPointOnNoise(p, r.readRssiNow());          // 1 выборка/с

    uint32_t seq = r.packetSeq();
    if (seq != _scan.lastPktSeq) {
        uint32_t d = seq - _scan.lastPktSeq;            // уникальные пакеты
        _scan.lastPktSeq = seq;
        wgs::scanPointOnPacket(p, (uint16_t)d, r.rssiDbm());
    }

    if (millis() - _scan.pointStartMs < (uint32_t)_scan.dwellS * 1000UL)
        return;

    // Точка закрыта: следующая или финиш
    _scan.idx++;
    if (_scan.idx >= _scan.count) {
        _scan.active = false;
        _scan.done   = true;
        r.setFreqMHz(_scan.homeX100 / 100.0f);
        int8_t rec = wgs::scanRecommend(_scan.pts, _scan.count,
                                        _scan.homeX100);
        if (rec >= 0)
            log(LogLevel::Info,
                "scan: финиш, возврат на %.2f МГц; рекомендация %.2f МГц "
                "(rssi_max %d дБм, pkt %u)",
                (double)r.freqMHz(),
                (double)(_scan.pts[rec].freqX100 / 100.0f),
                (int)_scan.pts[rec].rssiMax, (unsigned)_scan.pts[rec].pkt);
        else
            log(LogLevel::Info,
                "scan: финиш, возврат на %.2f МГц; пакетов нет — "
                "рекомендации нет", (double)r.freqMHz());
        return;
    }
    _scan.lastPktSeq   = r.packetSeq();
    _scan.pointStartMs = millis();
    r.setFreqMHz(_scan.pts[_scan.idx].freqX100 / 100.0f);
}

size_t WeatherGateApp::scanStatusJson(char* buf, size_t bufSize) const {
    const Cc1101Driver& r = Cc1101Driver::getInstance();
    const char* state = _scan.active ? "run" : (_scan.done ? "done" : "idle");
    int n = snprintf(buf, bufSize,
             "{\"state\":\"%s\",\"idx\":%u,\"count\":%u,"
             "\"home\":%.2f,\"freq\":%.2f,\"dwell_s\":%u,\"step\":%.2f,",
             state, (unsigned)_scan.idx, (unsigned)_scan.count,
             (double)(_scan.homeX100 / 100.0f), (double)r.freqMHz(),
             (unsigned)_scan.dwellS, (double)(_scan.stepX100 / 100.0f));
    if (n <= 0) return 0;
    if (_scan.active) {
        uint32_t per = (uint32_t)_scan.dwellS * 1000UL;
        uint32_t left = per - (millis() - _scan.pointStartMs);
        uint32_t eta = (left + (_scan.count - _scan.idx - 1) * per) / 1000UL;
        n += snprintf(buf + n, bufSize - (size_t)n, "\"eta_s\":%lu,",
                      (unsigned long)eta);
    }
    int8_t rec = (_scan.done || _scan.active)
               ? wgs::scanRecommend(_scan.pts, _scan.count, _scan.homeX100)
               : -1;
    n += snprintf(buf + n, bufSize - (size_t)n, "\"rec\":%d,\"points\":[",
                  (int)rec);
    uint8_t upto = _scan.active ? _scan.idx + 1
                                : (_scan.done ? _scan.count : 0);
    for (uint8_t i = 0; i < upto && n > 0; i++) {
        const wgs::ScanPoint& p = _scan.pts[i];
        n += snprintf(buf + n, bufSize - (size_t)n,
                      "%s{\"f\":%.2f,\"pkt\":%u,\"rmax\":%d,"
                      "\"ravg\":%d,\"noise\":%d}",
                      i ? "," : "",
                      (double)(p.freqX100 / 100.0f), (unsigned)p.pkt,
                      p.pkt ? (int)p.rssiMax : 0,
                      (int)p.rssiAvg(), (int)p.noiseAvg());
    }
    if (n > 0)
        n += snprintf(buf + n, bufSize - (size_t)n, "]}");
    return (n > 0) ? (size_t)n : 0;
}
