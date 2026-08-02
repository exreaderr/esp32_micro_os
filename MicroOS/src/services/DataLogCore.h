// ============================================================================
// DataLogCore.h — ДАТАЛОГГЕР: ЧИСТАЯ ЛОГИКА (D2: host-тесты, без Arduino)
// ============================================================================
// Наследник идей DataLoggerManager мёртвой ветки v4.2.2 (дельта-хранение,
// ярусы агрегации, JSON для uPlot, автоочистка), переложенный на механику
// 5.0. От ветки отказались осознанно:
//   · std::vector/std::map/String/рекурсивные мьютексы -> фиксированные
//     бюджеты BSS (урок фрагментации heap);
//   · 30 сенсоров -> 8 каналов; 6 ярусов (raw..month) -> 3 (raw/hour/day);
//   · события на каждый чих -> тихое ядро, диагностика счётчиками.
//
// Ярусы хранения (на канал):
//   RAW  — кольцо в RAM, 1 точка/мин (но приём нерегулярный тоже честен),
//          360 точек = 6 ч. Переживает ли ребут? Нет — и не должно:
//          краткосрочная история, дешёвая.
//   HOUR — append-only файл в LittleFS: {ts, min, max, avg} за каждый час,
//          744 записи = 31 сутки. Переполнение -> компакция (сброс старейших
//          25%): перезапись файла ~раз в месяц, flash не трём.
//   DAY  — то же по суткам, 366 записей = год. Компакция ~раз в год.
//
// Всё здесь — чистая математика над POD: тестируется на хосте (tests.cpp).
// Файловый ввод-вывод, мьютексы, RTC — в DataLogService (обёртка).
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// --- БЮДЖЕТЫ ------------------------------------------------------------------
constexpr uint8_t  DLOG_MAX_CHANNELS = 8;
constexpr uint16_t DLOG_RAW_CAP      = 360;   // точек в RAM-кольце (6 ч @ 1/мин)
constexpr uint16_t DLOG_HOUR_CAP     = 744;   // почасовых записей (31 сутки)
constexpr uint16_t DLOG_DAY_CAP      = 366;   // посуточных записей (год)
constexpr uint16_t DLOG_JSON_POINTS  = 240;   // потолок точек в ответе API
                                              // (буфер ядра 8 КБ)

// --- ТОЧКИ --------------------------------------------------------------------
struct DlogPoint {                  // сырая точка (RAM), 8 байт
    uint32_t ts;                    // unix
    float    v;
};

struct DlogAggr {                   // агрегат яруса (файл), 16 байт
    uint32_t ts;                    // начало периода (час/сутки)
    float    mn;
    float    mx;
    float    avg;
};

// Заголовок append-only файла яруса. count растёт до cap, дальше компакция.
struct DlogFileHeader {
    uint32_t magic;                 // DLOG_FILE_MAGIC
    uint16_t count;                 // записей в файле
    uint16_t cap;                   // потолок (DLOG_HOUR_CAP / DLOG_DAY_CAP)
    uint32_t reserved[2];
};
constexpr uint32_t DLOG_FILE_MAGIC = 0x31474C44UL;   // 'DLG1' (LE)

namespace dlog {

// ============================================================================
// КОЛЬЦО СЫРЫХ ТОЧЕК (RAM)
// ============================================================================
struct Ring {
    DlogPoint pts[DLOG_RAW_CAP];
    uint16_t  head  = 0;            // куда пишем следующую
    uint16_t  count = 0;            // сколько валидных (<= cap)

    void reset() { head = 0; count = 0; }

    void push(uint32_t ts, float v) {
        pts[head].ts = ts;
        pts[head].v  = v;
        head = (uint16_t)((head + 1) % DLOG_RAW_CAP);
        if (count < DLOG_RAW_CAP) ++count;
    }

    /// Снимок в хронологическом порядке (старые -> новые), только ts >= fromTs
    /// (0 — всё). Возвращает число скопированных.
    uint16_t snapshot(DlogPoint* out, uint16_t maxN, uint32_t fromTs) const {
        uint16_t n = 0;
        for (uint16_t i = 0; i < count && n < maxN; ++i) {
            // старейшая = (head - count + i) mod cap
            uint16_t idx = (uint16_t)((head + DLOG_RAW_CAP - count + i)
                                      % DLOG_RAW_CAP);
            if (fromTs != 0 && pts[idx].ts < fromTs) continue;
            out[n++] = pts[idx];
        }
        return n;
    }
};

// ============================================================================
// НАКОПИТЕЛЬ АГРЕГАТА (часовое/суточное ведро)
// ============================================================================
// periodSec = 3600 (час) или 86400 (сутки). Ведро открыто, пока точки
// попадают в тот же период; первая точка нового периода закрывает ведро
// (roll) и открывает следующее. Пропуски периодов (устройство спало) —
// честно: пустых записей в файле нет, график покажет разрыв.
struct Bucket {
    uint32_t periodStart = 0;       // 0 — ведро пусто
    float    mn  = 0.0f;
    float    mx  = 0.0f;
    float    sum = 0.0f;
    uint32_t n   = 0;

    /// Период, которому принадлежит ts.
    static uint32_t periodOf(uint32_t ts, uint32_t periodSec) {
        return ts - (ts % periodSec);
    }

    /// Принять точку. Если ведро перекрылось — сначала вернуть готовый
    /// агрегат через rolled (true) и открыть новое ведро.
    bool add(uint32_t ts, float v, uint32_t periodSec, DlogAggr& rolled) {
        uint32_t p = periodOf(ts, periodSec);
        bool hasRolled = false;
        if (n > 0 && p != periodStart) {
            rolled.ts  = periodStart;
            rolled.mn  = mn;
            rolled.mx  = mx;
            rolled.avg = sum / (float)n;
            hasRolled = true;
            n = 0;                       // ведро закрыто — открываем новое
        }
        if (n == 0) {
            periodStart = p;
            mn = mx = v;
            sum = v;
            n = 1;
        } else {
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
            ++n;
        }
        return hasRolled;
    }

    /// Принудительно закрыть ведро (ребут/останов): false — ведро пустое.
    bool flush(DlogAggr& out) {
        if (n == 0) return false;
        out.ts  = periodStart;
        out.mn  = mn;
        out.mx  = mx;
        out.avg = sum / (float)n;
        n = 0;
        return true;
    }
};

// ============================================================================
// ДЕЦИМАЦИЯ (влезть в DLOG_JSON_POINTS без потери формы графика)
// ============================================================================
// RAW: прореживание шагом stride (первая/последняя точки сохраняются —
// края графика честные).
inline uint16_t decimateRaw(const DlogPoint* in, uint16_t n,
                            DlogPoint* out, uint16_t maxN) {
    if (n <= maxN) {
        if (out != in) memcpy(out, in, n * sizeof(DlogPoint));
        return n;
    }
    uint16_t stride = (uint16_t)((n + maxN - 1) / maxN);
    uint16_t o = 0;
    for (uint16_t i = 0; i < n && o < maxN; i = (uint16_t)(i + stride)) {
        out[o++] = in[i];
    }
    if (o > 0 && o < maxN && in[n - 1].ts != out[o - 1].ts) {
        out[o++] = in[n - 1];          // последняя точка обязательна
    }
    return o;
}

// АГРЕГАТЫ: сливаем группы по k соседних записей — min из min, max из max,
// avg средним (точность второго порядка, для графика честно).
inline uint16_t decimateAggr(const DlogAggr* in, uint16_t n,
                             DlogAggr* out, uint16_t maxN) {
    if (n <= maxN) {
        if (out != in) memcpy(out, in, n * sizeof(DlogAggr));
        return n;
    }
    uint16_t stride = (uint16_t)((n + maxN - 1) / maxN);
    uint16_t o = 0;
    for (uint16_t i = 0; i < n && o < maxN; i = (uint16_t)(i + stride)) {
        DlogAggr a = in[i];
        uint16_t end = (uint16_t)(i + stride);
        if (end > n) end = n;
        for (uint16_t j = (uint16_t)(i + 1); j < end; ++j) {
            if (in[j].mn < a.mn) a.mn = in[j].mn;
            if (in[j].mx > a.mx) a.mx = in[j].mx;
            a.avg = (a.avg + in[j].avg) * 0.5f;
        }
        out[o++] = a;
    }
    return o;
}

} // namespace dlog
