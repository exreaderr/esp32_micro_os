// ============================================================================
// HomeMasterEvents.h — СОБЫТИЯ ПРОФИЛЯ HOME_MASTER (диапазон из реестра)
// ============================================================================
// База диапазона выдаётся ResourceManager::claimEventRange("home_master")
// из HomeMasterProfile::registerModules() (урок v4.2.2: жёсткие ID =
// коллизии). До регистрации база = 0, использование — ошибка программирования.
//
// Что здесь, а что в ядре: сеть/OTA/деградация/аудит — ядерные (Events.h);
// здесь — домен мастера: SD-хранилище, режимы мастера, (M1+) брокер/мост.
// ============================================================================
#pragma once

#include <cstdint>

namespace hm_ev {

// База диапазона (inline-переменная C++17 — профиль header-only).
inline int32_t g_base = 0;

// Смещения (шаг claimEventRange = 0x40 — запас 64 ID).
inline int32_t sdStateChanged()  { return g_base + 0x00; } // code: SdState, payload: тип карты
// Резерв этапов концепции:
//  +0x01 masterModeChanged   — SOLO/BRIDGE/AUTO-переход (M2)
//  +0x02 brokerClientChanged — подключение/отключение клиента брокера (M1)
//  +0x03 journalRotated      — ротация журнала на SD (M3)
//  +0x04 fleetDeviceSeen     — контроллер появился/пропал в реестре (M3)

} // namespace hm_ev
