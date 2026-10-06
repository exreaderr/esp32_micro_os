// ============================================================================
// HmArchiveModule.cpp — архивариус W7 (накопление на SD + выдача по MQTT)
// ============================================================================
#include "HmArchiveModule.h"
#include "SdService.h"
#include <services/ConfigService.h>
#include <services/TimeService.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// --- КАНОН ФАЙЛА -------------------------------------------------------------
// /archive/<gw_id>/<канал>-<ГГГГ>.w7a: заголовок 16 Б, дальше DlogAggr 16 Б.
// Самоописывающий заголовок (требование ядерной ветки): магия + версия +
// имя канала — читается «руками с SD-карты через год».
#pragma pack(push, 1)
struct W7aHeader {
    uint32_t magic;        // W7A_FILE_MAGIC
    uint8_t  ver;          // 1
    char     ch[8];        // "wx_ot" ...
    uint8_t  pad[3];
};
#pragma pack(pop)
static constexpr uint32_t W7A_FILE_MAGIC = 0x31413757UL;   // 'W7A1' (LE)
static_assert(sizeof(W7aHeader) == 16, "W7aHeader 16B");
static_assert(sizeof(DlogAggr) == 16, "DlogAggr 16B");

static constexpr uint32_t HOUR_SEC        = 3600;
static constexpr uint32_t SUBDAY_SEC      = 14400;   // кадр = 4 часа (4 записи)
static constexpr uint8_t  PARTS_PER_DAY   = 6;       // субкадров в сутках (24/4)
static constexpr uint8_t  FRAME_RECS      = 4;       // записей в кадре (макс)
// Почему 4, а не 6 и не 12: входящий путь MQTT (subscribeExternal,
// MqttTransport ExtSub.payload) капает тело на MQTT_BODY_LEN=256. Худший
// кадр: 6 записей x [10-зн. ts + 3x7-симв. значения] + обвязка с 10-зн. CRC
// = 290 Б > 255 — молчаливое усечение, CRC не сойдётся никогда (нашла
// профильная ветка weather_gate, доп. к контракту 20.09.2026). 4 записи:
// худший 216 Б, типичный ~150-200 Б — под капом 240 с запасом.
static constexpr uint32_t SESSION_GUARD_MS  = 240000;  // сторож сессии выдачи (0.8.9: сессия ~105 с + переспросы)
static constexpr uint32_t REQ_ANTIBOUNCE_MS = 120000;  // повторный полный req
// 0.8.8: пейсинг выдачи по wall-clock. Замер ветки weather_gate (02.10):
// фактический период serveTick ~20 мс, не 150 — выдача уходила ~50 кадров/с
// и затиралась однослотовым mailbox шлюза. 0.8.9: пауза — из конфига
// (arch.pace_ms, дефолт 500: дренаж шлюза измерен ~2 кадра/с), это лишь
// запасной дефолт если поля нет в NVS.
static constexpr uint32_t SERVE_PACE_MS     = 500;

HmArchiveModule& HmArchiveModule::getInstance() {
    static HmArchiveModule inst;
    return inst;
}

const char* HmArchiveModule::chName(uint8_t i) {
    static const char* names[CH_COUNT] = {"wx_ot", "wx_oh", "wx_p", "wx_w", "wx_r"};
    return (i < CH_COUNT) ? names[i] : "wx_??";
}
const char* HmArchiveModule::chJsonKey(uint8_t i) {
    // Ключи weather-JSON шлюза (контракт 0.9.0). Якорь по имени ключа
    // целиком (урок №25): \"temp\" не встречается в единицах измерения.
    static const char* keys[CH_COUNT] = {"\"temp\":", "\"humidity\":",
                                         "\"press\":", "\"wind\":", "\"rain\":"};
    return (i < CH_COUNT) ? keys[i] : "\"?\":";
}

// --- ЖИЗНЕННЫЙ ЦИКЛ ----------------------------------------------------------
void HmArchiveModule::init() {
    _enabled = cfgGetBool("arch.enabled", true);
    cfgGetStr("arch.src_topic", _srcTopic, sizeof(_srcTopic),
              "microos/weather_gate/weather");
    cfgGetStr("arch.gw_id", _gwId, sizeof(_gwId), "weather_gate");
    // 0.9.0: строка чтения pace_ms была съедена гремлином при сборке 0.8.9
    // (дефолт 500 совпал с полем — потому и работало; поймано ревизией W8).
    _paceMs = cfgGetUInt("arch.pace_ms", SERVE_PACE_MS);
    _bfEnabled = cfgGetBool("arch.bf_enabled", true);      // 0.9.0 (W8)
    _bfHour = (uint8_t)cfgGetUInt("arch.bf_hour", 3);
    _initialized = true;
}

void HmArchiveModule::start() {
    _started = true;
    if (!_enabled) {
        log(LogLevel::Info, "архив W7 выключен (arch.enabled=false)");
        return;
    }
    BrokerService::getInstance().addEventHook(&HmArchiveModule::onBrokerEvent);
    log(LogLevel::Info, "архив W7: источник %s, архив /archive/%s/",
        _srcTopic, _gwId);
}

void HmArchiveModule::stop() {
    BrokerService::getInstance().removeEventHook(&HmArchiveModule::onBrokerEvent);
    if (_scanFile) { _scanFile.close(); }
    _serving = false;
    _started = false;
}

// --- ХУК БРОКЕРА (контекст tick брокера — ТОЛЬКО RAM, без SD!) ----------------
void HmArchiveModule::onBrokerEvent(const BrokerEventInfo& info) {
    HmArchiveModule& self = getInstance();
    if (!self._enabled || !self._started) return;
    if (info.type != BrokerEventInfo::Publish || info.truncated) return;

    if (strcmp(info.topic, self._srcTopic) == 0) {
        // Дедуп retained-повторов (реконнекты): тело в точности как прошлое.
        if (strncmp(info.payload, self._lastPayload,
                    sizeof(self._lastPayload)) == 0) {
            self._recSkipped++;
            return;
        }
        self.onWeather(info.payload);
        return;
    }
    // req-топик: собран в start() из mqtt.prefix — сравниваем суффикс,
    // чтобы не тащить prefix в хук.
    if (strstr(info.topic, "/master/archive/req") != nullptr) {
        self.onRequest(info.payload);
        return;
    }
    // W8: ответы бэкфилла (кадры формата W7) — разбор в хуке, только RAM.
    if (strstr(info.topic, "/master/archive/bfresp/") != nullptr) {
        self.onBfResp(info.payload);
        return;
    }
    // W8 триггер: retained state шлюза (<prefix>/<gw_id>/state).
    // online после offline > 75 мин → внеплановый скан дыр.
    {
        char st[48];
        snprintf(st, sizeof(st), "/%s/state", self._gwId);
        if (strstr(info.topic, st) != nullptr) {
            bool on = (strncmp(info.payload, "online", 6) == 0);
            if (!on) {
                if (self._gwDownMs == 0) self._gwDownMs = millis();
            } else {
                if (self._gwDownMs != 0 &&
                    millis() - self._gwDownMs > 75UL * 60UL * 1000UL) {
                    self._bfScanPend = true;
                    self.log(LogLevel::Info, "архив W8: шлюз online после offline %lu мин — скан дыр",
                        (unsigned long)((millis() - self._gwDownMs) / 60000));
                }
                self._gwDownMs = 0;
            }
        }
    }
}

void HmArchiveModule::onWeather(const char* payload) {
    uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
    if (ts < 1700000000UL) return;   // время ещё не синхронизировано

    // 0.9.2: W8-триггер «данные возобновились после перерыва > 75 мин».
    // Полевой тест 05.10: обрыв линка мастера НЕ фиксируется брокером
    // (нет keepalive, сокет повисает) — retained state шлюза остаётся
    // «online», state-триггер слепнет в главном аварийном сценарии.
    // Возобновление живого потока — транспортно-независимый признак
    // перерыва: сами данные и есть доказательство дыры.
    {
        uint32_t nowMs = millis();
        if (_bfEnabled && _lastWxMs != 0 &&
            nowMs - _lastWxMs > 75UL * 60UL * 1000UL) {
            _bfScanPend = true;
            log(LogLevel::Info, "архив W8: поток возобновился после %lu мин — скан дыр",
                (unsigned long)((nowMs - _lastWxMs) / 60000UL));
        }
        _lastWxMs = nowMs;
    }

    bool any = false;
    for (uint8_t ch = 0; ch < CH_COUNT; ++ch) {
        const char* p = strstr(payload, chJsonKey(ch));
        if (p == nullptr) continue;
        p += strlen(chJsonKey(ch));
        if (strncmp(p, "null", 4) == 0) continue;   // press: null и т.п.
        float v = (float)atof(p);
        DlogAggr rolled;
        if (_bkt[ch].add(ts, v, HOUR_SEC, rolled)) {
            if (_rollPend[ch]) _recSkipped++;   // tick не успел слить — не бывает
            _rolled[ch] = rolled;
            _rollPend[ch] = true;
        }
        any = true;
    }
    if (any) {
        if (!_firstWxLogged) {
            _firstWxLogged = true;
            log(LogLevel::Info, "архив W7: первый weather-кадр принят, накопление пошло");
        }
        safeStrCopy(_lastPayload, sizeof(_lastPayload), payload);
    }
}

// --- TICK: слив ведер на SD + темпованная выдача -------------------------------
void HmArchiveModule::tick() {
    if (!_enabled || !_started) return;
    // Один append за тик (короткий сеанс SD, урок питания 15.09)
    for (uint8_t ch = 0; ch < CH_COUNT; ++ch) {
        if (_rollPend[ch]) { flushChannel(ch); break; }
    }
    serveTick();
    bfTick();          // W8: планировщик бэкфилла (скан/сессия/мердж)
}

void HmArchiveModule::flushChannel(uint8_t ch) {
    _rollPend[ch] = false;
    if (appendRecord(ch, _rolled[ch])) {
        _recWritten++;
    } else {
        _recSkipped++;
    }
}

void HmArchiveModule::filePath(uint8_t ch, int year, char* out, size_t n) const {
    snprintf(out, n, "/archive/%s/%s-%04d.w7a", _gwId, chName(ch), year);
}

bool HmArchiveModule::writeHeaderIfNew(fs::File& f, const char* ch) {
    if (f.size() != 0) return true;
    W7aHeader h{};
    h.magic = W7A_FILE_MAGIC;
    h.ver = 1;
    strncpy(h.ch, ch, sizeof(h.ch) - 1);
    return f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h);
}

uint32_t HmArchiveModule::readLastTs(uint8_t ch) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) return 0;
    // Год от текущего времени; хвост ищем в текущем, потом в прошлом.
    time_t nowT = TimeService::getInstance().getUnixTime();
    struct tm* tmv = gmtime(&nowT);
    int year = tmv ? tmv->tm_year + 1900 : 1970;
    char path[64];
    for (int pass = 0; pass < 2; ++pass) {
        filePath(ch, year - pass, path, sizeof(path));
        if (!sd->exists(path)) continue;
        fs::File f = sd->open(path, FILE_READ);
        if (!f) continue;
        size_t sz = f.size();
        uint32_t ts = 0;
        if (sz >= sizeof(W7aHeader) + sizeof(DlogAggr)) {
            f.seek(sz - sizeof(DlogAggr));
            DlogAggr r;
            if (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) ts = r.ts;
        }
        f.close();
        if (ts != 0) return ts;
    }
    return 0;
}

bool HmArchiveModule::appendRecord(uint8_t ch, const DlogAggr& r) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) return false;

    // Идемпотентность: ts не новее последнего записанного — пропуск
    // (retained-повторы, реконнекты, двойные сливы после ребута).
    if (!_lastTsKnown[ch]) {
        _lastTs[ch] = readLastTs(ch);
        _lastTsKnown[ch] = true;
    }
    if (r.ts <= _lastTs[ch]) return false;

    // Каталоги — один раз за сессию (mkdir существующего безвреден).
    static bool dirsTried = false;
    if (!dirsTried) {
        dirsTried = true;
        sd->mkdir("/archive");
        char dir[48];
        snprintf(dir, sizeof(dir), "/archive/%s", _gwId);
        sd->mkdir(dir);
    }

    time_t nowT = TimeService::getInstance().getUnixTime();
    struct tm* tmv = gmtime(&nowT);
    int year = tmv ? tmv->tm_year + 1900 : 1970;
    char path[64];
    filePath(ch, year, path, sizeof(path));

    fs::File f = sd->open(path, FILE_APPEND);   // создаёт при отсутствии
    if (!f) return false;
    bool ok = writeHeaderIfNew(f, chName(ch)) &&
              f.write((const uint8_t*)&r, sizeof(r)) == sizeof(r);
    f.close();
    if (ok) _lastTs[ch] = r.ts;
    return ok;
}

// ============================================================================
// ВЫДАЧА: MQTT запрос-ответ (контракт W7). Кадр = канал-4-ЧАСА (4 записи)
// — вторая поправка к ноте «канал-день (24)»: сначала полдня (12) по бюджету
// 700 Б, затем 4 часа по входящему капу MQTT_BODY_LEN=256 (доп. ветки
// weather_gate 20.09.2026): 6 записей в худшем случае = 290 Б > 255.
// seq = ch*(days*6) + day*6 + part; total = CH_COUNT × days × 6.
// Детерминизм seq → переспрос регенерирует кадр из архива заново.
// ============================================================================

// CRC32/zlib (poly 0xEDB88320, init/xorout 0xFFFFFFFF) — контрактный вариант.
// Побитовый, без таблицы: кадры редки, 600 Б × 8 итераций несущественно.
uint32_t HmArchiveModule::crc32zlib(const uint8_t* d, size_t n) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (uint8_t b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFUL;
}

// Компактный float: "%.2f" с обрезкой хвостовых нулей (21.50 → 21.5).
static void w7FmtNum(float v, char* out, size_t n) {
    snprintf(out, n, "%.2f", (double)v);
    char* dot = strchr(out, '.');
    if (dot == nullptr) return;
    char* e = out + strlen(out) - 1;
    while (e > dot && *e == '0') *e-- = '\0';
    if (e == dot) *e = '\0';
}

static bool w7ParseStr(const char* json, const char* key, char* out, size_t n) {
    const char* p = strstr(json, key);          // key вида "\"id\":\""
    if (p == nullptr) return false;
    p += strlen(key);
    const char* q = strchr(p, '"');
    if (q == nullptr || (size_t)(q - p) >= n) return false;
    memcpy(out, p, (size_t)(q - p));
    out[q - p] = '\0';
    return true;
}

static bool w7ParseUint(const char* json, const char* key, uint32_t& out) {
    const char* p = strstr(json, key);          // key вида "\"days\":"
    if (p == nullptr) return false;
    out = (uint32_t)strtoul(p + strlen(key), nullptr, 10);
    return true;
}

void HmArchiveModule::onRequest(const char* payload) {
    char id[33] = "";
    if (!w7ParseStr(payload, "\"id\":\"", id, sizeof(id))) return;
    if (strcmp(id, _gwId) != 0) {
        // Диагностика конфигурации: чужой id — почти всегда это значит,
        // что arch.gw_id не совпадает с реальным id шлюза (урок 27.09:
        // дефолт "weather_gate" vs фактический "d4e9f4bd612b" — молчали оба).
        log(LogLevel::Warning, "архив W7: req с чужим id '%s' (жду '%s') — игнор",
            id, _gwId);
        return;
    }

    uint32_t v = 0;
    if (w7ParseUint(payload, "\"seq\":", v)) {
        // Переспрос кадра (CRC не сошёлся): регенерация по seq.
        // 0.8.6: если идёт полная сессия — прячем её _seq и ПОЛНОСТЬЮ
        // сбрасываем состояние скана (файл, канал, буфер), иначе переспрос
        // ломал бегущую выдачу (дефект A полевого теста 01.10).
        if (_serving && !_resendOnly) {
            _resumeSeq = _seq;
            _resumePend = 1;
        }
        if (_scanFile) _scanFile.close();
        _scanCh = 0xFF;
        _scanYearIdx = 0;
        _rbuf[0] = '\0';
        _rN = 0;
        _resendOnly = 1;
        _resendSeq = v;
        _seq = v;
        _total = (uint32_t)CH_COUNT * _days * PARTS_PER_DAY;
        _serving = true;
        _reqStartedMs = millis();
        _lastPubMs = 0;                       // первый кадр — сразу
        return;
    }
    if (w7ParseUint(payload, "\"days\":", v)) {
        // Антидребезг: повторный полный req от того же id в 120 с — мимо
        // (reconnect-шторм на рваном линке не должен перезапускать выдачу).
        uint32_t now = millis();
        if (strcmp(id, _lastFullReqId) == 0 &&
            now - _lastFullReqMs < REQ_ANTIBOUNCE_MS) {
            log(LogLevel::Info, "архив W7: повторный req от %s за %lu с — антидребезг, мимо",
                id, (unsigned long)((now - _lastFullReqMs) / 1000));
            return;
        }
        safeStrCopy(_lastFullReqId, sizeof(_lastFullReqId), id);
        _lastFullReqMs = now;

        _days = (uint8_t)constrain((int)v, 1, 31);
        _total = (uint32_t)CH_COUNT * _days * PARTS_PER_DAY;
        _seq = 0;
        _resendOnly = 0;
        _resumePend = 0;                        // новая полная сессия — старая забыта
        _serving = true;
        _reqStartedMs = now;
        _lastPubMs = 0;                       // первый кадр — сразу
        _scanYearIdx = 0;
        _scanCh = 0xFF;
        _rbuf[0] = '\0';
        _rN = 0;
        log(LogLevel::Info, "архив W7: req от %s, days %u → %lu кадров",
            id, _days, (unsigned long)_total);
    }
}

void HmArchiveModule::sendEmpty() {
    char topic[MQTT_TOPIC_LEN];
    char prefix[24];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    snprintf(topic, sizeof(topic), "%s/master/archive/resp/%s", prefix, _gwId);
    BrokerService::getInstance().publishLocal(topic, "{\"seq\":0,\"total\":0}",
                                              false);
    _framesSent++;
}

void HmArchiveModule::sendFrame(const char* ch, int day, const char* rbuf,
                                uint32_t seq, uint32_t total) {
    char topic[MQTT_TOPIC_LEN];
    char prefix[24];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    snprintf(topic, sizeof(topic), "%s/master/archive/resp/%s", prefix, _gwId);
    uint32_t crc = crc32zlib((const uint8_t*)rbuf, strlen(rbuf));
    int n = snprintf(_frame, sizeof(_frame),
                     "{\"seq\":%lu,\"total\":%lu,\"ch\":\"%s\",\"day\":%d,"
                     "\"crc\":%lu,\"r\":%s}",
                     (unsigned long)seq, (unsigned long)total, ch, day,
                     (unsigned long)crc, rbuf);
    if (n <= 0 || n >= (int)sizeof(_frame)) return;   // не влез — пропуск
    if (BrokerService::getInstance().publishLocal(topic, _frame, false)) {
        _framesSent++;
    }
}

// --- API вкладки «Архив» (0.8.4): статус + файлы. Вызывается из HTTP-    ---
// --- контекста веб-сервера (НЕ из хука брокера) — SD трогать можно.      ---
size_t HmArchiveModule::apiStatus(char* buf, size_t size) {
    fs::FS* sd = SdService::getInstance().fs();
    int n = snprintf(buf, size,
        "{\"enabled\":%d,\"started\":%d,\"src_topic\":\"%s\",\"gw_id\":\"%s\","
        "\"rec_written\":%lu,\"rec_skipped\":%lu,\"req_served\":%lu,"
        "\"frames_sent\":%lu,\"serving\":%d,\"seq\":%lu,\"total\":%lu,"
        "\"sd\":\"%s\",\"bf\":{\"enabled\":%d,\"scanning\":%d,\"active\":%d,"
        "\"queue\":%u,\"holes_found\":%lu,\"holes_closed\":%lu,"
        "\"sessions\":%lu,\"unserv\":%lu,\"rec_merged\":%lu},\"files\":[",
        _enabled ? 1 : 0, _started ? 1 : 0, _srcTopic, _gwId,
        (unsigned long)_recWritten, (unsigned long)_recSkipped,
        (unsigned long)_reqServed, (unsigned long)_framesSent,
        _serving ? 1 : 0, (unsigned long)_seq, (unsigned long)_total,
        (sd != nullptr) ? "mounted" : "нет",
        _bfEnabled ? 1 : 0, _bfScanning ? 1 : 0, _bfActive ? 1 : 0,
        _bfQLen, (unsigned long)_bfHolesFound, (unsigned long)_bfHolesClosed,
        (unsigned long)_bfSessions, (unsigned long)_bfUnserv,
        (unsigned long)_bfRecMerged);
    if (n <= 0 || (size_t)n >= size) { buf[0] = '\0'; return 0; }
    size_t used = (size_t)n;
    if (sd != nullptr) {
        char dir[48];
        snprintf(dir, sizeof(dir), "/archive/%s", _gwId);
        fs::File d = sd->open(dir);
        if (d) {
            bool first = true;
            fs::File f;
            while (used + 96 < size && (f = d.openNextFile())) {
                if (!f.isDirectory()) {
                    // File.name() на SD отдаёт полный путь — берём хвост.
                    const char* nm = f.name();
                    const char* base = strrchr(nm, '/');
                    base = (base != nullptr) ? base + 1 : nm;
                    unsigned long sz = (unsigned long)f.size();
                    // 16 Б заголовок W7A1 + записи DlogAggr по 16 Б.
                    unsigned long recs = (sz >= 16) ? (sz - 16) / 16 : 0;
                    n = snprintf(buf + used, size - used,
                                 "%s{\"name\":\"%s\",\"size\":%lu,\"records\":%lu}",
                                 first ? "" : ",", base, sz, recs);
                    if (n > 0) { used += (size_t)n; first = false; }
                }
                f.close();
            }
            d.close();
        }
    }
    if (used + 3 <= size) { memcpy(buf + used, "]}", 2); used += 2; buf[used] = '\0'; }
    return used;
}

void HmArchiveModule::abortSession(const char* why) {
    if (_scanFile) _scanFile.close();
    _serving = false;
    _rbuf[0] = '\0';
    _rN = 0;
    log(LogLevel::Warning, "архив W7: сессия выдачи прервана (%s)", why);
}

// Сессия выдачи: ровно один кадр на интервал arch.pace_ms (дефолт 500 мс,
// 0.8.9) — 210 кадров ≈ 105 с. Пейсинг по wall-clock, НЕ по тикам: замер
// 02.10 показал, что реальный период serveTick ~20 мс, а не 150, как было
// принято в комментарии 0.8.7; а замер дренажа шлюза (~2 кадра/с — loop
// занят dlog-записью в LittleFS и стартовыми задачами) показал, что и
// 200 мс мало (приём 86/210). Таймаут сборки на шлюзе поднят веткой
// до 150 с (0.9.6).
void HmArchiveModule::serveTick() {
    if (!_serving) return;
    uint32_t now = millis();
    if (now - _reqStartedMs > SESSION_GUARD_MS) {
        abortSession("сторож 240 с");
        return;
    }
    // Пейсинг по часам: тик службы ~20 мс, а между публикациями должно
    // пройти ≥arch.pace_ms (дефолт 500) — иначе однослотовый mailbox
    // шлюза затирает кадры (дренаж шлюза измерен ~2 кадра/с, 02.10).
    if (now - _lastPubMs < _paceMs) return;
    bool done = false;
    for (uint8_t iter = 0; iter < 32 && !done; ++iter) {
        done = buildFrame();        // шаги скана внутри тика, публикация — одна
    }
    if (!done) return;              // кадр ещё собирается — следующий тик
    _lastPubMs = now;
    // Кадр отправлен (или пуст). Конец сессии?
    if (_resendOnly) {
        _resendOnly = 0;
        if (_resumePend) {
            // Возвращаемся в прерванную полную сессию: скан уже сброшен
            // в onRequest, откроется заново с начала файла канала.
            _resumePend = 0;
            _seq = _resumeSeq;
            _total = (uint32_t)CH_COUNT * _days * PARTS_PER_DAY;
            _reqStartedMs = millis();
            log(LogLevel::Info, "архив W7: переспрос %lu отдан, возврат в сессию (seq %lu)",
                (unsigned long)_resendSeq, (unsigned long)_seq);
        } else {
            _serving = false;
        }
        return;
    }
    if (_seq >= _total) {
        _serving = false;
        _reqServed++;
        log(LogLevel::Info, "архив W7: выдача завершена, %lu кадров",
            (unsigned long)_total);
    }
}

// Строит и шлёт кадр для _seq. true — кадр завершён (отправлен), false —
// нужен ещё тик (читаем файл кусками).
bool HmArchiveModule::buildFrame() {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { abortSession("нет SD"); return true; }

    uint32_t framesPerCh = (uint32_t)_days * PARTS_PER_DAY;
    uint8_t  ch   = (uint8_t)(_seq / framesPerCh);
    uint32_t rem  = _seq % framesPerCh;
    int      day  = (int)(rem / PARTS_PER_DAY);
    int      part = (int)(rem % PARTS_PER_DAY);

    // Пустой архив целиком: на первом кадре ни одного файла — total=0.
    if (_seq == 0 && _scanYearIdx == 0 && !_scanFile) {
        char dir[48];
        snprintf(dir, sizeof(dir), "/archive/%s", _gwId);
        if (!sd->exists(dir)) {
            sendEmpty();
            _serving = false;
            _reqServed++;
            return true;
        }
    }

    // Окно кадра: day=0 — самый старый день; part 0..5 — 4-часовой слот UTC.
    uint32_t nowU = (uint32_t)TimeService::getInstance().getUnixTime();
    uint32_t todayStart = nowU - (nowU % 86400UL);
    uint32_t w0 = todayStart - (uint32_t)(_days - 1 - day) * 86400UL
                + (uint32_t)part * SUBDAY_SEC;
    uint32_t w1 = w0 + SUBDAY_SEC;

    // Открыть следующий файл канала (прошлый год, затем текущий).
    if (!_scanFile) {
        if (_scanCh != ch) { _scanCh = ch; _scanYearIdx = 0; }
        time_t nowT = (time_t)nowU;
        struct tm* tmv = gmtime(&nowT);
        int year = tmv ? tmv->tm_year + 1900 : 1970;
        bool opened = false;
        while (_scanYearIdx < 2 && !opened) {
            char path[64];
            filePath(ch, year - 1 + (int)_scanYearIdx, path, sizeof(path));
            _scanYearIdx++;
            if (!sd->exists(path)) continue;
            _scanFile = sd->open(path, FILE_READ);
            if (_scanFile) {
                _scanFile.seek(sizeof(W7aHeader));   // заголовок — не данные
                opened = true;
            }
        }
        if (!opened) {   // файлов канала нет — кадр пустой ("r":[])
            strcpy(_rbuf, "[]");
            sendFrame(chName(ch), day, _rbuf, _seq, _total);
            _seq++;
            _scanCh = 0xFF;   // следующий кадр — новый канал, переоткрытие
            _rbuf[0] = '\0'; _rN = 0;
            return true;
        }
    }

    // Кусок файла: 512 Б = 32 целых записи (заголовок 16 Б кратен записи,
    // чтение всегда выровнено — неполных записей в окне не бывает).
    int n = _scanFile.read((uint8_t*)_win, sizeof(_win));
    if (n >= (int)sizeof(DlogAggr)) {
        uint16_t recs = (uint16_t)(n / sizeof(DlogAggr));
        bool past = false;
        for (uint16_t i = 0; i < recs && _rN < FRAME_RECS; ++i) {
            const DlogAggr& r = _win[i];
            if (r.ts >= w1) { past = true; break; }   // записи по возрастанию ts
            if (r.ts < w0) continue;
            char num[12];
            char rec[48];
            // [ts,mn,mx,avg] — компактные float'ы (хвостовые нули долой)
            int m = snprintf(rec, sizeof(rec), "%s[%lu,", (_rN ? "," : ""), (unsigned long)r.ts);
            (void)m;
            size_t used = strlen(rec);
            w7FmtNum(r.mn, num, sizeof(num));
            used += snprintf(rec + used, sizeof(rec) - used, "%s,", num);
            w7FmtNum(r.mx, num, sizeof(num));
            used += snprintf(rec + used, sizeof(rec) - used, "%s,", num);
            w7FmtNum(r.avg, num, sizeof(num));
            used += snprintf(rec + used, sizeof(rec) - used, "%s]", num);
            if (strlen(_rbuf) + used + 2 < sizeof(_rbuf)) {
                strcat(_rbuf, rec);
                _rN++;
            }
        }
        if (!past) return false;   // читаем дальше следующим тиком
        // Окно пройдено — файл для этого кадра больше не нужен.
        _scanFile.close();
        _scanYearIdx = 2;         // сразу к финалу кадра
    } else {
        // Файл кончился: закрыть; следующий тик откроет другой год.
        _scanFile.close();
    }
    if (_scanYearIdx < 2) return false;   // следующий тик откроет другой год

    // Финал кадра: собрать "[...]" и отправить.
    char body[sizeof(_rbuf)];
    if (_rN == 0) {
        strcpy(body, "[]");
    } else {
        snprintf(body, sizeof(body), "[%s]", _rbuf);
    }
    sendFrame(chName(ch), day, body, _seq, _total);
    _seq++;
    // Следующий кадр: если канал сменился — переоткрытие (помечаем 0xFF),
    // иначе продолжаем сканировать тот же канал с начала файлов? Нет —
    // файл уже дочитан; но кадры идут по возрастанию окон, а записи в файле
    // по возрастанию ts: дочитанный файл для следующего кадра ЭТОГО канала
    // не нужен только если окна не пересекаются с остатком файла. Честно и
    // просто: переоткрываем файлы канала на каждый кадр (дешево: SD-чтение
    // ~140 КБ × 70 кадров раз в перепрошивку; зато без хрупких оптимизаций).
    _scanCh = 0xFF;
    _scanYearIdx = 0;
    _rbuf[0] = '\0';
    _rN = 0;
    return true;
}

// ============================================================================
// W8 BACKFILL (0.9.0) — дозаливка дыр w7a с почасового яруса шлюза.
// Контракт утверждён 04.10.2026 (ответ ветке weather_gate):
//   bfreq : <prefix>/master/archive/bfreq  {"id":"<gw>","from":F,"to":T}
//           окно = ОДНИ сутки UTC (F = 00:00 UTC, T = F + 86400);
//   bfresp: <prefix>/master/archive/bfresp/<gw> — кадры формата W7,
//           days=1 → всего 30 (5 каналов × 6 слотов по 4 ч);
//   пустые кадры r:[] легальны; пустой ответ целиком {"seq":0,"total":0};
//   мердж только ОТСУТСТВУЮЩИХ ts; взаимное исключение с W7 (_serving).
// Приём на мастере — in-proc хук брокера (mailbox ядра не задействован);
// хук — ТОЛЬКО RAM (разбор + буфер), SD — из тика.
// ============================================================================
static constexpr uint8_t  BF_FRAMES_PER_DAY = 30;   // 5 каналов × 6 слотов по 4 ч
static constexpr uint32_t BF_GUARD_MS       = 240000;  // как сторож W7
// 0.9.3: пауза между сессиями очереди 130 с — ДОЛЖНА перекрывать шлюзовой
// антидребезг bfreq (120 с по контракту). Полевой тест №2 (06.10): пауза
// 5 с → второй bfreq суток 29.09 проигнорирован шлюзом («в окне
// антидребезга, игнор»), мастер по сторожу 240 с пометил сутки мёртвыми.
// 7 суток очереди × (30 с выдача + 130 с пауза) ≈ 19 мин худший случай.
static constexpr uint32_t BF_SESS_GAP_MS    = 130000;  // > 120 с антидребезга шлюза

int HmArchiveModule::chIndexByName(const char* name) const {
    for (uint8_t i = 0; i < CH_COUNT; ++i)
        if (strcmp(name, chName(i)) == 0) return (int)i;
    return -1;
}

bool HmArchiveModule::bfIsDead(uint32_t dayStart) const {
    for (uint8_t i = 0; i < _bfDeadN; ++i)
        if (_bfDead[i] == dayStart) return true;
    return false;
}

bool HmArchiveModule::bfDeadMark(uint32_t dayStart) {
    if (bfIsDead(dayStart)) return false;
    if (_bfDeadN >= 8) {   // FIFO: вытесняем самое старое
        memmove(_bfDead, _bfDead + 1, 7 * sizeof(uint32_t));
        _bfDeadN = 7;
    }
    _bfDead[_bfDeadN++] = dayStart;
    return true;
}

// --- Планировщик (tick) ------------------------------------------------------
void HmArchiveModule::bfTick() {
    if (!_bfEnabled) return;
    uint32_t nowU = (uint32_t)TimeService::getInstance().getUnixTime();
    if (nowU < 1700000000UL) return;          // время не синхронизировано
    uint32_t now = millis();

    // Хук попросил финал сессии (все кадры / пустой ответ) — делаем в тике.
    if (_bfActive && _bfEndReq) {
        bfFinalize(_bfEmptySess ? "пустой ответ" : "окно принято");
        return;
    }
    // Сторож активной сессии.
    if (_bfActive && now - _bfReqMs > BF_GUARD_MS) {
        bfFinalize("сторож 240 с");
        return;
    }
    if (_bfActive || _serving) return;        // взаимное исключение W7/W8

    // Плановый скан: первый через 5 мин после старта, далее ежесуточно
    // в arch.bf_hour UTC.
    if (_bfNextScanUtc == 0) _bfNextScanUtc = nowU + 300;
    if (!_bfScanning && (_bfScanPend || nowU >= _bfNextScanUtc)) {
        _bfScanPend = false;
        _bfScanning = true;
        _bfScanCh = 0;
    }
    if (_bfScanning) { bfScanStep(); return; }

    // Старт сессии по очереди (пауза 5 с между окнами).
    if (_bfQLen > 0 && now - _bfSessGapMs > BF_SESS_GAP_MS) {
        bfStartSession();
    }
}

// --- Скан дыр: один канал за вызов (файл читается целиком — он мал) ---------
void HmArchiveModule::bfScanStep() {
    fs::FS* sd = SdService::getInstance().fs();
    uint32_t nowU = (uint32_t)TimeService::getInstance().getUnixTime();
    uint32_t todayStart = nowU - (nowU % 86400UL);

    // Смена суток UTC — очищаем список «мёртвых» (повтор раз в сутки).
    if (_bfDeadDay != todayStart) { _bfDeadDay = todayStart; _bfDeadN = 0; }

    // Часовая сетка: 168 часов, h=0 — самый старый; два последних часа
    // (текущее открытое ведро + только что закрывшееся) пропускаем —
    // запись могла ещё не слиться.
    static uint8_t present[168];
    memset(present, 0, sizeof(present));
    uint32_t hourStart = nowU - (nowU % 3600UL);
    uint32_t oldest = hourStart - 167 * 3600UL;

    if (sd != nullptr) {
        for (int pass = 0; pass < 2; ++pass) {   // прошлый год, текущий
            time_t t0 = (time_t)nowU;
            struct tm* tmv = gmtime(&t0);
            int year = tmv ? tmv->tm_year + 1900 : 1970;
            char path[64];
            filePath(_bfScanCh, year - 1 + pass, path, sizeof(path));
            if (!sd->exists(path)) continue;
            fs::File f = sd->open(path, FILE_READ);
            if (!f) continue;
            f.seek(sizeof(W7aHeader));
            DlogAggr r;
            while (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
                if (r.ts < oldest || r.ts >= hourStart) continue;
                if (r.ts % 3600UL != 0) continue;
                uint32_t h = (r.ts - oldest) / 3600UL;
                if (h < 168) present[h] = 1;
            }
            f.close();
        }
    }

    // Дыры канала → общая битовая карта суток (0=сегодня … 6=самый старый).
    static uint32_t holeDays[CH_COUNT];   // аккумулятор по каналам скана
    uint32_t holes = 0;
    for (uint16_t h = 0; h < 166; ++h) {
        if (present[h]) continue;
        uint32_t ds = ((oldest + h * 3600UL) / 86400UL) * 86400UL;
        if (ds > todayStart) continue;
        uint32_t d = (todayStart - ds) / 86400UL;
        if (d < 7) holes |= (1UL << d);
    }
    holeDays[_bfScanCh] = holes;
    _bfScanCh++;

    if (_bfScanCh < CH_COUNT) return;     // следующий канал — следующим тиком
    _bfScanning = false;

    // Сводка: очередь суток (без dead и без дублей очереди).
    uint8_t added = 0;
    for (uint8_t d = 0; d < 7; ++d) {
        uint32_t m = 0;
        for (uint8_t c = 0; c < CH_COUNT; ++c) m |= holeDays[c];
        if ((m & (1UL << d)) == 0) continue;
        uint32_t ds = todayStart - d * 86400UL;
        if (bfIsDead(ds)) continue;
        bool queued = false;
        for (uint8_t q = 0; q < _bfQLen; ++q)
            if (_bfQueue[q] == ds) { queued = true; break; }
        if (queued || (_bfActive && _bfDayStart == ds)) continue;
        if (_bfQLen < 8) { _bfQueue[_bfQLen++] = ds; _bfHolesFound++; added++; }
    }
    // Следующий плановый скан — ближайший arch.bf_hour UTC.
    uint32_t next = todayStart + (uint32_t)_bfHour * 3600UL;
    if (next <= nowU) next += 86400UL;
    _bfNextScanUtc = next;
    log(LogLevel::Info, "архив W8: скан дыр — %u суток-дырок в очереди (всего найдено %lu)",
        _bfQLen, (unsigned long)_bfHolesFound);
    (void)added;
}

// --- Старт сессии: bfreq на первые сутки очереди -----------------------------
void HmArchiveModule::bfStartSession() {
    uint32_t ds = _bfQueue[0];
    memmove(_bfQueue, _bfQueue + 1, 7 * sizeof(uint32_t));
    _bfQLen--;

    _bfDayStart = ds;
    _bfActive = true;
    _bfReqMs = millis();
    _bfRxFrames = 0;
    _bfEndReq = 0;
    _bfEmptySess = 0;
    for (uint8_t c = 0; c < CH_COUNT; ++c) _bfCnt[c] = 0;
    _bfSessions++;

    char topic[MQTT_TOPIC_LEN];
    char prefix[24];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    snprintf(topic, sizeof(topic), "%s/master/archive/bfreq", prefix);
    char pl[112];
    snprintf(pl, sizeof(pl), "{\"id\":\"%s\",\"from\":%lu,\"to\":%lu}",
             _gwId, (unsigned long)ds, (unsigned long)(ds + 86400UL));
    BrokerService::getInstance().publishLocal(topic, pl, false);

    time_t t0 = (time_t)ds;
    struct tm* tmv = gmtime(&t0);
    log(LogLevel::Info, "архив W8: bfreq за %02d.%02d.%04d — сессия %lu",
        tmv ? tmv->tm_mday : 0, tmv ? tmv->tm_mon + 1 : 0,
        tmv ? tmv->tm_year + 1900 : 0, (unsigned long)_bfSessions);
}

// --- Приём кадра (хук брокера — ТОЛЬКО RAM!) ---------------------------------
void HmArchiveModule::onBfResp(const char* payload) {
    if (!_bfActive || _bfEndReq) return;

    // Пустой ответ целиком: {"seq":0,"total":0} — у шлюза нет ничего.
    if (strstr(payload, "\"total\":0") != nullptr) {
        _bfEmptySess = 1;
        _bfEndReq = 1;
        return;
    }

    uint32_t seq = 0, crc = 0;
    if (!w7ParseUint(payload, "\"seq\":", seq)) return;
    if (!w7ParseUint(payload, "\"crc\":", crc)) return;
    char chn[8] = "";
    if (!w7ParseStr(payload, "\"ch\":\"", chn, sizeof(chn))) return;
    int ch = chIndexByName(chn);
    if (ch < 0) return;

    // r — подстрока от '[' после "r": до последнего ']' (кадр ≤ 240 Б).
    const char* rp = strstr(payload, "\"r\":");
    if (rp == nullptr) return;
    const char* rStart = strchr(rp, '[');
    const char* rEnd = strrchr(payload, ']');
    if (rStart == nullptr || rEnd == nullptr || rEnd < rStart) return;
    size_t rLen = (size_t)(rEnd - rStart + 1);
    if (crc32zlib((const uint8_t*)rStart, rLen) != crc) {
        log(LogLevel::Warning, "архив W8: кадр seq %lu — CRC не сошёлся, пропуск",
            (unsigned long)seq);
        return;
    }
    _bfRxFrames++;

    // Разбор записей [ts,mn,mx,avg] — в буфер, если час в окне суток.
    // 0.9.2: r — [[rec],[rec]] (ДВОЙНАЯ скобка). Поиск '[' с p+1: первый
    // найденный — внутренняя скобка первой записи, не скобка массива
    // (дефект 0.9.0/0.9.1: strchr(p,...) хватал внешнюю '[', strtoul
    // упирался во вторую '[' → break → 0 записей из ЛЮБОГО непустого
    // кадра; пустые "[]" проходили безотказно — потому и не поймали).
    // Проверено на пойманных с провода кадрах 05.10.2026.
    const char* p = rStart;
    while ((p = strchr(p + 1, '[')) != nullptr && p < rEnd) {
        p++;
        char* end = nullptr;
        uint32_t ts = (uint32_t)strtoul(p, &end, 10);
        if (end == p || *end != ',') break;
        float mn = strtof(end + 1, &end);
        if (*end != ',') break;
        float mx = strtof(end + 1, &end);
        if (*end != ',') break;
        float av = strtof(end + 1, &end);
        if (ts >= _bfDayStart && ts < _bfDayStart + 86400UL) {
            bool dup = false;
            for (uint8_t i = 0; i < _bfCnt[ch]; ++i)
                if (_bfBuf[ch][i].ts == ts) { dup = true; break; }
            if (!dup && _bfCnt[ch] < 24) {
                DlogAggr& d = _bfBuf[ch][_bfCnt[ch]++];
                d.ts = ts; d.mn = mn; d.mx = mx; d.avg = av;
            }
        }
        p = end;
    }
    if (_bfRxFrames >= BF_FRAMES_PER_DAY) _bfEndReq = 1;
}

// --- Финал сессии: мердж буферов в w7a (tick, SD можно) -----------------------
void HmArchiveModule::bfFinalize(const char* why) {
    _bfActive = false;
    _bfEndReq = 0;
    _bfSessGapMs = millis();

    uint16_t total = 0;
    for (uint8_t c = 0; c < CH_COUNT; ++c) total += _bfCnt[c];

    if (total == 0) {
        bfDeadMark(_bfDayStart);
        _bfUnserv++;
        log(LogLevel::Info, "архив W8: сессия %s — 0 записей, сутки помечены необслужимыми",
            why);
        return;
    }
    uint32_t before = _bfRecMerged;
    for (uint8_t c = 0; c < CH_COUNT; ++c)
        if (_bfCnt[c] > 0) bfMergeChannel(c);
    _bfHolesClosed++;
    log(LogLevel::Info, "архив W8: сессия %s — %lu кадров, влито %lu записей",
        why, (unsigned long)_bfRxFrames, (unsigned long)(_bfRecMerged - before));
}

// Мердж одного канала: потоковый two-pointer (оба источника по возрастанию
// ts), запись через tmp+rename (урок питания). Существующие ts первичны.
void HmArchiveModule::bfMergeChannel(uint8_t ch) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) return;

    // Буфер — по возрастанию ts (вставка, ≤24 записей).
    for (uint8_t i = 1; i < _bfCnt[ch]; ++i) {
        DlogAggr v = _bfBuf[ch][i];
        int j = (int)i - 1;
        while (j >= 0 && _bfBuf[ch][j].ts > v.ts) {
            _bfBuf[ch][j + 1] = _bfBuf[ch][j];
            j--;
        }
        _bfBuf[ch][j + 1] = v;
    }

    time_t t0 = (time_t)_bfDayStart;
    struct tm* tmv = gmtime(&t0);
    int year = tmv ? tmv->tm_year + 1900 : 1970;
    char path[64], tmp[72];
    filePath(ch, year, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    fs::File in;
    if (sd->exists(path)) in = sd->open(path, FILE_READ);
    fs::File out = sd->open(tmp, FILE_WRITE);
    if (!out) { if (in) in.close(); return; }

    // Заголовок: из входа или новый.
    if (in && in.size() >= sizeof(W7aHeader)) {
        W7aHeader h;
        if (in.read((uint8_t*)&h, sizeof(h)) == sizeof(h))
            out.write((const uint8_t*)&h, sizeof(h));
    } else {
        writeHeaderIfNew(out, chName(ch));
    }

    uint16_t merged = 0;
    uint8_t bi = 0;
    DlogAggr r;
    while (in && in.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
        while (bi < _bfCnt[ch] && _bfBuf[ch][bi].ts < r.ts) {
            out.write((const uint8_t*)&_bfBuf[ch][bi], sizeof(DlogAggr));
            merged++; bi++;
        }
        if (bi < _bfCnt[ch] && _bfBuf[ch][bi].ts == r.ts) bi++;  // дубль — мастерское первично
        out.write((const uint8_t*)&r, sizeof(r));
    }
    while (bi < _bfCnt[ch]) {
        out.write((const uint8_t*)&_bfBuf[ch][bi], sizeof(DlogAggr));
        merged++; bi++;
    }
    if (in) in.close();
    out.close();

    sd->remove(path);
    if (sd->rename(tmp, path)) {
        _bfRecMerged += merged;
        if (merged > 0)
            log(LogLevel::Info, "архив W8: %s — влито %u записей", chName(ch), merged);
    } else {
        log(LogLevel::Warning, "архив W8: %s — rename не удался", chName(ch));
        sd->remove(tmp);
    }
}
