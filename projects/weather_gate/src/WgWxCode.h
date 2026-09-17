// ============================================================================
// WgWxCode.h — ЧЕСТНЫЙ WMO weather_code ИЗ СЫРЫХ ПЕРЕМЕННЫХ (0.7.4)
// ----------------------------------------------------------------------------
// Предыстория: парсер forecastTask годами показывал «ясно» — strstr находил
// "weather_code" сначала в блоке current_units ("wmo code"), atoi давал 0.
// Плюс сам open-meteo предупреждает: модельный код — сценарий модели,
// надёжнее выводить состояние из сырых переменных (облачность, осадки).
//
// deriveWxCode: осадки важнее облачности (модель «видит» дождь точнее,
// чем облака). Пороги — WMO-коды, привычные фронту wx.html:
//   осадки мм/ч: >=5 -> 65 ливень; >=2.5 -> 63 дождь; >=0.3 -> 61 слабый
//                дождь; >0 -> 51 морось;
//   иначе облачность %: >=85 -> 3 пасмурно; >=50 -> 2 переменная;
//                >=10 -> 1 в осн. ясно; иначе 0 ясно.
//   precipMm < 0 = «поля нет» (урезанный ответ) — решает облачность;
//   cloudCover < 0 = «поля нет» — возвращаем fallback (модельный код).
// Чистая логика, host-тестируемая. Ядро не трогаем, конфиг не меняется.
// ============================================================================
#pragma once
#include <stdint.h>

namespace wgs {

inline int8_t deriveWxCode(int cloudCover, float precipMm, int8_t fallback) {
    if (cloudCover < 0 && precipMm < 0.0f) return fallback;  // нет сырых данных
    if (precipMm >= 5.0f)  return 65;   // ливень
    if (precipMm >= 2.5f)  return 63;   // дождь
    if (precipMm >= 0.3f)  return 61;   // слабый дождь
    if (precipMm > 0.0f)   return 51;   // морось
    if (cloudCover < 0)    return fallback;  // осадков нет, облачности нет
    if (cloudCover >= 85)  return 3;    // пасмурно
    if (cloudCover >= 50)  return 2;    // переменная облачность
    if (cloudCover >= 10)  return 1;    // в основном ясно
    return 0;                           // ясно
}

// ----------------------------------------------------------------------------
// 0.9.0 (W6): WMO weather_code -> HA condition (ASCII-токен протокола HA,
// НЕ русский текст — локализация на стороне фронтенда HA, трафик не тратим).
// Набор HA: sunny, partlycloudy, cloudy, fog, rainy, pouring, snowy,
// snowy-rainy, lightning-rainy, windy. Неизвестный код -> "cloudy"
// (честнее, чем выдуманное «ясно» — урок №25).
// ----------------------------------------------------------------------------
inline const char* wmoToHaCond(int wmo) {
    if (wmo == 0)  return "sunny";
    if (wmo == 1 || wmo == 2)  return "partlycloudy";
    if (wmo == 3)  return "cloudy";
    if (wmo == 45 || wmo == 48) return "fog";
    if (wmo >= 51 && wmo <= 57) return "rainy";         // морось
    if (wmo == 61 || wmo == 63 || wmo == 80 || wmo == 81) return "rainy";
    if (wmo == 65 || wmo == 82) return "pouring";       // ливень
    if (wmo == 66 || wmo == 67) return "snowy-rainy";   // ледяной дождь
    if ((wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86) return "snowy";
    if (wmo >= 95 && wmo <= 99) return "lightning-rainy"; // гроза
    return "cloudy";
}

} // namespace wgs
