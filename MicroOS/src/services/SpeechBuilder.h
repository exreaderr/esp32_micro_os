// ============================================================================
// SpeechBuilder.h — СОСТАВНАЯ РЕЧЬ: клей к AudioService (фасад профилей)
// ============================================================================
// Ядерный бэклог 5.1.0. Тонкий статический фасад: SpeechCore.h (чистая
// логика цепочек) -> AudioService::sayRaw (политика: enabled, анти-флуд,
// тихие часы, приоритет). Профилям не нужно знать раскладку папок 05/06.
//
//   SpeechBuilder::sayNumber(21, SB_MASC);        // «двадцать один»
//   SpeechBuilder::sayTime(12, 0);                // «двенадцать часов ровно»
//   SpeechBuilder::sayDegrees(21);                // «двадцать один градусов»
//   SpeechBuilder::sayNumberUnit(3, SB_FEM,
//                                SB_UNIT_MIN_ONE); // «три минуты»
//
// Гарантии/честные границы:
//   · цепочка ставится в очередь ОДНИМ вызовом — элементы равного
//     приоритета играются FIFO подряд; между элементами может вклиниться
//     только фраза СТРОГО выше приоритетом (тревога важнее «двадцати
//     одного градуса» — это фича арбитража, не баг);
//   · false — звук выключен/нет плеера/вне диапазона числа: вызывающий
//     решает фолбэк (молчание — легитимный исход, профиль знает контекст);
//   · глубина цепочек <= 6 при очереди 8 (см. SpeechCore.h).
// ============================================================================
#pragma once

#include "SpeechCore.h"
#include "AudioService.h"

class SpeechBuilder {
public:
    /// Число прописью (-4999..4999). gender — род хвостовых 1/2.
    static bool sayNumber(int32_t n, SpeechGender gender,
                          uint8_t priority = (uint8_t)SndPriority::Normal) {
        SpeechTrack chain[SB_MAX_CHAIN];
        const uint8_t len = speech::numberTracks(n, gender,
                                                 chain, SB_MAX_CHAIN);
        return playChain(chain, len, priority);
    }

    /// Число + единица. Триады: triadBase = SB_UNIT_HOUR_ONE/SB_UNIT_MIN_ONE
    /// (форма по числу). Фиксированные: triadBase = 0, fixedTrack = SB_UNIT_*.
    static bool sayNumberUnit(int32_t n, SpeechGender gender,
                              uint8_t triadBase, uint8_t fixedTrack,
                              uint8_t priority = (uint8_t)SndPriority::Normal) {
        SpeechTrack chain[SB_MAX_CHAIN];
        const uint8_t len = speech::numberUnitTracks(n, gender,
                                                     triadBase, fixedTrack,
                                                     chain, SB_MAX_CHAIN);
        return playChain(chain, len, priority);
    }

    /// Время прописью: «двенадцать часов ровно».
    static bool sayTime(uint8_t hh, uint8_t mm,
                        uint8_t priority = (uint8_t)SndPriority::Normal) {
        SpeechTrack chain[SB_MAX_CHAIN];
        const uint8_t len = speech::timeTracks(hh, mm, chain, SB_MAX_CHAIN);
        return playChain(chain, len, priority);
    }

    // --- Частые случаи (сахар для профилей) -----------------------------------
    static bool sayDegrees(int32_t n,
            uint8_t priority = (uint8_t)SndPriority::Normal) {
        return sayNumberUnit(n, SB_MASC, 0, SB_UNIT_DEGREES, priority);
    }
    static bool sayPercent(int32_t n,
            uint8_t priority = (uint8_t)SndPriority::Normal) {
        return sayNumberUnit(n, SB_MASC, 0, SB_UNIT_PERCENT, priority);
    }
    static bool sayCubicMeters(uint32_t n,
            uint8_t priority = (uint8_t)SndPriority::Normal) {
        return sayNumberUnit((int32_t)n, SB_MASC, 0, SB_UNIT_CUBIC_M, priority);
    }
    static bool sayKwh(uint32_t n,
            uint8_t priority = (uint8_t)SndPriority::Normal) {
        return sayNumberUnit((int32_t)n, SB_MASC, 0, SB_UNIT_KWH, priority);
    }

private:
    /// Постановка цепочки в очередь: все элементы одним проходом.
    /// Провал ЛЮБОГО элемента (очередь полна) -> false; уже вставшие
    /// элементы доиграют (полуфраза при переполнении — цена простой
    /// модели; глубина 8 против цепочки <= 6 делает его экзотикой).
    static bool playChain(const SpeechTrack* chain, uint8_t len,
                          uint8_t priority) {
        if (len == 0) return false;
        bool ok = true;
        for (uint8_t i = 0; i < len; ++i) {
            if (!AudioService::getInstance().sayRaw(chain[i].folder,
                                                    chain[i].track,
                                                    priority)) {
                ok = false;
            }
        }
        return ok;
    }
};
