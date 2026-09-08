// ============================================================================
// WgZambretti.h — ПРОГНОЗ ЗАМБРЕТТИ (W5, чистая логика, host-тестируемая)
// ----------------------------------------------------------------------------
// Таблица портирована 1:1 из монолита weather_gate v5.2
// (calculateZambrettiForecast): уровень давления, приведённого к у.м.,
// × тренд за 3 ч -> текст прогноза. Свои кольца истории НЕ портируем —
// тренд приходит из даталога (wxc::baroTrend3h, WxTrend.h).
//
// Входы: pressSeaHpa (гПа у.м.), trend (−1/0/+1 от wxc::baroTrend3h),
// deltaHpa3h (сырая дельта — для шторм-флага). NaN/вне диапазона -> idx 0.
// Шторм-флаг: падение > 4 гПа/3ч (слот WG_EVENT_STORM_WARNING ядра,
// Issue #1 23.08; дополнение владельца 08.09 — предупреждение в ПАЗ).
// Ядро не трогаем; новых полей конфига — 0 (нота W5, 08.09.2026).
// ============================================================================
#pragma once
#include <stdint.h>
#include <math.h>

namespace wxz {

// Индексы прогноза: 0 = нет данных, 1..12 — ветки таблицы монолита
// (текст — forecastText(); JSON несёт только индекс — бюджет MQTT 256).
constexpr uint8_t FC_NONE = 0;

// Штормовой порог спада давления, гПа/3ч (резкий спад = ухудшение).
constexpr float STORM_DROP_HPA_3H = 4.0f;

// Давление у.м. в допустимых пределах? (защита от мусора/NaN)
inline bool pressValid(float p) {
    return !isnan(p) && p >= 900.0f && p <= 1100.0f;
}

// Шторм-флаг: резкий спад более STORM_DROP_HPA_3H за 3 часа.
inline bool stormAlarm(float deltaHpa3h) {
    return !isnan(deltaHpa3h) && deltaHpa3h < -STORM_DROP_HPA_3H;
}

// Таблица Замбретти (монолит v5.2, 1:1; trend<0 там был < −0.5 гПа —
// у нас тренд дискретный ±1.0 гПа/3ч, WxTrend.h, семантика сохранена).
inline uint8_t forecastIdx(float p, int8_t trend) {
    if (!pressValid(p)) return FC_NONE;
    if (p > 1030.0f && trend > 0)  return 1;   // Отличная, ясная погода
    if (p > 1025.0f)               return 2;   // Ясная погода
    if (p > 1020.0f && trend < 0)  return 3;   // Переменная облачность
    if (p > 1020.0f)               return 4;   // Хорошая погода
    if (p > 1015.0f && trend > 0)  return 5;   // Улучшение погоды
    if (p > 1015.0f)               return 6;   // Облачно с прояснениями
    if (p > 1010.0f && trend < 0)  return 7;   // Вероятны осадки
    if (p > 1010.0f)               return 8;   // Облачно
    if (p > 1005.0f && trend < 0)  return 9;   // Дождливая погода
    if (p > 1005.0f)               return 10;  // Пасмурно
    if (p > 1000.0f)               return 11;  // Дожди
    return 12;                                 // Штормовое предупреждение
}

// Текст прогноза по индексу (UTF-8, как в монолите; эмодзи сохранены).
inline const char* forecastText(uint8_t idx) {
    switch (idx) {
        case 1:  return "Отличная, ясная погода ☀";
        case 2:  return "Ясная погода ☀";
        case 3:  return "Переменная облачность ⛅";
        case 4:  return "Хорошая погода ☀";
        case 5:  return "Улучшение погоды 🌤";
        case 6:  return "Облачно с прояснениями ⛅";
        case 7:  return "Вероятны осадки 🌧";
        case 8:  return "Облачно ☁";
        case 9:  return "Дождливая погода 🌧";
        case 10: return "Пасмурно ☁";
        case 11: return "Дожди 🌧";
        case 12: return "Штормовое предупреждение ⚡";
        default: return "Ожидание данных…";
    }
}

} // namespace wxz
