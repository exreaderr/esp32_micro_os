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
static constexpr uint32_t SESSION_GUARD_MS  = 120000;  // сторож сессии выдачи
static constexpr uint32_t REQ_ANTIBOUNCE_MS = 120000;  // повторный полный req

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
    }
}

void HmArchiveModule::onWeather(const char* payload) {
    uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
    if (ts < 1700000000UL) return;   // время ещё не синхронизировано

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
        "\"sd\":\"%s\",\"files\":[",
        _enabled ? 1 : 0, _started ? 1 : 0, _srcTopic, _gwId,
        (unsigned long)_recWritten, (unsigned long)_recSkipped,
        (unsigned long)_reqServed, (unsigned long)_framesSent,
        _serving ? 1 : 0, (unsigned long)_seq, (unsigned long)_total,
        (sd != nullptr) ? "mounted" : "нет");
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

// Сессия выдачи: до 8 шагов buildFrame за тик (150 мс) — ~кадр за тик.
// 0.8.6: раньше один 512-байтный шаг за тик — 210 кадров ≈ 95+ с, шлюз
// с 60-с таймаутом сборки падал (дефект B полевого теста 01.10).
void HmArchiveModule::serveTick() {
    if (!_serving) return;
    uint32_t now = millis();
    if (now - _reqStartedMs > SESSION_GUARD_MS) {
        abortSession("сторож 120 с");
        return;
    }
    bool done = false;
    for (uint8_t iter = 0; iter < 8 && !done; ++iter) {
        done = buildFrame();
    }
    if (!done) return;              // кадр ещё собирается — следующий тик
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
