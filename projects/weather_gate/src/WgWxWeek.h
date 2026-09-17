// ============================================================================
// WgWxWeek.h — НЕДЕЛЬНЫЙ ПРОГНОЗ OPEN-METEO -> HA (0.9.0, W6)
// ----------------------------------------------------------------------------
// Решение владельца: по сети — только сырые данные (числа и ASCII-токены
// HA), «человеческий язык» восстанавливается на стороне HA (карточка с
// Jinja-словарями, без configuration.yaml). Поэтому здесь — ни одного
// русского байта: даты ISO, HA condition-коды, температуры, осадки.
//
// Парсинг — урок №25: strstr ТОЛЬКО с якорем на секцию данных "daily":{,
// имена полей встречаются и в daily_units. Каждый массив ищется от якоря,
// значения вычленяются посимвольно (atof/atoi двигаются по запятым).
//
// Чистая логика, host-тестируемая. Ядро не трогаем, конфиг не меняется.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WgWxCode.h"

namespace wgw {

static const uint8_t MAX_DAYS = 7;

struct Day {
    char     d[11];    // "2026-09-18" (ISO, 10 chars + NUL)
    int      wmo;      // WMO weather_code дня (−1 — нет данных)
    float    tmax;     // °C
    float    tmin;     // °C
    float    pr;       // мм осадков за сутки
};

// Разбор секции "daily":{...} ответа open-meteo. Возвращает число дней
// (0..MAX_DAYS). body — полный ответ; якорь "daily":{ обязателен.
inline uint8_t parseDaily(const char* body, Day* out, uint8_t maxDays) {
    if (body == nullptr || out == nullptr || maxDays == 0) return 0;
    if (maxDays > MAX_DAYS) maxDays = MAX_DAYS;
    const char* sec = strstr(body, "\"daily\":{");
    if (sec == nullptr) return 0;

    const char* pTime = strstr(sec, "\"time\":[");
    const char* pCode = strstr(sec, "\"weather_code\":[");
    const char* pTmax = strstr(sec, "\"temperature_2m_max\":[");
    const char* pTmin = strstr(sec, "\"temperature_2m_min\":[");
    const char* pPr   = strstr(sec, "\"precipitation_sum\":[");
    if (pTime == nullptr || pCode == nullptr ||
        pTmax == nullptr || pTmin == nullptr || pPr == nullptr) return 0;

    // Границы каждого массива — ']': за её пределы не шагаем (иначе
    // последний элемент без запятой утащит указатель в следующее поле).
    const char* eTime = strchr(pTime, ']');
    const char* eCode = strchr(pCode, ']');
    const char* eTmax = strchr(pTmax, ']');
    const char* eTmin = strchr(pTmin, ']');
    const char* ePr   = strchr(pPr, ']');
    if (eTime == nullptr || eCode == nullptr || eTmax == nullptr ||
        eTmin == nullptr || ePr == nullptr) return 0;

    uint8_t n = 0;
    // Смещения — ЗА '[': длина маркера с кавычками, двоеточием и скобкой
    // ("time":[ = 8, "weather_code":[ = 16, "temperature_2m_max/min":[ = 22,
    // "precipitation_sum":[ = 21).
    const char *t = pTime + 8, *c = pCode + 16, *x = pTmax + 22,
               *m = pTmin + 22, *p = pPr + 21;
    while (n < maxDays) {
        // Дата: "YYYY-MM-DD" в кавычках, строго внутри массива time
        const char* q = strchr(t, '\"');
        if (q == nullptr || q >= eTime) break;
        if (strncmp(q + 1, "20", 2) != 0) break;   // конец массива/мусор
        memcpy(out[n].d, q + 1, 10);
        out[n].d[10] = '\0';
        t = q + 12;   // за закрывающую кавычку
        // Числа: atof/atoi с текущей позиции (все указатели внутри своих
        // массивов — проверено выше и на прошлых итерациях)
        if (c >= eCode || x >= eTmax || m >= eTmin || p >= ePr) break;
        out[n].wmo = atoi(c);
        out[n].tmax = (float)atof(x);
        out[n].tmin = (float)atof(m);
        out[n].pr   = (float)atof(p);
        n++;
        // Следующий элемент — за запятой, но не за пределами массива
        const char* nc = strchr(c, ','); if (nc == nullptr || nc >= eCode) break;
        const char* nx = strchr(x, ','); if (nx == nullptr || nx >= eTmax) break;
        const char* nm = strchr(m, ','); if (nm == nullptr || nm >= eTmin) break;
        const char* np = strchr(p, ','); if (np == nullptr || np >= ePr) break;
        c = nc + 1; x = nx + 1; m = nm + 1; p = np + 1;
    }
    return n;
}

// Сборка weekly-JSON: {"days":[{"d":"...","cond":"rainy","tmax":18.1,
// "tmin":9.2,"pr":1.2},...]}. Возвращает длину (0 — не собралось).
// Бюджет: 7 дней ≈ 450 Б (publishRaw онлайн без капа; офлайн-outbox 256
// честно дропнет со счётчиком — без сети прогноз всё равно не обновить).
inline size_t weeklyJson(const Day* days, uint8_t n, char* buf, size_t cap) {
    if (days == nullptr || buf == nullptr || n == 0 || cap < 64) return 0;
    int len = snprintf(buf, cap, "{\"days\":[");
    for (uint8_t i = 0; i < n && len > 0 && (size_t)len < cap; i++) {
        len += snprintf(buf + len, cap - (size_t)len,
            "%s{\"d\":\"%s\",\"cond\":\"%s\",\"tmax\":%.1f,\"tmin\":%.1f,"
            "\"pr\":%.1f}",
            i ? "," : "", days[i].d, wgs::wmoToHaCond(days[i].wmo),
            (double)days[i].tmax, (double)days[i].tmin, (double)days[i].pr);
    }
    if (len > 0 && (size_t)len < cap)
        len += snprintf(buf + len, cap - (size_t)len, "]}");
    return (len > 0 && (size_t)len < cap) ? (size_t)len : 0;
}

} // namespace wgw
