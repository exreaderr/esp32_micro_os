// ============================================================================
// WgArchiveRestore.h — ВОССТАНОВЛЕНИЕ DLOG ИЗ АРХИВА МАСТЕРА (0.9.1, W7)
// ----------------------------------------------------------------------------
// Контракт: дизайн-нота W7 рев.3 + пять штифтиков ветки + дополнение
// «кадр ≤ 240 Б» (входящий MQTT-путь капает payload на MQTT_BODY_LEN=256;
// исходящий publishRaw — без капа, отсюда асимметрия размеров).
//
// Кадр (текст, JSON):
//   {"seq":<n>,"total":<N>,"ch":"<id>","day":<d>,"crc":<u32>,
//    "r":[[ts,mn,mx,avg],...]}
//   day=0 — самый старый день набора; записи в r — по возрастанию ts;
//   crc — CRC32 zlib (poly 0xEDB88320, init/xorout 0xFFFFFFFF) над СЫРЫМИ
//   байтами сериализованной строки поля "r" как есть (от '[' до ']').
//   Пустой архив: {"seq":0,"total":0}.
//
// Парсинг — урок №25: только якоря-имена полей с кавычками. Чистая логика,
// host-тестируемая; ФС/MQTT — снаружи (WeatherGateApp).
// ============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
// DlogAggr — канон флота. Угловые скобки: ядро на include-пути (--libraries),
// относительный путь ломается кэш-копией скетча при сборке; host-тесты
// добавляют тот же корень через -I.
#include <services/DataLogCore.h>

namespace wgar {

// До 24 записей в кадре (контракт после дополнения — 6; запас на случай
// будущей ревизии без смены кода шлюза — парсер размер-агностичен).
constexpr uint8_t  MAX_REC_PER_FRAME = 24;
constexpr uint8_t  MAX_CH            = 8;    // = DLOG_MAX_CHANNELS
constexpr uint16_t MAX_REC_PER_CH    = 744;  // весь часовой ярус
constexpr uint16_t MAX_FRAMES        = 256;  // потолок кадров набора

struct Frame {
    uint16_t seq;
    uint16_t total;
    char     ch[12];
    int      day;
    uint32_t crc;
    uint8_t  nrec;
    DlogAggr recs[MAX_REC_PER_FRAME];
};

// --- CRC32 zlib (штифтик 2) -------------------------------------------------
inline uint32_t crc32z(const char* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint8_t)data[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

// --- Разбор кадра -----------------------------------------------------------
// false — битый кадр (нет поля, CRC не сошёлся, записей > потолка).
inline bool parseFrame(const char* p, Frame& f) {
    if (p == nullptr) return false;
    const char* a;
    a = strstr(p, "\"seq\":");   if (!a) return false; f.seq   = (uint16_t)atoi(a + 6);
    a = strstr(p, "\"total\":"); if (!a) return false; f.total = (uint16_t)atoi(a + 8);
    if (f.total == 0) { f.nrec = 0; f.ch[0] = '\0'; f.day = 0; f.crc = 0;
                        return true; }                 // пустой архив (штифтик 4)
    a = strstr(p, "\"ch\":\"");  if (!a) return false;
    a += 6;
    size_t cl = 0;
    while (a[cl] && a[cl] != '"' && cl < sizeof(f.ch) - 1) cl++;
    if (a[cl] != '"' || cl == 0) return false;
    memcpy(f.ch, a, cl); f.ch[cl] = '\0';
    a = strstr(p, "\"day\":");   if (!a) return false; f.day   = atoi(a + 6);
    a = strstr(p, "\"crc\":");   if (!a) return false; f.crc   = (uint32_t)strtoul(a + 6, nullptr, 10);
    // Поле r: от '[' до парного ']' (внутри — вложенные массивы записей)
    a = strstr(p, "\"r\":[");
    if (!a) return false;
    const char* rs = a + 4;                  // на '['
    int depth = 0; const char* re = rs;
    for (; *re; re++) {
        if (*re == '[') depth++;
        else if (*re == ']') { depth--; if (depth == 0) break; }
    }
    if (*re != ']') return false;
    if (crc32z(rs, (size_t)(re - rs + 1)) != f.crc) return false;  // штифтик 2
    // Записи [ts,mn,mx,avg]
    f.nrec = 0;
    const char* q = rs + 1;
    while (q < re) {
        if (*q != '[') { q++; continue; }
        q++;
        if (f.nrec >= MAX_REC_PER_FRAME) return false;
        DlogAggr& r = f.recs[f.nrec];
        r.ts  = (uint32_t)strtoul(q, (char**)&q, 10);  if (*q != ',') return false; q++;
        r.mn  = strtof(q, (char**)&q);                 if (*q != ',') return false; q++;
        r.mx  = strtof(q, (char**)&q);                 if (*q != ',') return false; q++;
        r.avg = strtof(q, (char**)&q);
        while (q < re && *q != ']') q++;
        if (q >= re) return false;
        q++;                               // за ']'
        f.nrec++;
    }
    return f.nrec > 0;
}

// --- ОБРАЗ ВОССТАНОВЛЕНИЯ (RAM, однократно на boot) ---------------------------
struct Image {
    struct Ch {
        char     id[12];
        uint16_t n;
        DlogAggr recs[MAX_REC_PER_CH];
    } chs[MAX_CH];
    uint8_t  nch = 0;
    uint16_t total = 0;                    // ожидаемое число кадров
    uint8_t  got[MAX_FRAMES / 8];          // битовая карта принятых seq
    uint16_t gotCount = 0;

    void reset() {
        nch = 0; total = 0; gotCount = 0;
        memset(got, 0, sizeof(got));
    }
    bool gotSeq(uint16_t s) const {
        return s < MAX_FRAMES && (got[s / 8] >> (s % 8)) & 1;
    }
    Ch* findCh(const char* id) {
        for (uint8_t i = 0; i < nch; i++)
            if (strncmp(chs[i].id, id, sizeof(chs[i].id)) == 0) return &chs[i];
        return nullptr;
    }
    // Дубликат кадра — ок (ретрай мастера); новый канал — регистрируем.
    // false — кадр вне набора (сменился total посреди сессии и т.п.)
    bool addFrame(const Frame& f) {
        if (f.total == 0) { total = 0; return true; }  // пустой архив
        if (total != 0 && f.total != total) return false;
        total = f.total;
        if (f.seq >= f.total || f.seq >= MAX_FRAMES) return false;
        if (gotSeq(f.seq)) return true;                // дубликат
        Ch* c = findCh(f.ch);
        if (c == nullptr) {
            if (nch >= MAX_CH) return false;
            c = &chs[nch++];
            memset(c, 0, sizeof(*c));
            strncpy(c->id, f.ch, sizeof(c->id) - 1);
        }
        for (uint8_t i = 0; i < f.nrec; i++) {
            if (c->n >= MAX_REC_PER_CH) break;
            // идемпотентность по ts (контракт): ts-дубликат — пропуск
            bool dup = false;
            for (uint16_t k = 0; k < c->n; k++)
                if (c->recs[k].ts == f.recs[i].ts) { dup = true; break; }
            if (!dup) c->recs[c->n++] = f.recs[i];
        }
        got[f.seq / 8] |= (uint8_t)(1u << (f.seq % 8));
        gotCount++;
        return true;
    }
    bool complete() const { return total > 0 && gotCount >= total; }
};

// --- МЕРДЖ: локальное новее архива побеждает (контракт) ---------------------
// recs канала сортируются по ts; локальные записи замещают архивные с тем
// же ts и дописываются в хвост. Возврат — итоговое число записей.
inline uint16_t mergeLocal(Image::Ch& c, const DlogAggr* local, uint16_t ln) {
    // сортировка вставками по ts (записей ≤ 744, раз в прошивку — норм)
    for (uint16_t i = 1; i < c.n; i++) {
        DlogAggr v = c.recs[i]; int16_t j = (int16_t)i - 1;
        while (j >= 0 && c.recs[j].ts > v.ts) { c.recs[j + 1] = c.recs[j]; j--; }
        c.recs[j + 1] = v;
    }
    for (uint16_t i = 0; i < ln && c.n < MAX_REC_PER_CH; i++) {
        bool replaced = false;
        for (uint16_t k = 0; k < c.n; k++)
            if (c.recs[k].ts == local[i].ts) { c.recs[k] = local[i]; replaced = true; break; }
        if (!replaced) {
            // вставка по возрастанию ts
            uint16_t pos = c.n;
            while (pos > 0 && c.recs[pos - 1].ts > local[i].ts) {
                if (pos < MAX_REC_PER_CH) c.recs[pos] = c.recs[pos - 1];
                pos--;
            }
            c.recs[pos] = local[i];
            if (c.n < MAX_REC_PER_CH) c.n++;
        }
    }
    return c.n;
}

} // namespace wgar
