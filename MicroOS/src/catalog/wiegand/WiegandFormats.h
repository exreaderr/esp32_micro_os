// ============================================================================
// WiegandFormats.h — ЧИСТАЯ ЛОГИКА ФОРМАТОВ WIEGAND (без зависимостей)
// ============================================================================
// Выделено из WiegandDriver.h по требованию D2 (host-тесты): файл включает
// только <cstdint> — компилируется и на ESP32, и на хосте (g++), что даёт
// регрессионные тесты декодера ДО прошивки железа.
//
// УНИВЕРСАЛЬНАЯ СХЕМА (требование: один драйвер на все реализации):
//
//     [P_even][ headCover бит ][ body ][ tailCover бит ][P_odd]
//
//   · ведущий бит — ЧЁТНЫЙ паритет над следующими headCover битами;
//   · замыкающий  — НЕЧЁТНЫЙ паритет над предыдущими tailCover битами;
//   · 37-битный H10302 — без паритетов вообще (голые данные).
//
// Новый экзотический формат = одна строка в таблице, код не трогаем.
// ============================================================================
#pragma once

#include <cstdint>

// ============================================================================
// ДЕСКРИПТОР ФОРМАТА WIEGAND
// ============================================================================
struct WiegandFormat {
    uint8_t     totalBits;       // полная длина кадра, включая паритеты
    uint8_t     headParityCover; // бит под ведущим (чётным) паритетом
    uint8_t     tailParityCover; // бит под замыкающим (нечётным) паритетом
    bool        hasParity;       // false — голые данные (37 бит H10302)
    const char* name;            // "W26-H10301" — для логов/диагностики
};

// Общепринятые форматы (расширяется добавлением строки):
//   W26  H10301 — стандарт: P + 12+12 + P
//   W34        — P + 16+16 + P
//   W35 Corp1000 — P(12: company) + данные + P(21: card) — АСИММЕТРИЧНЫЙ
//   W36        — P + 17+17 + P
//   W37  H10302 — без паритетов, 37 бит данных
//   W40        — P + 19+19 + P
//   W48 Corp1000 — P + 23+23 + P
//   W56        — P + 27+27 + P
constexpr WiegandFormat WIEGAND_FORMATS[] = {
    { 26, 12, 12, true,  "W26-H10301" },
    { 34, 16, 16, true,  "W34"        },
    { 35, 12, 21, true,  "W35-C1000"  },
    { 36, 17, 17, true,  "W36"        },
    { 37,  0,  0, false, "W37-H10302" },
    { 40, 19, 19, true,  "W40"        },
    { 48, 23, 23, true,  "W48-C1000"  },
    { 56, 27, 27, true,  "W56"        },
};
constexpr uint8_t WIEGAND_FORMATS_COUNT =
    sizeof(WIEGAND_FORMATS) / sizeof(WIEGAND_FORMATS[0]);

/// Максимальная поддерживаемая длина кадра (буфер бит = uint64_t)
constexpr uint8_t WIEGAND_MAX_BITS = 64;

// ============================================================================
// РЕЗУЛЬТАТ ДЕКОДИРОВАНИЯ
// ============================================================================
struct WiegandCard {
    uint64_t data;               // информационные биты (без паритетов)
    uint8_t  bitCount;           // длина исходного кадра
    const WiegandFormat* format; // распознанный формат (nullptr = неизвестный)
    bool     parityOk;           // контроль паритета прошёл
};

namespace wiegand {

/// Поиск формата по длине кадра. nullptr — длина неизвестна.
inline const WiegandFormat* findFormatByBits(uint8_t totalBits) {
    for (uint8_t i = 0; i < WIEGAND_FORMATS_COUNT; ++i) {
        if (WIEGAND_FORMATS[i].totalBits == totalBits) {
            return &WIEGAND_FORMATS[i];
        }
    }
    return nullptr;
}

/// Распознать формат и проверить паритет кадра.
/// Чистая функция: без глобального состояния, ISR и EventBus.
inline WiegandCard decodeFrame(uint64_t bits, uint8_t count) {
    WiegandCard card;
    card.data = 0;
    card.bitCount = count;
    card.format = findFormatByBits(count);
    card.parityOk = false;

    if (card.format == nullptr) return card;   // неизвестная длина

    const WiegandFormat& f = *card.format;

    // --- Информационные биты: кадр без паритетных бит по краям -------------
    if (f.hasParity) {
        uint8_t bodyBits = f.totalBits - 2;
        card.data = (bits >> 1) & ((bodyBits >= 64) ? ~0ULL
                                                    : ((1ULL << bodyBits) - 1));
    } else {
        card.data = bits;   // 37 бит: весь кадр — данные
    }

    // --- Паритеты ------------------------------------------------------------
    if (!f.hasParity) {
        card.parityOk = true;   // нечего проверять
        return card;
    }

    // Ведущий (чётный): бит totalBits-1 покрывает следующие headParityCover бит
    uint8_t headParityBit = (uint8_t)((bits >> (f.totalBits - 1)) & 1ULL);
    uint64_t headField = (bits >> (f.totalBits - 1 - f.headParityCover)) &
                         ((1ULL << f.headParityCover) - 1);

    // Замыкающий (нечётный): бит 0 покрывает предыдущие tailParityCover бит
    uint8_t tailParityBit = (uint8_t)(bits & 1ULL);
    uint64_t tailField = (bits >> 1) & ((1ULL << f.tailParityCover) - 1);

    // __builtin_parity: 1 если число единиц нечётно (GCC builtin — есть и
    // на xtensa, и на хосте: файл остаётся переносимым).
    // Чётный паритет: P == parity(поля); нечётный: P != parity(поля).
    bool headOk = (headParityBit == (uint8_t)__builtin_parityll(headField));
    bool tailOk = (tailParityBit != (uint8_t)__builtin_parityll(tailField));

    card.parityOk = headOk && tailOk;
    return card;
}

} // namespace wiegand
