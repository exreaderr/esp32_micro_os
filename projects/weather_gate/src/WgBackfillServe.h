// ============================================================================
// WgBackfillServe.h — РАЗДАЧА СУТОК ИЗ ПОЧАСОВОГО ЯРУСА МАСТЕРУ (0.9.7, W8)
// ----------------------------------------------------------------------------
// Контракт: дизайн-нота W8 + ответ ветки 04.10.2026 («мастер готов»).
// Направление ОБРАТНОЕ W7: мастер нашёл дыру в w7a → bfreq {"id","from",
// "to"} (окно = одни сутки UTC, to искл.) → шлюз отдаёт кадры W7 на
// …/bfresp/<gw_id>. Формула кадров дословно W7: days=1 → total=nch×6
// (30 при пяти каналах), seq = ch×6 + part, кадр = канал-4-часа (≤4
// почасовых записи), CRC32 zlib по сырой строке "r" от '[' до ']'.
// Пустые кадры r:[] ЛЕГАЛЬНЫ (часа нет и у шлюза — окно закрывается
// пустым кадром). Совсем пустой снимок → один ответ {"seq":0,"total":0}.
//
// Пейсинг — урок W7 в обратную сторону: приёмник (мастер) на том же
// однослотовом mailbox ядра → кадр не чаще раза в 500 мс по millis()
// (пейсер в WeatherGateApp, здесь только чистая логика — host-тесты).
// Антидребезг bfreq 120 с (утверждено веткой 04.10) — тоже в App.
//
// Снимок суток делается ОДИН РАЗ на запрос (getTier по каналу, фильтр
// [from,to)) — дальше раздача идёт из RAM, ФС/MQTT снаружи. ~2 КБ,
// статика App — heap-образ не нужен (урок №26 учтён конструктивно).
// ============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <services/DataLogCore.h>
#include "WgArchiveRestore.h"   // wgar::crc32z — общий CRC флота W7/W8

namespace wgbf {

constexpr uint8_t  MAX_CH        = 5;    // пять каналов погоды
constexpr uint8_t  RECS_PER_DAY  = 24;   // почасовых записей в сутках
constexpr uint8_t  PARTS_PER_CH  = 6;    // 4-часовых окон в сутках
constexpr uint32_t PART_SEC      = 14400UL;   // 4 ч
constexpr uint32_t DAY_SEC       = 86400UL;
constexpr uint32_t REQ_DEBOUNCE_SEC = 120UL;  // антидребезг bfreq (контракт)
constexpr uint32_t PACE_MS       = 500UL;     // пейсинг кадров (урок W7)
constexpr uint16_t FRAME_CAP     = 240;       // потолок кадра, байт

// --- СНИМОК СУТОК (RAM, ~2 КБ, статика) --------------------------------------
struct ChSnap {
    char     id[12];
    uint8_t  n;                       // записей в окне [from, to)
    DlogAggr recs[RECS_PER_DAY];
};

struct DaySnap {
    uint32_t from = 0;                // окно [from, to), from кратно суткам UTC
    uint32_t to   = 0;
    uint8_t  nch  = 0;
    ChSnap   chs[MAX_CH];

    uint16_t totalRecs() const {
        uint16_t s = 0;
        for (uint8_t c = 0; c < nch; c++) s += chs[c].n;
        return s;
    }
    uint16_t totalFrames() const { return (uint16_t)(nch * PARTS_PER_CH); }
};

// --- Разбор bfreq -------------------------------------------------------------
// {"id":"<gw>","from":<ts>,"to":<ts>} — якоря-имена с кавычками (урок №25).
// Валидация: to > from, окно ≤ суток, from кратно суткам UTC (мастер
// нарезает очередь по суткам — чужое окно не обслуживаем, чтобы seq-раскладка
// оставалась контрактной).
inline bool parseBfreq(const char* p, uint32_t& from, uint32_t& to) {
    if (p == nullptr) return false;
    const char* a = strstr(p, "\"from\":");
    if (!a) return false;
    from = (uint32_t)strtoul(a + 7, nullptr, 10);
    a = strstr(p, "\"to\":");
    if (!a) return false;
    to = (uint32_t)strtoul(a + 5, nullptr, 10);
    if (to <= from) return false;
    if (to - from > DAY_SEC) return false;
    if (from % DAY_SEC != 0) return false;
    return true;
}

// --- Сериализация одной записи [ts,mn,mx,avg] ---------------------------------
// Формат float — %.2f: приёмник (мастер) разбирает strtof, байтовая
// симметрия с W7 не нужна — CRC считается по НАШЕЙ строке и ей же
// проверяется на той стороне.
inline size_t recJson(const DlogAggr& r, char* out, size_t cap) {
    int n = snprintf(out, cap, "[%lu,%.2f,%.2f,%.2f]",
                     (unsigned long)r.ts, (double)r.mn,
                     (double)r.mx, (double)r.avg);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// --- Кадр seq → JSON -----------------------------------------------------------
// seq = ch×6 + part; part накрывает [from + part×4ч, +4ч). Записи снимка
// хронологичны (getTier), фильтр по окну part-а. Пустое окно → r:[] —
// ЛЕГАЛЬНЫЙ кадр (закрывает окно на мастере).
// Возврат: длина строки (0 — seq вне снимка / не влезло в cap).
inline uint16_t frameJson(const DaySnap& ds, uint16_t seq,
                          char* out, size_t cap) {
    if (ds.nch == 0 || seq >= ds.totalFrames()) return 0;
    uint8_t ci   = (uint8_t)(seq / PARTS_PER_CH);
    uint8_t part = (uint8_t)(seq % PARTS_PER_CH);
    const ChSnap& ch = ds.chs[ci];
    uint32_t w0 = ds.from + (uint32_t)part * PART_SEC;
    uint32_t w1 = w0 + PART_SEC;
    // Собираем r сначала в локальный буфер: CRC идёт по сырым байтам r.
    char rbuf[160];                     // 4 записи × ~38 Б + скобки
    size_t rl = 0;
    rbuf[rl++] = '[';
    uint8_t put = 0;
    for (uint8_t i = 0; i < ch.n && put < 4; i++) {
        const DlogAggr& r = ch.recs[i];
        if (r.ts < w0) continue;
        if (r.ts >= w1) break;          // хронология — дальше только позже
        if (put > 0) rbuf[rl++] = ',';
        size_t n = recJson(r, rbuf + rl, sizeof(rbuf) - rl);
        if (n == 0) return 0;
        rl += n;
        put++;
    }
    rbuf[rl++] = ']';
    rbuf[rl] = '\0';
    uint32_t crc = wgar::crc32z(rbuf, rl);
    int n = snprintf(out, cap,
                     "{\"seq\":%u,\"total\":%u,\"ch\":\"%s\",\"day\":0,"
                     "\"crc\":%lu,\"r\":%s}",
                     (unsigned)seq, (unsigned)ds.totalFrames(), ch.id,
                     (unsigned long)crc, rbuf);
    return (n > 0 && (size_t)n < cap) ? (uint16_t)n : 0;
}

} // namespace wgbf
