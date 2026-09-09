// ============================================================================
// CyrFont5x7.h — МИНИ-ШРИФТ КИРИЛЛИЦЫ 5×7 для OLED (M5)
// ============================================================================
// Утилитарный шрифт статус-экрана: 33 глифа А–Я + Ё (Ё = Е, на 5×7 точки
// не помещаются), строчные отображаются ПРОПИСНЫМИ («Бэкапы» → «БЭКАПЫ»).
// ASCII рисует штатный шрифт Adafruit GFX, кириллицу — мы сами.
// Данные: 5 колонок на глиф, бит 0 = верхний пиксель (как glcdfont).
// Таблица — 165 байт flash, дешевле и предсказуемее полноценного U8g2.
// ============================================================================
#pragma once

#include <Adafruit_GFX.h>

namespace cyr5x7 {

// Порядок: А Б В Г Д Е Ё Ж З И Й К Л М Н О П Р С Т У Ф Х Ц Ч Ш Щ Ъ Ы Ь Э Ю Я
constexpr uint8_t FONT[33][5] = {
    {0x7E,0x09,0x09,0x09,0x7E},   // А
    {0x7F,0x40,0x7C,0x42,0x3C},   // Б
    {0x7F,0x49,0x49,0x49,0x36},   // В
    {0x7F,0x40,0x40,0x40,0x40},   // Г
    {0x02,0x7D,0x44,0x44,0x7F},   // Д
    {0x7F,0x49,0x49,0x49,0x41},   // Е
    {0x7F,0x49,0x49,0x49,0x41},   // Ё (≈Е)
    {0x63,0x14,0x7F,0x14,0x63},   // Ж
    {0x41,0x49,0x49,0x49,0x36},   // З
    {0x7F,0x02,0x04,0x08,0x7F},   // И
    {0x7F,0x02,0x05,0x0A,0x7F},   // Й
    {0x7F,0x08,0x14,0x22,0x41},   // К
    {0x01,0x3F,0x40,0x40,0x7F},   // Л
    {0x7F,0x20,0x10,0x20,0x7F},   // М
    {0x7F,0x08,0x08,0x08,0x7F},   // Н
    {0x3E,0x41,0x41,0x41,0x3E},   // О
    {0x7F,0x40,0x40,0x40,0x7F},   // П
    {0x7F,0x48,0x48,0x48,0x30},   // Р
    {0x3E,0x41,0x41,0x41,0x41},   // С
    {0x40,0x40,0x7F,0x40,0x40},   // Т
    {0x61,0x12,0x0C,0x78,0x40},   // У
    {0x3C,0x42,0x7E,0x42,0x3C},   // Ф
    {0x63,0x14,0x08,0x14,0x63},   // Х
    {0x3F,0x20,0x20,0x3F,0x03},   // Ц
    {0x78,0x04,0x04,0x04,0x7F},   // Ч
    {0x7F,0x20,0x7F,0x20,0x7F},   // Ш
    {0x3F,0x20,0x3F,0x20,0x7F},   // Щ
    {0x40,0x7E,0x09,0x09,0x06},   // Ъ
    {0x7E,0x09,0x06,0x09,0x7E},   // Ы
    {0x7E,0x09,0x09,0x09,0x06},   // Ь
    {0x3E,0x41,0x49,0x49,0x22},   // Э
    {0x78,0x08,0x3E,0x41,0x3E},   // Ю
    {0x0F,0x09,0x09,0x09,0x7E},   // Я
};

/// Индекс глифа по Unicode codepoint (строчные сводятся к прописным);
/// -1 — не кириллица.
inline int8_t glyphIndex(uint16_t cp) {
    if (cp >= 0x0410 && cp <= 0x042F) return (int8_t)(cp - 0x0410);      // А–Я
    if (cp == 0x0401) return 6;                                          // Ё
    if (cp >= 0x0430 && cp <= 0x044F) return (int8_t)(cp - 0x0430);      // а–я
    if (cp == 0x0451) return 6;                                          // ё
    return -1;
}

/// Декодировать один UTF-8 символ; возвращает codepoint, p сдвигается.
inline uint16_t utf8next(const char*& p) {
    uint8_t c = (uint8_t)*p++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && *p != '\0') {
        uint16_t cp = ((uint16_t)(c & 0x1F) << 6) | ((uint8_t)*p & 0x3F);
        p++;
        return cp;
    }
    return 0xFFFD;
}

/// Ширина строки в пикселях (6 на символ — 5 глиф + 1 зазор).
inline uint8_t textWidth(const char* s) {
    uint8_t n = 0;
    const char* p = s;
    while (*p != '\0') { utf8next(p); n++; }
    return (uint8_t)(n * 6);
}

/// Напечатать строку: ASCII — штатным шрифтом GFX, кириллица — нашей
/// таблицей. Курсор (x,y) — как у GFX print (baseline top).
inline void print(Adafruit_GFX& d, int16_t x, int16_t y, const char* s,
                  uint16_t color) {
    int16_t cx = x;
    const char* p = s;
    while (*p != '\0') {
        uint16_t cp = utf8next(p);
        int8_t gi = glyphIndex(cp);
        if (gi >= 0) {
            for (uint8_t col = 0; col < 5; col++) {
                uint8_t bits = FONT[gi][col];
                for (uint8_t row = 0; row < 7; row++) {
                    if (bits & (1 << row)) d.drawPixel(cx + col, y + row, color);
                }
            }
        } else if (cp < 0x80) {
            d.setCursor(cx, y);
            d.setTextColor(color);
            d.write((uint8_t)cp);
        }
        cx += 6;
    }
}

} // namespace cyr5x7
