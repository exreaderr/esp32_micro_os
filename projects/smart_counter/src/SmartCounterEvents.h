// ============================================================================
// SmartCounterEvents.h — СОБЫТИЯ ПРОФИЛЯ SMART_COUNTER (диапазон от RM)
// ============================================================================
// База диапазона — от ResourceManager::claimEventRange("smart_counter")
// (урок v4.2.2: жёсткие ID = коллизии; образец — WeatherGateEvents.h).
// До регистрации база = 0 — любое использование до этого момента является
// ошибкой программирования. Состав событий утверждён концепт-нотой C0
// (Issue #3, рецензия ядерной ветки 01.09.2026).
// ============================================================================
#pragma once

#include <cstdint>

namespace sc_ev {

// Записывается один раз из SmartCounterProfile::registerModules().
inline int32_t g_base = 0;

// Смещения внутри диапазона (шаг claimEventRange = 0x40 — запас 64 ID).
inline int32_t leakDetected()  { return g_base + 0x00; } // протечка: нет сухого часа (ScPulseCore)
inline int32_t leakCleared()   { return g_base + 0x01; } // протечка снята (первый сухой час)
inline int32_t meterReset()    { return g_base + 0x02; } // показания/базы сброшены (вручную/биллинг)
inline int32_t acLoss()        { return g_base + 0x03; } // сеть пропала — переход на АКБ (снимок!)
inline int32_t acRestored()    { return g_base + 0x04; } // сеть вернулась
inline int32_t filterReplace() { return g_base + 0x05; } // ресурс Big Blue исчерпан
inline int32_t billReset()     { return g_base + 0x06; } // зафиксирована база отчётного периода

} // namespace sc_ev
