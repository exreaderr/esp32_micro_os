// ============================================================================
// JournalService.cpp — обвязка журнала M3.1 (логика — JournalCore.h)
// ============================================================================
#include "JournalService.h"
#include "BrokerService.h"
#include "SdService.h"
#include <services/ConfigService.h>
#include <services/TimeService.h>
#include <core/Events.h>
#include <FS.h>

JournalService& JournalService::getInstance() {
    static JournalService inst;
    return inst;
}

// --- Конфиг -------------------------------------------------------------------
void JournalService::init() {
    _enabled = cfgGetBool("journal.enabled", true);
    cfgGetStr("journal.topics", _filter, sizeof(_filter),
              "microos/+/events/#,microos/+/state");
    _flushS  = (uint32_t)cfgGetInt("journal.flush_s",  (long)JRN_FLUSH_DEF_S);
    _maxMb   = (uint32_t)cfgGetInt("journal.max_mb",   (long)JRN_MAX_MB_DEF);
    _maxDays = (uint32_t)cfgGetInt("journal.max_days", (long)JRN_MAX_DAYS_DEF);
    log(LogLevel::Info,
        "journal: %s | фильтр \"%s\" | сброс %lu с | сегмент %lu МБ | глубина %lu дн",
        _enabled ? "ON" : "OFF", _filter,
        (unsigned long)_flushS, (unsigned long)_maxMb, (unsigned long)_maxDays);
}

void JournalService::start() {
    if (!_enabled) return;
    BrokerService::getInstance().addEventHook(&JournalService::onBrokerEvent);
}

void JournalService::stop() {
    if (!_enabled) return;
    BrokerService::getInstance().removeEventHook(&JournalService::onBrokerEvent);
    materializeRepeat(false);   // 5.8.0: хвост открытой серии — в кольцо до слива
    flushRing();
    closeSegment();
}

// --- Хук брокера: СИНХРОННО, контекст tick. Только формат + кольцо. ----------
void JournalService::onBrokerEvent(const BrokerEventInfo& info) {
    JournalService& self = getInstance();
    if (!self._enabled) return;

    uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
    if (ts < 1700000000UL) ts = 0;   // время недостоверно — честный 0

    if (info.type == BrokerEventInfo::Publish) {
        if (!jrn::topicListed(self._filter, info.topic)) return;

        // Антифлуд 5.8.0 (урок ночи 14→15.08: защёлкнутый HEAP_CRITICAL
        // замка — одна и та же строка каждые 30 с всю ночь, 1488 строк
        // шума в сегменте). Серия «топик+тело» подряд: первое событие
        // пишется сразу, повторы считаются, при закрытии серии выходит
        // сводка «<тело> ×N» (см. materializeRepeat). Потерь нет: N
        // сохраняет количество, штампы первого и последнего — в строках.
        bool same = self._streakActive &&
                    strncmp(info.topic, self._lastTopic,
                            sizeof(self._lastTopic) - 1) == 0 &&
                    strncmp(info.payload, self._lastPayload,
                            sizeof(self._lastPayload) - 1) == 0;
        if (same) {
            self._repeatCount++;
            if (self._repeatCount >= JRN_REPEAT_CAP) {
                self.materializeRepeat(true);   // серия длинная — режем на куски
            }
            return;
        }
        self.materializeRepeat(false);   // серия сменилась — закрыть прошлую

        char line[JRN_LINE_LEN];
        size_t n = jrn::formatLine(line, sizeof(line), ts, info.topic,
                                   info.payload, info.truncated);
        if (n > 0) self._ring.push(line, (uint16_t)n);   // переполнение: dropped++
        strncpy(self._lastTopic, info.topic, sizeof(self._lastTopic) - 1);
        self._lastTopic[sizeof(self._lastTopic) - 1] = '\0';
        strncpy(self._lastPayload, info.payload, sizeof(self._lastPayload) - 1);
        self._lastPayload[sizeof(self._lastPayload) - 1] = '\0';
        self._streakActive = true;
        self._repeatCount  = 0;
        return;
    }
    // RemoveClient: sMQTT LWT не исполняет — фиксируем смерть сами.
    self.materializeRepeat(false);   // хвост открытой серии не прячем
    char line[JRN_LINE_LEN];
    size_t n = jrn::formatLine(line, sizeof(line), ts,
                               "microos/broker/events/client_left",
                               info.clientId, false);
    if (n > 0) self._ring.push(line, (uint16_t)n);   // переполнение: dropped++
}

// Антифлуд 5.8.0: серия повторов закрылась — сводка «<тело> ×N» в кольцо
// (N — повторы СВЕРХ уже записанной первой строки; время сводки = время
// последнего повтора). continues=true: серия не кончилась, а перевалила
// кап — счётчик обнуляем, серию не гасим (длинный флуд режется на куски).
void JournalService::materializeRepeat(bool continues) {
    if (!_streakActive) return;
    if (_repeatCount > 0) {
        uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
        if (ts < 1700000000UL) ts = 0;
        char pl[JRN_PAYLOAD_KEEP + 24];
        snprintf(pl, sizeof(pl), "%s ×%lu",
                 _lastPayload, (unsigned long)_repeatCount);
        char line[JRN_LINE_LEN];
        size_t n = jrn::formatLine(line, sizeof(line), ts,
                                   _lastTopic, pl, false);
        if (n > 0) _ring.push(line, (uint16_t)n);
    }
    _repeatCount = 0;
    if (!continues) _streakActive = false;
}

// --- События ядра --------------------------------------------------------------
bool JournalService::canHandleEvent(int32_t eventId) const {
    return _enabled && eventId == SH_EVENT_SHUTDOWN;
}

void JournalService::onEvent(int32_t eventId, const ShEventData*) {
    if (eventId != SH_EVENT_SHUTDOWN) return;
    // Финальный flush по штатному завершению (концепция ИБЖ): последняя
    // пачка не должна сгореть с ребутом. Бюджет shutdown короткий — пишем
    // что успели, без ротаций и очисток.
    materializeRepeat(false);   // 5.8.0: сводка открытой серии тоже в финал
    flushRing();
    closeSegment();
}

// --- Тик: периодический сброс + обслуживание -----------------------------------
void JournalService::tick() {
    if (!_enabled) return;
    uint32_t now = millis();

    bool highWater = _ring.count >= (JRN_RING_CAP * 3) / 4;
    bool timeUp = (now - _lastFlushMs) >= _flushS * 1000UL;
    if (_ring.count > 0 && (highWater || timeUp)) flushRing();

    rotateIfNeeded();

    if (now - _lastCleanupMs >= CLEANUP_INTERVAL_MS) {
        _lastCleanupMs = now;
        cleanupExpired();
    }
}

// --- Файлы ---------------------------------------------------------------------
void JournalService::fullPath(char* out, size_t cap, const char* name) const {
    snprintf(out, cap, "%s/%s", DIR, name);
}

bool JournalService::openSegment() {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { _degraded = true; return false; }
    if (!sd->exists(DIR)) sd->mkdir(DIR);

    uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
    if (ts < 1700000000UL) ts = 0;
    bool sameDay = false;
    if (_segName[0] != '\0' && ts != 0) {
        char today[JRN_NAME_LEN];
        jrn::segmentName(today, sizeof(today), ts, false);
        sameDay = (strncmp(_segName, today, 15) == 0);   // "events-YYYYMMDD"
    }
    if (_segName[0] == '\0' || (ts != 0 && !sameDay)) {
        // первый сегмент за старт или новые сутки
        jrn::segmentName(_segName, sizeof(_segName), ts, false);
        bootCheck(_segName);
    }
    char path[JRN_NAME_LEN + 12];
    fullPath(path, sizeof(path), _segName);
    fs::File* f = new fs::File(sd->open(path, FILE_APPEND));
    if (!f || !(*f)) {
        if (f) { delete f; }
        _degraded = true;
        log(LogLevel::Error, "journal: open %s FAILED", path);
        return false;
    }
    _file = f;
    _fileOpen = true;
    _segBytes = (uint32_t)f->size();
    _degraded = false;
    log(LogLevel::Info, "journal: сегмент %s (%lu КБ)", path,
        (unsigned long)(_segBytes / 1024));
    return true;
}

void JournalService::closeSegment() {
    if (!_fileOpen) return;
    fs::File* f = (fs::File*)_file;
    f->flush();
    f->close();
    delete f;
    _file = nullptr;
    _fileOpen = false;
}

void JournalService::bootCheck(const char* name) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) return;
    char path[JRN_NAME_LEN + 12];
    fullPath(path, sizeof(path), name);
    if (!sd->exists(path)) return;
    fs::File f = sd->open(path, FILE_READ);
    if (!f) return;
    size_t sz = f.size();
    if (sz == 0) { f.close(); return; }
    size_t scan = sz < TAIL_SCAN ? sz : TAIL_SCAN;
    uint8_t tail[TAIL_SCAN];
    f.seek(sz - scan);
    size_t got = f.read(tail, scan);
    f.close();
    size_t valid = jrn::validTail(tail, got);
    if (valid == got) return;   // хвост цел

    // Хвост порван (обрыв питания посреди записи): сегмент — улика,
    // переименовываем с меткой torn, свежая запись пойдёт в новый файл.
    uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
    if (ts < 1700000000UL) ts = 0;
    char torn[JRN_NAME_LEN];
    jrn::segmentName(torn, sizeof(torn), ts, true);         // events-YYYYMMDD-HHMMSS
    // вставляем "-torn" перед ".jsonl": events-YYYYMMDD-tornHHMMSS.jsonl
    char tornPath[JRN_NAME_LEN + 16];
    char* dot = strstr(torn, ".jsonl");
    if (dot == nullptr) { snprintf(tornPath, sizeof(tornPath), "%s/%s.torn", DIR, name); }
    else {
        *dot = '\0';
        // torn сейчас "events-YYYYMMDD-HHMMSS" — соберём torn-имя
        snprintf(tornPath, sizeof(tornPath), "%s/%s-torn.jsonl", DIR, torn);
    }
    if (sd->rename(path, tornPath)) {
        log(LogLevel::Warning, "journal: хвост %s порван (%u/%u Б валидно) -> %s",
            path, (unsigned)valid, (unsigned)got, tornPath);
    } else {
        log(LogLevel::Error, "journal: rename %s FAILED — пишем поверх", path);
    }
}

void JournalService::rotateIfNeeded() {
    if (!_fileOpen) return;
    // Ротация по размеру: сегмент перерос — закрыть, следующий openSegment
    // возьмёт имя с временем (те же сутки, новый файл).
    if (_maxMb > 0 && _segBytes >= _maxMb * 1024UL * 1024UL) {
        log(LogLevel::Info, "journal: ротация по размеру (%lu МБ)",
            (unsigned long)_maxMb);
        closeSegment();
        // следующий сегмент — с временным суффиксом, чтобы не сесть на то же имя
        uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
        if (ts >= 1700000000UL) {
            jrn::segmentName(_segName, sizeof(_segName), ts, true);
        } else {
            _segName[0] = '\0';   // время нет — openSegment возьмёт undated
        }
        return;
    }
    // Смена суток: переехать на свежий дневной сегмент
    uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
    if (ts >= 1700000000UL && _segName[0] != '\0') {
        char today[JRN_NAME_LEN];
        jrn::segmentName(today, sizeof(today), ts, false);
        if (strncmp(_segName, today, 15) != 0) {
            closeSegment();
            _segName[0] = '\0';   // openSegment откроет today + bootCheck
        }
    }
}

void JournalService::cleanupExpired() {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) return;
    uint32_t ts = (uint32_t)TimeService::getInstance().getUnixTime();
    if (ts < 1700000000UL) return;   // без времени возраст не считаем
    fs::File dir = sd->open(DIR);
    if (!dir || !dir.isDirectory()) return;
    uint16_t removed = 0;
    char path[JRN_NAME_LEN + 12];
    // Потоково: openNextFile, без «буфера на весь каталог»
    for (fs::File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        const char* nm = e.name();   // полный путь у SD: /journal/events-...
        const char* base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        if (jrn::segmentExpired(base, ts, _maxDays)) {
            e.close();
            snprintf(path, sizeof(path), "%s/%s", DIR, base);
            if (sd->remove(path)) removed++;
        } else {
            e.close();
        }
    }
    dir.close();
    if (removed > 0) log(LogLevel::Info, "journal: очистка по возрасту — %u сегм.", removed);
}

// --- Сброс кольца ---------------------------------------------------------------
void JournalService::flushRing() {
    if (_ring.count == 0) return;
    if (!_fileOpen && !openSegment()) {
        // SD нет/сбой: кольцо переполнится и dropped покажет потери.
        // Молчим (тихое ядро) — degraded виден в статусе.
        return;
    }
    fs::File* f = (fs::File*)_file;
    char line[JRN_LINE_LEN];
    uint16_t n;
    uint16_t batch = 0;
    while ((n = _ring.pop(line, sizeof(line))) > 0) {
        size_t w = f->write((const uint8_t*)line, n);
        if (w != n) {   // карта полна/сбой — строка потеряна, dropped честно
            _ring.dropped++;
        } else {
            _segBytes += n;
            _written++;
            batch++;
        }
    }
    f->flush();
    _lastFlushMs = millis();
    _lastFlushUnix = (uint32_t)TimeService::getInstance().getUnixTime();
    if (batch > 0) {
        log(LogLevel::Debug, "journal: flush %u строк (%lu КБ сегмент)",
            batch, (unsigned long)(_segBytes / 1024));
    }
}

// ============================================================================
// M3.2: ЧТЕНИЕ ДЛЯ ВЬЮЕРА (контекст HTTP-задачи; read-only, потоково)
// ============================================================================
// Кондиции: сегменты читаем блоками _rdbuf (BSS), строку собираем в carry
// JRN_LINE_LEN на стеке; сегмент целиком в память — НИКОГДА. Строка без
// завершающего '\n' (хвост, который журнал пишет прямо сейчас) не отдаётся:
// допишется — придёт следующим курсором. Строка-монстр (> бюджета строки)
// выкидывается целиком — битая строка в JSON-массив страницы не попадает.
// ============================================================================

bool JournalService::validSegmentName(const char* name) const {
    // Только свои: events-<цифры/строки/точка/дефис>.jsonl, без путей — HTTP-
    // аргумент это чужой ввод, '/' и '..' здесь не пройдут никогда.
    if (name == nullptr || strncmp(name, "events-", 7) != 0) return false;
    size_t len = strlen(name);
    if (len < 14 || len >= JRN_NAME_LEN) return false;
    if (strcmp(name + len - 6, ".jsonl") != 0) return false;
    for (const char* p = name + 7; *p != '\0'; ++p) {
        char c = *p;
        bool okc = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                   c == '-' || c == '.';
        if (!okc) return false;
    }
    return true;
}

// 5.8.0, «Скачать журнал»: валидированный путь сегмента для потоковой
// отдачи (стримит HomeMasterUi — файл целиком в heap НЕ поднимается).
bool JournalService::segmentPath(const char* name, char* out, size_t cap) const {
    if (!validSegmentName(name)) return false;
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) return false;
    fullPath(out, cap, name);
    return sd->exists(out);
}

bool JournalService::apiFiles(char* out, size_t cap) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { snprintf(out, cap, "{\"ok\":0,\"err\":\"sd\"}"); return true; }
    if (!sd->exists(DIR)) {   // журнал ещё ни разу не писал — честный пустой
        snprintf(out, cap, "{\"ok\":1,\"current\":\"%s\",\"trunc\":0,\"files\":[]}",
                 _segName);
        return true;
    }
    // Потоковый обход + вставка по убыванию имени (= по дате: events-YYYYMMDD…)
    uint8_t cnt = 0; bool trunc = false;
    fs::File dir = sd->open(DIR);
    if (!dir || !dir.isDirectory()) {
        snprintf(out, cap, "{\"ok\":0,\"err\":\"dir\"}");
        return true;
    }
    for (fs::File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        const char* nm = e.name();
        const char* base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        if (!validSegmentName(base)) { e.close(); continue; }
        if (cnt >= JRN_LIST_MAX) { trunc = true; e.close(); continue; }
        uint8_t i = cnt;
        while (i > 0 && strcmp(_lstNames[i - 1], base) < 0) {
            memcpy(_lstNames[i], _lstNames[i - 1], JRN_NAME_LEN);
            _lstSizes[i] = _lstSizes[i - 1];
            --i;
        }
        strncpy(_lstNames[i], base, JRN_NAME_LEN - 1);
        _lstNames[i][JRN_NAME_LEN - 1] = '\0';
        _lstSizes[i] = (uint32_t)e.size();
        cnt++;
        e.close();
    }
    dir.close();
    size_t pos = (size_t)snprintf(out, cap,
        "{\"ok\":1,\"current\":\"%s\",\"trunc\":%d,\"files\":[",
        _segName, trunc ? 1 : 0);
    for (uint8_t i = 0; i < cnt && pos < cap - 80; ++i) {
        pos += (size_t)snprintf(out + pos, cap - pos, "%s{\"n\":\"%s\",\"b\":%lu}",
                                i ? "," : "", _lstNames[i],
                                (unsigned long)_lstSizes[i]);
    }
    snprintf(out + pos, cap - pos, "]}");
    return true;
}

// Хвост текущего сегмента: окно JRN_TAIL_WIN байт с конца, два прохода —
// подсчёт полных строк и выдача последних maxLines. Два прохода по 14 КБ
// дешевле, чем кольцо последних N строк в памяти (25 КБ — ни в BSS, ни в кучу).
bool JournalService::apiTail(char* out, size_t cap, uint16_t maxLines) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr || _segName[0] == '\0') {
        snprintf(out, cap, "{\"ok\":0,\"err\":\"degraded\"}");
        return true;
    }
    char path[JRN_NAME_LEN + 12];
    fullPath(path, sizeof(path), _segName);
    if (!sd->exists(path)) {
        snprintf(out, cap,
            "{\"ok\":1,\"file\":\"%s\",\"size\":0,\"skipped\":0,\"lines\":[],\"count\":0}",
            _segName);
        return true;
    }
    fs::File f = sd->open(path, FILE_READ);
    if (!f) { snprintf(out, cap, "{\"ok\":0,\"err\":\"open\"}"); return true; }
    uint32_t sz = (uint32_t)f.size();
    uint32_t win = sz < JRN_TAIL_WIN ? sz : JRN_TAIL_WIN;
    uint32_t start = sz - win;
    if (maxLines == 0 || maxLines > JRN_PAGE_LINES) maxLines = JRN_PAGE_LINES;

    // Проход 1: полные строки окна = кол-во '\n'; при start>0 первый '\n'
    // закрывает НЕполную строку (мы вошли посередине) — её не считаем.
    uint32_t usable = 0; bool firstSkipped = (start == 0);
    f.seek(start);
    {
        uint32_t left = win;
        while (left > 0) {
            size_t want = left > JRN_BLK ? JRN_BLK : left;
            size_t got = f.read(_rdbuf, want);
            if (got == 0) break;
            for (size_t i = 0; i < got; ++i) {
                if (_rdbuf[i] != '\n') continue;
                if (!firstSkipped) { firstSkipped = true; continue; }
                usable++;
            }
            left -= got;
        }
    }
    uint32_t skip = usable > maxLines ? usable - maxLines : 0;

    // Проход 2: пропускаем skip полных строк, остальные — в страницу.
    size_t pos = (size_t)snprintf(out, cap,
        "{\"ok\":1,\"file\":\"%s\",\"size\":%lu,\"skipped\":%lu,\"lines\":",
        _segName, (unsigned long)sz, (unsigned long)skip);
    jrn::JrnPage pg;
    jrn::pageBegin(pg, out + pos, cap - pos);

    f.seek(start);
    uint32_t left = win, skipped = 0;
    bool partial = (start > 0);
    char ln[JRN_LINE_LEN]; size_t llen = 0; bool lbad = false;
    while (left > 0 && pg.count < maxLines) {
        size_t want = left > JRN_BLK ? JRN_BLK : left;
        size_t got = f.read(_rdbuf, want);
        if (got == 0) break;
        for (size_t i = 0; i < got && pg.count < maxLines; ++i) {
            char c = (char)_rdbuf[i];
            if (c != '\n') {
                if (!lbad) {
                    if (llen < sizeof(ln) - 1) ln[llen++] = c;
                    else lbad = true;   // монстр: выкидываем строку целиком
                }
                continue;
            }
            if (partial) { partial = false; }
            else if (skipped < skip) { skipped++; }
            else if (!lbad && llen > 0) {
                if (!jrn::pagePut(pg, ln, llen)) break;  // буфер ответа полон
            }
            lbad = false; llen = 0;
        }
        left -= got;
    }
    f.close();
    pos += jrn::pageEnd(pg);
    snprintf(out + pos, cap - pos, ",\"count\":%u}", (unsigned)pg.count);
    return true;
}

// Страница сегмента с фильтром и курсором: читаем от offset, отдаём
// прошедшие фильтр строки, пока не кончился файл, лимит строк ИЛИ место в
// буфере ответа. next — байт-курсор продолжения (more=1) либо EOF (more=0).
bool JournalService::apiRead(char* out, size_t cap, const char* name,
                             uint32_t offset, const jrn::JrnQuery& q) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { snprintf(out, cap, "{\"ok\":0,\"err\":\"sd\"}"); return true; }
    if (!validSegmentName(name)) {
        snprintf(out, cap, "{\"ok\":0,\"err\":\"bad_name\"}");
        return true;
    }
    char path[JRN_NAME_LEN + 12];
    fullPath(path, sizeof(path), name);
    if (!sd->exists(path)) {
        snprintf(out, cap, "{\"ok\":0,\"err\":\"no_file\"}");
        return true;
    }
    fs::File f = sd->open(path, FILE_READ);
    if (!f) { snprintf(out, cap, "{\"ok\":0,\"err\":\"open\"}"); return true; }
    uint32_t sz = (uint32_t)f.size();
    if (offset > sz) offset = sz;
    f.seek(offset);

    size_t pos = (size_t)snprintf(out, cap,
        "{\"ok\":1,\"file\":\"%s\",\"size\":%lu,\"offset\":%lu,\"lines\":",
        name, (unsigned long)sz, (unsigned long)offset);
    jrn::JrnPage pg;
    jrn::pageBegin(pg, out + pos, cap - pos);

    uint32_t next = sz;   // по умолчанию — дошли до конца
    bool more = false;
    uint32_t lineStart = offset;
    char ln[JRN_LINE_LEN]; size_t llen = 0; bool lbad = false;
    uint32_t cursor = offset;
    bool stop = false;
    while (!stop && cursor < sz) {
        size_t want = sz - cursor; if (want > JRN_BLK) want = JRN_BLK;
        size_t got = f.read(_rdbuf, want);
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            uint32_t abs = cursor + (uint32_t)i;
            char c = (char)_rdbuf[i];
            if (c != '\n') {
                if (!lbad) {
                    if (llen < sizeof(ln) - 1) ln[llen++] = c;
                    else lbad = true;
                }
                continue;
            }
            // строка [lineStart .. abs) собрана
            if (!lbad && llen > 0) {
                ln[llen] = '\0';
                if (jrn::lineMatches(ln, q)) {
                    if (!jrn::pagePut(pg, ln, llen)) {
                        next = lineStart;      // не влезла — продолжим с неё
                        more = true; stop = true; break;
                    }
                    if (pg.count >= JRN_PAGE_LINES) {
                        next = abs + 1;        // лимит — продолжим со следующей
                        more = true; stop = true; break;
                    }
                }
            }
            lbad = false; llen = 0;
            lineStart = abs + 1;
        }
        cursor += got;
    }
    f.close();
    pos += jrn::pageEnd(pg);
    snprintf(out + pos, cap - pos, ",\"count\":%u,\"next\":%lu,\"more\":%d}",
             (unsigned)pg.count, (unsigned long)next, more ? 1 : 0);
    return true;
}
