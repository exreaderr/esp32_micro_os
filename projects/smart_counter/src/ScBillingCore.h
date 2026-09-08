// ============================================================================
// ScBillingCore.h — БИЛЛИНГ И ФИЛЬТР BIG BLUE: ЧИСТАЯ ЛОГИКА (C1)
// ============================================================================
// СТАТУС: C1. Тег v5.8.4 выпущен, блокировка снята (Issue #3).
// Host-тесты: ../host/tests.cpp.
//
// Происхождение: monthly = total − base, bill = monthly × price, граница
// по report_day, якорь фильтра — из продакшн-монолита v4.4.2. Переносим
// КАК ЕСТЬ по смыслу; исправляем два известных дефекта монолита (уроки 1–2).
//
// Уроки, зашитые в этот заголовок:
//  1. Монолит хранил lastResetDay в RAM-static: ребут 24-го числа после
//     сброса давал ПОВТОРНЫЙ сброс базы и обнуление честно набранного
//     месячного расхода. Здесь «последний сброс» — YYYYMMDD (uint32),
//     живёт в NVS-поле профиля (sc.bill.last_ymd(R)) через ConfigService;
//     повторный сброс в тот же день исключён сравнением дат.
//  2. Догоняющий сброс (catch-up): если устройство было обесточено
//     ровно через отчётный день (24-е пропало вместе со светом), монолит
//     сбрасывал базу при первом же включении ПОСЛЕ — и терял расход за
//     «слепые» дни в месячной квитанции не корректнее, а иначе: здесь
//     сброс делается ОТ ПОКАЗАНИЙ, ЗАФИКСИРОВАННЫХ в последнем AC-loss
//     снимке (см. концепт-ноту §5, ярус 3): базой становится значение
//     «на момент пропадания питания», а не «на момент включения».
//     Функция catchupBase() отдаёт именно это значение.
//  3. report_day валидируется 1..28 (схема конфига) — февраль не ломает
//     биллинг; 29–31 монолитом молча «терялись» в коротких месяцах.
//  4. Деньги: считаем в копейках/сотых кВт·ч там, где можно (см. урок 5
//     в ScMercuryCore.h); float — только для показа в UI.
// ============================================================================
#pragma once

#include <cstdint>

namespace scb {

// --- МЕСЯЧНЫЙ УЧЁТ -------------------------------------------------------------------
/// Месячный расход и сумма. Отрицательный monthly (базу подняли руками
/// выше текущих показаний) — зажимаем в 0, как монолит: отрицательный
/// счёт к оплате абсурден и путает UI.
struct Bill { float monthly; float amount; };
inline Bill computeBill(float total, float base, float price) {
    Bill b;
    b.monthly = total - base;
    if (b.monthly < 0.0f) b.monthly = 0.0f;
    b.amount = b.monthly * price;
    return b;
}

// --- ОТЧЁТНЫЙ ДЕНЬ ---------------------------------------------------------------------
/// YYYYMMDD одним числом — сравнивается обычным неравенством (урок 1).
inline uint32_t ymd(int year, int month, int day) {
    return (uint32_t)(year * 10000 + month * 100 + day);
}

/// Пора ли фиксировать базу отчётного периода?
/// cur* — ЛОКАЛЬНАЯ дата из TimeService (gmtime_r(unix + tz*3600), RTC=UTC);
/// reportDay валидирован 1..28 схемой; lastYmd==0 — сброс ещё ни разу не
/// выполнялся (первый запуск -> фиксируем базу сразу, это «день установки»).
/// Срабатывает ровно один раз на дату: lastYmd != сегодня (урок 1).
inline bool isReportDue(int curYear, int curMonth, int curDay,
                        int reportDay, uint32_t lastYmd) {
    if (lastYmd == 0) return true;
    uint32_t today = ymd(curYear, curMonth, curDay);
    if (today == lastYmd) return false;                    // сегодня уже сбрасывали
    if (curDay == reportDay) return true;                  // штатный отчётный день
    // catch-up (урок 2): отчётный день прошёл, пока устройство молчало —
    // последний сброс старше, а в текущем месяце reportDay уже миновал.
    if (curDay > reportDay && lastYmd < ymd(curYear, curMonth, reportDay)) return true;
    return false;
}

/// Каким значением фиксировать базу при catch-up: показания из AC-loss
/// снимка («что было, когда свет пропал»), если он валиден и не новее
/// текущих показаний; иначе — текущие (урок 2).
inline float catchupBase(float currentTotal, float snapshotTotal, bool snapshotValid) {
    if (snapshotValid && snapshotTotal <= currentTotal) return snapshotTotal;
    return currentTotal;
}

// --- ФИЛЬТР BIG BLUE 20" ------------------------------------------------------------------
/// Расход через фильтр, литры: (water_total_m3 − base_m3) × 1000 (монолит).
inline float filterUsageL(float waterTotalM3, float filterBaseM3) {
    float u = (waterTotalM3 - filterBaseM3) * 1000.0f;
    return u > 0.0f ? u : 0.0f;   // базу подняли выше total — износа «в минус» нет
}
inline float filterRemainingL(float waterTotalM3, float filterBaseM3, float limitL) {
    float rem = limitL - filterUsageL(waterTotalM3, filterBaseM3);
    return rem > 0.0f ? rem : 0.0f;
}

/// Переходы состояния «заменить картридж»: 0 — без изменений,
/// 1 — ресурс исчерпан (событие SC_EVENT_FILTER_REPLACE), 2 — заменили,
/// тревога снята (новый картридж -> resetFilter снаружи + здесь флаг вниз).
inline int filterWarningStep(bool warningActive, float remainingL) {
    if (remainingL <= 0.0f && !warningActive) return 1;
    if (remainingL > 0.0f  &&  warningActive) return 2;
    return 0;
}

} // namespace scb
