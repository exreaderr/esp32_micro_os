// ============================================================================
// JournalCore.h — ЖУРНАЛ СОБЫТИЙ НА SD: ЧИСТАЯ ЛОГИКА (M3.1, host-тесты)
// ============================================================================
// Долгий журнал, которого нет у контроллеров (AuditService держит 64 КБ в
// LittleFS — оперативный след; здесь — архив парка на SD: недели и месяцы).
//
// Поток данных:
//   MQTT PUBLISH клиента -> хук BrokerService (контекст tick, СИНХРОННО,
//   SD трогать НЕЛЬЗЯ) -> formatLine() в RAM-кольцо -> tick JournalService
//   сбрасывает кольцо пачкой в /journal/events-YYYYMMDD.jsonl + fsync.
//
// Кондиции (унаследованы, не обсуждаются):
//   · фиксированные бюджеты BSS, без кучи и std::container (урок v4.2.2);
//   · «буфер на всю коллекцию» запрещён: файлы читаются/пишутся потоково;
//   · FAT боится внезапного выключения: батч-сброс, fsync, boot-check хвоста
//     (обрезка порванной записи), финальный flush по SH_EVENT_SHUTDOWN;
//   · переполнение кольца — не авария: роняем НОВУЮ строку, счётчик dropped
//     растёт (тихое ядро, диагностика счётчиками).
//
// Всё здесь — чистая математика над POD: тестируется на хосте (tests.cpp).
// Файловый ввод-вывод, MQTT, RTC — в JournalService (обёртка, профиль hm).
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// --- БЮДЖЕТЫ ------------------------------------------------------------------
constexpr uint16_t JRN_LINE_LEN    = 256;   // строка JSONL целиком, с '\n'
constexpr uint16_t JRN_PAYLOAD_KEEP= 160;   // потолок тела события в строке
constexpr uint8_t  JRN_RING_CAP    = 32;    // строк в RAM-кольце (8 КБ BSS)
constexpr uint8_t  JRN_NAME_LEN    = 40;    // events-YYYYMMDD-HHMMSS.jsonl
constexpr uint8_t  JRN_SRC_LEN     = 24;    // идентификатор источника
constexpr uint16_t JRN_FILTER_LEN  = 48;    // как bridge.down_extra (CFG)
constexpr uint32_t JRN_FLUSH_DEF_S = 5;     // дефолт периода сброса, с
constexpr uint32_t JRN_MAX_MB_DEF  = 100;   // дефолт потолка сегмента
constexpr uint32_t JRN_MAX_DAYS_DEF= 90;    // дефолт глубины архива

namespace jrn {

// --- ДАТА ИЗ UNIX (civil-from-days, Говард Хиннант; чистая математика) -------
// Нужна имён сегментов и очистке по возрасту. Без time.h — одинаково
// работает на хосте и на железе (на железе время может быть недостоверно —
// тогда ts=0 и имя сегмента честно events-undated.jsonl, см. segmentName).
inline void civilFromUnix(uint32_t ts, uint16_t& y, uint8_t& mo, uint8_t& d,
                          uint8_t& h, uint8_t& mi, uint8_t& s) {
    uint32_t days = ts / 86400U;
    uint32_t rem  = ts % 86400U;
    h  = (uint8_t)(rem / 3600U);
    mi = (uint8_t)((rem % 3600U) / 60U);
    s  = (uint8_t)(rem % 60U);
    // days since 1970-01-01 -> гражданская дата
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t  yy  = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    uint32_t mp  = (5*doy + 2)/153;
    d  = (uint8_t)(doy - (153*mp + 2)/5 + 1);
    mo = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
    y  = (uint16_t)(mo <= 2 ? yy + 1 : yy);
}

// --- ИМЯ СЕГМЕНТА -------------------------------------------------------------
// Дневной сегмент: events-YYYYMMDD.jsonl. При ротации по размеру внутри
// суток добавляется время: events-YYYYMMDD-HHMMSS.jsonl. Время недостоверно
// (ts==0): events-undated.jsonl (без суффиксов — ротация по размеру тогда
// невозможна по имени: сегмент один, растёт до max_mb и замолкает с событием
// деградации; честнее, чем мусорные имена).
inline void segmentName(char* out, size_t cap, uint32_t ts, bool withTime) {
    if (cap < JRN_NAME_LEN) { if (cap) out[0] = '\0'; return; }
    if (ts == 0) {
        strcpy(out, "events-undated.jsonl");
        return;
    }
    uint16_t y; uint8_t mo, d, h, mi, s;
    civilFromUnix(ts, y, mo, d, h, mi, s);
    int n = 0;
    out[n++] = 'e'; out[n++] = 'v'; out[n++] = 'e'; out[n++] = 'n';
    out[n++] = 't'; out[n++] = 's'; out[n++] = '-';
    out[n++] = (char)('0' + (y / 1000) % 10); out[n++] = (char)('0' + (y / 100) % 10);
    out[n++] = (char)('0' + (y / 10) % 10);   out[n++] = (char)('0' + y % 10);
    out[n++] = (char)('0' + mo / 10); out[n++] = (char)('0' + mo % 10);
    out[n++] = (char)('0' + d / 10);  out[n++] = (char)('0' + d % 10);
    if (withTime) {
        out[n++] = '-';
        out[n++] = (char)('0' + h / 10);  out[n++] = (char)('0' + h % 10);
        out[n++] = (char)('0' + mi / 10); out[n++] = (char)('0' + mi % 10);
        out[n++] = (char)('0' + s / 10);  out[n++] = (char)('0' + s % 10);
    }
    const char* ext = ".jsonl";
    for (const char* p = ext; *p; ++p) out[n++] = *p;
    out[n] = '\0';
}

// --- ВОЗРАСТ СЕГМЕНТА ---------------------------------------------------------
// Разбор даты из имени (для очистки по max_days). false — имя не наше
// (чужой файл в каталоге не трогаем никогда).
inline bool segmentDate(const char* name, uint16_t& y, uint8_t& mo, uint8_t& d) {
    if (strncmp(name, "events-", 7) != 0) return false;
    const char* p = name + 7;
    for (int i = 0; i < 8; ++i) if (p[i] < '0' || p[i] > '9') return false;
    y  = (uint16_t)((p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0'));
    mo = (uint8_t)((p[4]-'0')*10 + (p[5]-'0'));
    d  = (uint8_t)((p[6]-'0')*10 + (p[7]-'0'));
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    return true;
}

// Дни от гражданской даты (обратная к civilFromUnix, для сравнения возраста).
inline uint32_t daysFromCivil(uint16_t y, uint8_t mo, uint8_t d) {
    int64_t yy = mo <= 2 ? (int64_t)y - 1 : y;
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    uint32_t yoe = (uint32_t)(yy - era * 400);
    uint32_t mp  = mo > 2 ? mo - 3 : mo + 9;
    uint32_t doy = (153*mp + 2)/5 + d - 1;
    uint32_t doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return (uint32_t)(era * 146097 + (int64_t)doe - 719468);
}

// Сегмент старше maxDays относительно nowTs?
inline bool segmentExpired(const char* name, uint32_t nowTs, uint32_t maxDays) {
    uint16_t y; uint8_t mo, d;
    if (!segmentDate(name, y, mo, d)) return false;   // не наш файл
    uint32_t fileDay = daysFromCivil(y, mo, d);
    uint32_t nowDay  = nowTs / 86400U;
    if (nowDay <= fileDay) return false;              // из будущего — не трогаем
    return (nowDay - fileDay) > maxDays;
}

// --- ФИЛЬТР ТОПИКОВ (MQTT-маски '+' и '#', список через запятую) --------------
// Журнал — читатель, не граница безопасности (в отличие от bridge.down_extra),
// поэтому маски разрешены. Дефолт кураторский: "microos/+/events/#" +
// "microos/+/state" — события парка и живость устройств, БЕЗ телеметрии.
inline bool maskMatch(const char* f, const char* topic) {
    // f — один фильтр, может содержать + и # (по MQTT: '#' только последним)
    while (*f) {
        if (*f == '#') return true;                       // хвост любой
        const char* ft = f;  while (*ft && *ft != '/') ft++;   // сегмент фильтра
        const char* tt = topic; while (*tt && *tt != '/') tt++; // сегмент топика
        size_t fl = (size_t)(ft - f), tl = (size_t)(tt - topic);
        bool plus = (fl == 1 && *f == '+');
        if (!plus && (fl != tl || strncmp(f, topic, fl) != 0)) return false;
        if (*ft == '\0' && *tt == '\0') return true;      // сошлись целиком
        if (*tt == '\0') {
            // топик кончился: фильтр ".../#" по MQTT покрывает и сам родителя
            return (ft[0] == '/' && ft[1] == '#' && ft[2] == '\0');
        }
        if (*ft == '\0') return false;                    // топик длиннее
        f = ft + 1; topic = tt + 1;
    }
    return *topic == '\0';
}

// Топик проходит хотя бы один фильтр списка? Пустой список = ничего не писать
// (осознанное «журнал выключен фильтром», как bridge.down_extra).
inline bool topicListed(const char* filters, const char* topic) {
    if (filters == nullptr || filters[0] == '\0') return false;
    const char* p = filters;
    char one[JRN_FILTER_LEN];
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char* end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len > 0 && p[len - 1] == ' ') len--;
        if (len > 0 && len < sizeof(one)) {
            memcpy(one, p, len); one[len] = '\0';
            if (maskMatch(one, topic)) return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}

// --- ИСТОЧНИК ИЗ ТОПИКА -------------------------------------------------------
// microos/smart_lock/events/card -> "smart_lock". Не по схеме — "misc".
inline void srcFromTopic(const char* topic, char* out, size_t cap) {
    if (cap == 0) return;
    const char* s1 = strchr(topic, '/');
    if (s1 == nullptr || s1[1] == '\0') { strncpy(out, "misc", cap - 1); out[cap-1] = '\0'; return; }
    const char* b = s1 + 1;
    const char* e = strchr(b, '/');
    size_t len = e ? (size_t)(e - b) : strlen(b);
    if (len == 0 || len >= cap) len = cap - 1;
    memcpy(out, b, len); out[len] = '\0';
}

// --- СТРОКА JSONL -------------------------------------------------------------
// {"ts":1736...,"src":"smart_lock","t":"microos/.../card","p":"{...}"} + '\n'
// Тело — СТРОКОЙ с экранированием: усечённое (truncated) тело остаётся
// валидной строкой, строка журнала — валидным JSON всегда. Возвращает длину
// без завершающего нуля; 0 — не влезло даже с пустым телом (не журналируем).
inline size_t formatLine(char* out, size_t cap, uint32_t ts, const char* topic,
                         const char* payload, bool truncated) {
    if (cap < JRN_LINE_LEN) return 0;
    const char* tail = truncated ? "\",\"cut\":1}\n" : "\"}\n";
    const size_t tl = strlen(tail);                 // резерв под хвост — сразу
    char src[JRN_SRC_LEN];
    srcFromTopic(topic, src, sizeof(src));
    int n = snprintf(out, cap, "{\"ts\":%lu,\"src\":\"%s\",\"t\":\"%s\",\"p\":\"",
                     (unsigned long)ts, src, topic);
    if (n <= 0 || (size_t)n >= cap - tl - 1) return 0;
    size_t used = (size_t)n;
    // тело: экранируем '"', '\\' и управляющие; потолок JRN_PAYLOAD_KEEP
    // исходных байт (как бюджет внимания: события короткие, монстры не нужны)
    size_t kept = 0;
    for (const char* p = payload; *p && kept < JRN_PAYLOAD_KEEP; ++p, ++kept) {
        char c = *p;
        const char* esc = nullptr;
        if (c == '"' || c == '\\') esc = (c == '"') ? "\\\"" : "\\\\";
        else if ((uint8_t)c < 0x20) esc = "?";              // управляющие — схлопнуть
        if (esc) {
            size_t el = strlen(esc);
            if (used + el >= cap - tl - 1) break;
            memcpy(out + used, esc, el); used += el;
        } else {
            if (used + 1 >= cap - tl - 1) break;
            out[used++] = c;
        }
    }
    if (used + tl >= cap) return 0;
    memcpy(out + used, tail, tl); used += tl;
    out[used] = '\0';
    return used;
}

// --- BOOT-CHECK ХВОСТА --------------------------------------------------------
// FAT + внезапное выключение = хвост файла может быть порван посреди строки.
// Читаем последние len байт сегмента; возвращаем смещение ПОСЛЕ последнего
// '\n' — обрезать файл надо по нему (валидная часть). len==0 или хвост
// оканчивается на '\n' -> len (резать нечего). Ни одного '\n' в хвосте ->
// 0 (весь прочитанный кусок мусорный; сервис решает: пустой файл — ок,
// большой мусор — событие деградации, руки не заменяем).
inline size_t validTail(const uint8_t* buf, size_t len) {
    if (len == 0) return 0;
    for (size_t i = len; i > 0; --i) {
        if (buf[i - 1] == '\n') {
            return (i == len) ? len : i;   // '\n' последним — всё валидно
        }
    }
    return 0;
}

// --- M3.2: ЧТЕНИЕ ЖУРНАЛА (вьюер) ----------------------------------------------
// Строка машинно-порождённая, ключи в фиксированном порядке:
// {"ts":<digits>,"src":"...","t":"...","p":"..."[,"cut":1]}\n
// Поэтому чтение — БЕЗ полного JSON-парсера на железе: префиксный разбор
// ts и точечное сравнение src. Браузер парсит строки сам (они валидны) —
// железо лишь фильтрует и листает. Кондиции те же: потоковость, никаких
// буферов «на весь сегмент».

// Запрос фильтра вьюера (все поля опциональны: пусто/0 = без фильтра).
struct JrnQuery {
    char     src[JRN_SRC_LEN];   // точный источник (поле "src")
    char     q[JRN_FILTER_LEN];  // подстрока по СЫРОЙ строке (топик+тело)
    uint32_t from = 0;           // ts >= from (0 = без границы)
    uint32_t to   = 0;           // ts <= to   (0 = без границы)
};

// ts из начала строки: {"ts":12345,...  Битая строка -> 0.
inline uint32_t lineTs(const char* line) {
    if (strncmp(line, "{\"ts\":", 6) != 0) return 0;
    const char* p = line + 6;
    if (*p < '0' || *p > '9') return 0;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); ++p; }
    return v;
}

// src строки == заданному (точно, до закрывающей кавычки; "smart" != "smart_lock").
inline bool lineSrcIs(const char* line, const char* src) {
    const char* m = strstr(line, "\"src\":\"");
    if (m == nullptr) return false;
    m += 7;
    while (*src != '\0' && *m == *src) { ++m; ++src; }
    return *src == '\0' && *m == '"';
}

// Подстрока по сырой строке. ВНИМАНИЕ: тело экранировано — поиск по '"'
// и '\' неинтуитивен; обычный текст (слова, uid, топики) находится верно.
inline bool lineHas(const char* line, const char* q) {
    if (q == nullptr || q[0] == '\0') return true;
    return strstr(line, q) != nullptr;
}

// Строка проходит фильтр? Честность окна: строки с ts=0 (время при записи
// недостоверно) окном from/to ОТСЕКАЮТСЯ — их время неизвестно, под окно
// подгонять нельзя. Без окна показываются как обычные.
inline bool lineMatches(const char* line, const JrnQuery& query) {
    if (query.from != 0 || query.to != 0) {
        uint32_t ts = lineTs(line);
        if (ts == 0) return false;
        if (query.from != 0 && ts < query.from) return false;
        if (query.to   != 0 && ts > query.to)   return false;
    }
    if (query.src[0] != '\0' && !lineSrcIs(line, query.src)) return false;
    return lineHas(line, query.q);
}

// Сборщик страницы вьюера: копит строки как элементы JSON-массива.
// Каждая строка — валидный JSON-объект, поэтому массив строк валиден БЕЗ
// переэкранирования (кондиция M3.2: железо не перепаковывает данные).
struct JrnPage {
    char*    out = nullptr;
    size_t   cap = 0;
    size_t   pos = 0;
    uint16_t count = 0;
    bool     overflow = false;   // последняя строка не влезла
};
inline void pageBegin(JrnPage& pg, char* out, size_t cap) {
    pg.out = out; pg.cap = cap; pg.pos = 0; pg.count = 0; pg.overflow = false;
    if (cap > 1) { out[0] = '['; out[1] = '\0'; pg.pos = 1; }
}
// line — строка журнала, len — БЕЗ '\n'. false = не влезла: вызывающий
// закрывает страницу и продолжит с этой же строки по курсору.
inline bool pagePut(JrnPage& pg, const char* line, size_t len) {
    if (pg.out == nullptr || pg.cap < 4) { pg.overflow = true; return false; }
    size_t need = len + (pg.count ? 1 : 0) + 2;   // запятая + ']' + '\0'
    if (pg.pos + need > pg.cap) { pg.overflow = true; return false; }
    if (pg.count) pg.out[pg.pos++] = ',';
    memcpy(pg.out + pg.pos, line, len);
    pg.pos += len;
    pg.out[pg.pos] = '\0';
    pg.count++;
    return true;
}
inline size_t pageEnd(JrnPage& pg) {
    if (pg.out == nullptr || pg.cap == 0) return 0;
    if (pg.pos + 2 > pg.cap) pg.pos = pg.cap - 2;   // паранойя, не должно случиться
    pg.out[pg.pos++] = ']';
    pg.out[pg.pos] = '\0';
    return pg.pos;
}

} // namespace jrn

// --- RAM-КОЛЬЦО СТРОК (POD; экземпляр живёт в BSS сервиса) --------------------
// Push НЕ роняет старое: при переполнении новая строка отбрасывается,
// dropped++ — старые события ценнее свежих (свежие ещё придут повторно,
// старые не восстановить). Решение осознанное, обратное тоже защищаемо —
// фиксируем выбор здесь.
struct JrnRing {
    char     buf[JRN_RING_CAP][JRN_LINE_LEN];
    uint16_t len[JRN_RING_CAP];
    uint8_t  head = 0;     // сюда пишем
    uint8_t  tail = 0;     // отсюда читаем
    uint8_t  count = 0;
    uint32_t dropped = 0;  // строк потеряно за всё время (диагностика)

    bool push(const char* line, uint16_t n) {
        if (n == 0 || n >= JRN_LINE_LEN) return false;
        if (count >= JRN_RING_CAP) { dropped++; return false; }
        memcpy(buf[head], line, n);
        len[head] = n;
        head = (uint8_t)((head + 1) % JRN_RING_CAP);
        count++;
        return true;
    }
    // Выгрузка следующей строки в out (без копирования указателей наружу —
    // кольцо одновременно пишет хук). Возвращает длину, 0 — пусто.
    uint16_t pop(char* out, uint16_t cap) {
        if (count == 0) return 0;
        uint16_t n = len[tail];
        if (n >= cap) n = (uint16_t)(cap - 1);
        memcpy(out, buf[tail], n);
        tail = (uint8_t)((tail + 1) % JRN_RING_CAP);
        count--;
        return n;
    }
};
