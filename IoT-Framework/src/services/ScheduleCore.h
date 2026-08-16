// ============================================================================
// ScheduleCore.h — ВРЕМЕННЫЕ ПРАВИЛА: ЧИСТАЯ ЛОГИКА (D2: host-тесты)
// ============================================================================
// Доменный сервис «Scheduler» из базовой архитектуры (таблица модулей:
// «временные интервалы: ночной режим, рабочие часы, будильники, cron-
// подобные задачи. Подписчик TimeService»). Здесь — только математика над
// POD, без системного времени/конфига/шины: парсер строки правила, маски
// дней недели, принадлежность интервалу (делегат TimeInterval.h), детектор
// фронтов. Обёртка с жизненным циклом — ScheduleService.
//
// Формат правила (одна строка, поля через '|'):
//   имя|С|ПО|ДНИ|КОД
//     имя — до 19 символов, уезжает в payload событий ("ночь", "будни");
//     С   — "ЧЧ:ММ" начало (или момент точечного правила);
//     ПО  — "ЧЧ:ММ" конец; ПУСТО -> правило точечное (будильник/cron);
//     ДНИ — "*" (каждый день) или набор цифр 1..7 (пн..вс): "12345" —
//           будни, "67" — выходные;
//     КОД — 1..255, код периода, уезжает в code события; 0 зарезервирован
//           ядром как «вне периода».
// Примеры:
//   ночь|22:00|06:00|*|1        — ночной режим через полночь, ежедневно;
//   будни|09:00|18:00|12345|2   — рабочие часы по будням;
//   полив|06:30||1,3,5|4        — НЕТ: дни без запятых!  Правильно:
//   полив|06:30||135|4          — точечно в 06:30 по пн/ср/пт.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include "TimeInterval.h"   // sh_time::minutesInInterval (через полночь)

// --- БЮДЖЕТЫ ------------------------------------------------------------------
constexpr uint8_t SCHED_MAX_RULES     = 8;   // 4 из конфига + 4 от провайдера
constexpr uint8_t SCHED_CFG_RULES     = 4;   // правил из NVS (sched.rule0..3)
constexpr uint8_t SCHED_RULE_NAME_LEN = 20;  // с '\0'
constexpr uint8_t SCHED_RULE_STR_LEN  = 48;  // макс. длина строки правила
constexpr uint8_t SCHED_DAYS_ALL      = 0x7F; // бит0=пн ... бит6=вс

// --- ТИПЫ ---------------------------------------------------------------------
enum SchedRuleType : uint8_t {
    SCHED_RULE_INTERVAL = 0,  // период [С, ПО) с переходом через полночь
    SCHED_RULE_POINT    = 1,  // точечное срабатывание в момент С
};

struct SchedRule {
    char     name[SCHED_RULE_NAME_LEN]; // имя (payload событий)
    uint16_t startMin;                  // минуты от полуночи, 0..1439
    uint16_t endMin;                    // конец (у POINT равен startMin)
    uint8_t  dayMask;                   // SCHED_DAYS_*: бит0=пн ... бит6=вс
    uint8_t  periodCode;                // 1..255 -> code события
    uint8_t  type;                      // SchedRuleType
    bool     enabled;
};

// Детектор фронтов интервального правила.
enum SchedEdge : uint8_t {
    SCHED_EDGE_NONE = 0,
    SCHED_EDGE_ENTER,           // вступили в период
    SCHED_EDGE_EXIT,            // вышли из периода
};

namespace sched {

// --- РАЗБОР "ЧЧ:ММ" -------------------------------------------------------------
/// "22:00" -> 1320. Допускается одна цифра часа ("7:05"). false — мусор.
inline bool parseHHMM(const char* s, uint16_t& outMin) {
    if (!s) return false;
    int hh = -1, mm = -1, digits = 0;
    const char* p = s;
    while (*p >= '0' && *p <= '9') { ++p; ++digits; }
    if (digits < 1 || digits > 2 || *p != ':') return false;
    hh = atoi(s);
    const char* m = p + 1;
    int mdigits = 0;
    while (m[mdigits] >= '0' && m[mdigits] <= '9') ++mdigits;
    if (mdigits != 2 || m[mdigits] != '\0') return false;
    mm = atoi(m);
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    outMin = (uint16_t)(hh * 60 + mm);
    return true;
}

// --- ДНИ НЕДЕЛИ -----------------------------------------------------------------
/// tm_wday (0=воскресенье, соглашение struct tm) -> бит маски (бит0=пн).
inline uint8_t dayBit(int tmWday) {
    if (tmWday < 0 || tmWday > 6) return 0;
    // tm: 0=вс,1=пн..6=сб; маска: бит0=пн..бит5=сб,бит6=вс
    return (uint8_t)(1u << ((tmWday + 6) % 7));
}

/// Разбор поля ДНИ: "*" -> все; иначе цифры '1'..'7' (пн..вс) без
/// разделителей: "12345", "67". false — пусто/мусор/дубликаты/чужие символы.
inline bool parseDayMask(const char* s, uint8_t& outMask) {
    if (!s || !*s) return false;
    if (s[0] == '*' && s[1] == '\0') { outMask = SCHED_DAYS_ALL; return true; }
    uint8_t mask = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '1' || *p > '7') return false;
        uint8_t bit = (uint8_t)(1u << (*p - '1'));
        if (mask & bit) return false;   // "112" — дубликат, признак опечатки
        mask |= bit;
    }
    outMask = mask;
    return true;
}

// --- РАЗБОР СТРОКИ ПРАВИЛА ------------------------------------------------------
/// "имя|С|ПО|ДНИ|КОД" -> SchedRule. Пустое ПО -> SCHED_RULE_POINT.
/// false — строка пуста/полей не хватает/поле невалидно (правило
/// отбрасывается целиком, половинчатых состояний нет).
inline bool parseRule(const char* str, SchedRule& out) {
    if (!str) return false;
    // Пропуск ведущих пробелов — пользователь редактирует руками.
    while (*str == ' ') ++str;
    if (!*str) return false;

    char buf[SCHED_RULE_STR_LEN];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (strlen(str) >= sizeof(buf)) return false;   // обрезанное — не правило

    // Разрезаем по '|' на 5 полей (strtok_r нет в shim — руками).
    char* fields[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    uint8_t n = 0;
    fields[n++] = buf;
    for (char* p = buf; *p; ++p) {
        if (*p == '|') {
            if (n >= 5) return false;   // полей больше пяти
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    if (n != 5) return false;           // полей меньше пяти

    // Имя: непустое, обрезаем хвостовые пробелы, не длиннее буфера.
    char* name = fields[0];
    size_t nl = strlen(name);
    while (nl > 0 && name[nl - 1] == ' ') name[--nl] = '\0';
    if (nl == 0 || nl >= SCHED_RULE_NAME_LEN) return false;
    memcpy(out.name, name, nl + 1);

    // Времена.
    if (!parseHHMM(fields[1], out.startMin)) return false;
    if (fields[2][0] == '\0') {               // пустое ПО — точечное правило
        out.type = SCHED_RULE_POINT;
        out.endMin = out.startMin;
    } else {
        if (!parseHHMM(fields[2], out.endMin)) return false;
        if (out.endMin == out.startMin) return false;  // вырожденный период
        out.type = SCHED_RULE_INTERVAL;
    }

    // Дни.
    if (!parseDayMask(fields[3], out.dayMask)) return false;

    // Код: 1..255, строго число.
    const char* c = fields[4];
    if (!*c) return false;
    for (const char* q = c; *q; ++q)
        if (*q < '0' || *q > '9') return false;
    long code = atol(c);
    if (code < 1 || code > 255) return false;
    out.periodCode = (uint8_t)code;

    out.enabled = true;
    return true;
}

// --- ВЫЧИСЛЕНИЕ -----------------------------------------------------------------
/// Интервальное правило активно в момент (nowMin, tmWday)?
/// Интервал [start, end) считается по дню НАЧАЛА: ночь 22:00–06:00,
/// начавшаяся в понедельник, в 05:00 вторника ещё активна — для этого
/// после полуночи проверяется маска ВЧЕРАШНЕГО дня (правило монолита
/// «запрет 22–06 действует до утра»).
inline bool activeAt(const SchedRule& r, int nowMin, int tmWday) {
    if (!r.enabled) return false;
    const int start = (int)r.startMin, end = (int)r.endMin;
    if (start <= end) {   // дневной интервал — маска сегодняшнего дня
        if (!(dayBit(tmWday) & r.dayMask)) return false;
        return sh_time::minutesInInterval(nowMin, start, end);
    }
    // Ночной интервал через полночь: до полуночи — маска сегодня,
    // после — маска вчера (интервал открыли вчера вечером).
    if (nowMin >= start) {
        return (dayBit(tmWday) & r.dayMask) != 0;
    }
    if (nowMin < end) {
        return (dayBit((tmWday + 6) % 7) & r.dayMask) != 0;
    }
    return false;
}

/// Точечное правило приходится на минуту (nowMin, tmWday)?
inline bool pointDue(const SchedRule& r, int nowMin, int tmWday) {
    if (!r.enabled || r.type != SCHED_RULE_POINT) return false;
    return nowMin == (int)r.startMin && (dayBit(tmWday) & r.dayMask) != 0;
}

/// Фронт интервального правила между прежним состоянием и текущим моментом.
/// nowActive — выходное текущее состояние (для кэша вызывающего).
inline SchedEdge intervalEdge(bool wasActive, const SchedRule& r,
                              int nowMin, int tmWday, bool& nowActive) {
    nowActive = (r.type == SCHED_RULE_INTERVAL) && activeAt(r, nowMin, tmWday);
    if (nowActive == wasActive) return SCHED_EDGE_NONE;
    return nowActive ? SCHED_EDGE_ENTER : SCHED_EDGE_EXIT;
}

} // namespace sched
