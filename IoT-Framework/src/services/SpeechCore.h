// ============================================================================
// SpeechCore.h — СОСТАВНАЯ РЕЧЬ: ЧИСЛА И ЕДИНИЦЫ (чистая логика, D2)
// ============================================================================
// Ядерный бэклог 5.1.0 («SpeechBuilder: голосовые папки 05/06»).
// Реализует «Дальше (прошивка)» из README_ГОЛОС.md конвейера: сборка
// фраз цепочками треков — speakNumber(21) -> «дв+адцать од+ин»,
// speakTime(12, 0) -> «двен+адцать час+ов р+овно».
//
// Раскладка папок — из manifest.json конвейера (voice_library/05, /06),
// таблицы ниже экспортированы его же генератором констант. НЕ ПРАВИТЬ
// РУКАМИ: смена номера трека — только через перевыпуск голосовой папки.
//
// Папка 05 (numbers) — из manifest.json, не править руками в прошивке!
//   001 = ноль            012 = девять          023 = двадцать   034 = четыреста
//   002 = один (м)        013 = десять          024 = тридцать   035 = пятьсот
//   003 = одна (ж)        014 = одиннадцать     025 = сорок      036 = шестьсот
//   004 = два (м/ср)      015 = двенадцать      026 = пятьдесят  037 = семьсот
//   005 = две (ж)         016 = тринадцать      027 = шестьдесят 038 = восемьсот
//   006 = три             017 = четырнадцать    028 = семьдесят  039 = девятьсот
//   007 = четыре          018 = пятнадцать      029 = восемьдесят
//   008 = пять            019 = шестнадцать     030 = девяносто  040 = тысяча
//   009 = шесть           020 = семнадцать      031 = сто        041 = тысячи
//   010 = семь            021 = восемнадцать    032 = двести     042 = минус
//   011 = восемь          022 = девятнадцать    033 = триста     043 = ровно
//
// Папка 06 (units) — из manifest.json, не править руками в прошивке!
//   001 = градусов        004 = процентов влажности  007 = час    010 = минута
//   002 = процентов       005 = кубических метров    008 = часа   011 = минуты
//   003 = мм рт. ст.      006 = киловатт-часов       009 = часов  012 = минут
//                                                       013 = ватт
// Триады (час, минута) — выбор формы по числу (pluralForm). Остальные
// единицы в библиотеке существуют только в форме «много» — фиксированный
// хвост (осознанный минимализм конвейера: «двадцать один градусов» —
// бытовая норма говорящих железок).
//
// Честные границы конструктора:
//   · диапазон чисел -4999..+4999 (в библиотеке нет формы «тысяч»);
//   · род влияет только на хвост числа (1 -> од+ин/одн+а, 2 -> дв+а/две) —
//     по-русски так и есть: «сто дв+а», «дв+адцать одн+а».
// ============================================================================
#pragma once

#include <cstdint>

// --- АДРЕСА НА SD (константы раскладки, см. шапку) ------------------------------
constexpr uint8_t SB_FOLDER_NUMBERS = 5;
constexpr uint8_t SB_FOLDER_UNITS   = 6;

// Папка 05: точки, к которым обращается логика (остальные — арифметика)
constexpr uint8_t SB_TRACK_ZERO     = 1;    // ноль
constexpr uint8_t SB_TRACK_ONE_M    = 2;    // од+ин
constexpr uint8_t SB_TRACK_ONE_F    = 3;    // одн+а
constexpr uint8_t SB_TRACK_TWO_M    = 4;    // дв+а
constexpr uint8_t SB_TRACK_TWO_F    = 5;    // две
constexpr uint8_t SB_TRACK_THOUSAND_ONE = 40;  // т+ысяча
constexpr uint8_t SB_TRACK_THOUSAND_FEW = 41;  // т+ысячи
constexpr uint8_t SB_TRACK_MINUS    = 42;   // м+инус
constexpr uint8_t SB_TRACK_EXACTLY  = 43;   // р+овно

// Папка 06: единицы
constexpr uint8_t SB_UNIT_DEGREES   = 1;    // градусов (фикс.)
constexpr uint8_t SB_UNIT_PERCENT   = 2;    // процентов (фикс.)
constexpr uint8_t SB_UNIT_MMHG      = 3;    // мм рт. ст. (фикс.)
constexpr uint8_t SB_UNIT_HUMIDITY  = 4;    // процентов влажности (фикс.)
constexpr uint8_t SB_UNIT_CUBIC_M   = 5;    // кубических метров (фикс.)
constexpr uint8_t SB_UNIT_KWH       = 6;    // киловатт-часов (фикс.)
constexpr uint8_t SB_UNIT_HOUR_ONE  = 7;    // час   — триада,
constexpr uint8_t SB_UNIT_HOUR_FEW  = 8;    // часа    выбор по
constexpr uint8_t SB_UNIT_HOUR_MANY = 9;    // часов   pluralForm
constexpr uint8_t SB_UNIT_MIN_ONE   = 10;   // минута — триада
constexpr uint8_t SB_UNIT_MIN_FEW   = 11;   // минуты
constexpr uint8_t SB_UNIT_MIN_MANY  = 12;   // минут
constexpr uint8_t SB_UNIT_WATT      = 13;   // ватт (фикс.)

// --- ТИПЫ -----------------------------------------------------------------------
enum SpeechGender : uint8_t {
    SB_MASC = 0,    // час, градус, процент...
    SB_FEM  = 1,    // минута, тысяча...
};

// Русская плюральная форма: индекс в триаде (ONE/FEW/MANY = 0/1/2) —
// совпадает со смещением трека внутри триады папки 06.
enum SpeechPlural : uint8_t {
    SB_PL_ONE  = 0,   // 1, 21, 31... (кроме 11)
    SB_PL_FEW  = 1,   // 2..4, 22..24... (кроме 12..14)
    SB_PL_MANY = 2,   // 0, 5..20, 25..30...
};

// Элемент цепочки: адрес трека на SD.
struct SpeechTrack {
    uint8_t folder;
    uint8_t track;
};

// Бюджет цепочек: число <= 999 — до 3 треков + единица; время — до 6
// («одиннадцать часов двадцать девять минут» = 6). Очередь AudioService
// вмещает 8 — цепочка всегда влезает целиком.
constexpr uint8_t SB_MAX_CHAIN = 8;

namespace speech {

/// Плюральная форма числа (по модулю). 0 -> MANY («ноль часов»).
inline SpeechPlural pluralForm(int32_t n) {
    if (n < 0) n = -n;
    const int mod100 = (int)(n % 100);
    if (mod100 >= 11 && mod100 <= 14) return SB_PL_MANY;
    const int mod10 = mod100 % 10;
    if (mod10 == 1) return SB_PL_ONE;
    if (mod10 >= 2 && mod10 <= 4) return SB_PL_FEW;
    return SB_PL_MANY;
}

/// Число -> цепочка треков папки 05. Без единицы измерения.
/// Возвращает длину цепочки (0 — вне диапазона -4999..4999 ИЛИ maxOut
/// мал: вызывающий молчит/фолбэчит, полуфразы не бывает).
/// gender действует на хвостовые 1/2 (см. шапку).
inline uint8_t numberTracks(int32_t n, SpeechGender gender,
                            SpeechTrack* out, uint8_t maxOut) {
    if (n > 4999 || n < -4999 || maxOut == 0) return 0;
    uint8_t len = 0;
    // Каждое добавление — через guard: не влезло -> 0 (полуфразы не бывает).
    #define SB_PUSH(F, T) do { if (len >= maxOut) return 0; \
                                 out[len++] = { (F), (T) }; } while (0)

    if (n < 0) {
        SB_PUSH(SB_FOLDER_NUMBERS, SB_TRACK_MINUS);
        n = -n;
    }
    if (n == 0) {
        SB_PUSH(SB_FOLDER_NUMBERS, SB_TRACK_ZERO);
        return len;
    }
    // Тысячи (1..4): числительное ВСЕГДА женского рода («одна тысяча»)
    if (n >= 1000) {
        const int th = (int)(n / 1000);
        if (th == 1) SB_PUSH(SB_FOLDER_NUMBERS, SB_TRACK_ONE_F);
        else if (th == 2) SB_PUSH(SB_FOLDER_NUMBERS, SB_TRACK_TWO_F);
        else SB_PUSH(SB_FOLDER_NUMBERS, (uint8_t)(3 + th));   // 3..4
        SB_PUSH(SB_FOLDER_NUMBERS,
                (th == 1) ? SB_TRACK_THOUSAND_ONE : SB_TRACK_THOUSAND_FEW);
        n %= 1000;
        if (n == 0) return len;
    }
    // Сотни
    if (n >= 100) {
        SB_PUSH(SB_FOLDER_NUMBERS, (uint8_t)(30 + n / 100));
        n %= 100;
        if (n == 0) return len;
    }
    // 10..19 — одним треком
    if (n >= 10 && n <= 19) {
        SB_PUSH(SB_FOLDER_NUMBERS, (uint8_t)(3 + n));
        return len;
    }
    // Десятки
    if (n >= 20) {
        SB_PUSH(SB_FOLDER_NUMBERS, (uint8_t)(21 + n / 10));
        n %= 10;
        if (n == 0) return len;
    }
    // Единицы 1..9 (род — здесь)
    if (n == 1) SB_PUSH(SB_FOLDER_NUMBERS,
                        gender == SB_FEM ? SB_TRACK_ONE_F : SB_TRACK_ONE_M);
    else if (n == 2) SB_PUSH(SB_FOLDER_NUMBERS,
                             gender == SB_FEM ? SB_TRACK_TWO_F : SB_TRACK_TWO_M);
    else if (n > 0) SB_PUSH(SB_FOLDER_NUMBERS, (uint8_t)(3 + n));
    return len;
    #undef SB_PUSH
}

/// Трек единицы измерения. Триады (hourBase = SB_UNIT_HOUR_ONE /
/// SB_UNIT_MIN_ONE) — по форме числа; фиксированные — как есть.
/// hourBase == 0 -> fixedTrack.
inline uint8_t unitTrack(int32_t n, uint8_t triadBase, uint8_t fixedTrack) {
    if (triadBase == 0) return fixedTrack;
    return (uint8_t)(triadBase + (uint8_t)pluralForm(n));
}

/// Число + единица: цепочка = numberTracks + unitTrack.
/// triadBase/fixedTrack — см. unitTrack. 0 — не влезло/вне диапазона.
inline uint8_t numberUnitTracks(int32_t n, SpeechGender gender,
                                uint8_t triadBase, uint8_t fixedTrack,
                                SpeechTrack* out, uint8_t maxOut) {
    if (maxOut == 0) return 0;
    const uint8_t len = numberTracks(n, gender, out, (uint8_t)(maxOut - 1));
    if (len == 0) return 0;
    out[len] = { SB_FOLDER_UNITS, unitTrack(n, triadBase, fixedTrack) };
    return (uint8_t)(len + 1);
}

/// Время «ЧЧ:ММ» прописью: «двенадцать часов ровно»,
/// «ноль часов тридцать минут». Часы — м.р., минуты — ж.р.
/// 0 — не влезло (maxOut < 6 не поддерживаем).
inline uint8_t timeTracks(uint8_t hh, uint8_t mm,
                          SpeechTrack* out, uint8_t maxOut) {
    if (hh > 23 || mm > 59 || maxOut < 6) return 0;
    uint8_t len = numberUnitTracks((int32_t)hh, SB_MASC,
                                   SB_UNIT_HOUR_ONE, 0, out, maxOut);
    if (len == 0) return 0;
    if (mm == 0) {
        out[len++] = { SB_FOLDER_NUMBERS, SB_TRACK_EXACTLY };
        return len;
    }
    const uint8_t m = numberUnitTracks((int32_t)mm, SB_FEM,
                                       SB_UNIT_MIN_ONE, 0,
                                       out + len, (uint8_t)(maxOut - len));
    if (m == 0) return 0;
    return (uint8_t)(len + m);
}

} // namespace speech
