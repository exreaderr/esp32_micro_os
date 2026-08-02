// ============================================================================
// TimeInterval.h — ИНТЕРВАЛЫ ВРЕМЕНИ HH:MM (чистая логика, D2: host-тесты)
// ============================================================================
// Выделено из TimeService по требованию D2: сравнение "внутри ли интервала"
// с переходом через полночь — чистая функция без системного времени.
// Используется расписаниями (ночной запрет кнопки выхода СКУД и т.п.).
// ============================================================================
#pragma once

namespace sh_time {

/// nowMin внутри [startMin, endMin)? Все величины — минуты от полуночи.
/// start <= end — дневной интервал (09:00–18:00);
/// start >  end — ночной через полночь (22:00–06:00).
/// Граница: start включается, end — нет (полуоткрытый интервал).
inline bool minutesInInterval(int nowMin, int startMin, int endMin) {
    if (startMin <= endMin) {
        return nowMin >= startMin && nowMin < endMin;
    }
    return nowMin >= startMin || nowMin < endMin;
}

// --- Гражданская дата -> unix UTC (чистая, без libc/TZ) --------------------
// Зачем: RTC (DS3231) хранит UTC-wall, а mktime/timegm зависят от TZ-
// окружения libc (урок 5.0.x: TZ не выставлено — расписание кнопки выхода
// «плавало» на смещение пояса, пользователь видел инверсию запрета).
// Алгоритм days_from_civil (H. Hinnant, публичный домен) — детерминирован
// на ESP32 и на host (D2: сверка с timegm в host-тестах).
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    const int      mp  = ((int)m + 9) % 12;                      // март = 0
    const unsigned doy = (unsigned)((153 * mp + 2) / 5) + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/// Unix-секунды UTC из гражданских компонентов (y — полный год, m 1..12).
inline int64_t secondsFromCivil(int y, unsigned m, unsigned d,
                                unsigned hh, unsigned mi, unsigned ss) {
    return daysFromCivil(y, m, d) * 86400 + (int64_t)hh * 3600 +
           (int64_t)mi * 60 + (int64_t)ss;
}

} // namespace sh_time
