// ============================================================================
// ConformanceTest.h — СТЕНД СООТВЕТСТВИЯ ПРОФИЛЯ (D1)
// ============================================================================
// Назначение: автор нового устройства подставляет свой профиль в тестовый
// скетч, прошивает стенд — и получает на Serial протокол PASS/FAIL по
// правилам платформы ДО начала отладки бизнес-логики. Стенд отвечает на
// вопрос: "профиль играет по правилам ядра?"
//
// Что проверяется (правила из архитектуры МикроОС 5.0):
//   МАНИФЕСТ:
//   · safe_mode_pin задан, валиден и не является пином платформы/страппинга;
//   · пины манифеста валидны, без дублей, не пересекаются с платформой
//     (ETH RMII/power, I2C DS3231) и критичными страппинг-пинами (0, 12);
//   РЕЕСТР РЕСУРСОВ:
//   · после полной регистрации — ноль конфликтов ResourceManager (A2);
//   МОДУЛИ (вся таблица ядра, включая профильные):
//   · имя и версия непустые;
//   · ModuleId в узаконенных диапазонах (ShTypes);
//   · нет дублей ModuleId в таблице;
//   · интервал tick разумный (1..60000 мс — иначе модуль либо
//     грузит цикл, либо не живой).
//
// Использование:
//   HardwareManifest m;
//   profile.describeHardware(m);
//   conformance::runAll("my_profile", m) — из start() тестового модуля,
//   когда вся регистрация уже завершена.
// ============================================================================
#pragma once

#include "IDeviceProfile.h"
#include "Kernel.h"
#include "ResourceManager.h"

namespace conformance {

struct Stats { uint8_t pass = 0; uint8_t fail = 0; };

inline void check(Stats& s, bool cond, const char* name) {
    if (cond) { ++s.pass; Serial.printf("  PASS %s\n", name); }
    else      { ++s.fail; Serial.printf("  FAIL %s\n", name); }
}

// Пины, занятые платформой WT32-ETH01 (ETH RMII + PHY power + I2C DS3231).
// Профиль не должен трогать их НИКОГДА.
inline bool isPlatformPin(int8_t pin) {
    static const int8_t PINS[] = {
        16,                 // PHY power
        18, 19, 21, 22, 23, // RMII MDIO/TXD1/TXD0/TX_EN/MDC
        25, 26, 27,         // RMII RXD0/RXD1/CRS_DV
        32, 33,             // I2C (DS3231) — шиной владеет BusManager
    };
    for (size_t i = 0; i < sizeof(PINS); ++i) {
        if (PINS[i] == pin) return true;
    }
    return false;
}

// Критичные страппинг-пины ESP32: 0 (boot mode), 12 (flash voltage).
// Их использование профилем = лотерея при каждом старте -> FAIL.
// (2, 5, 15 — мягкие страппинги, допустимы с пониманием; не проверяем.)
inline bool isCriticalStrapPin(int8_t pin) { return pin == 0 || pin == 12; }

// Диапазоны ModuleId из ShTypes.h
inline bool isValidModuleId(ModuleId id) {
    return id == 0x0000 ||                    // ядро
           (id >= 0x0001 && id <= 0x00FF) ||  // системные сервисы
           (id >= 0x0100 && id <= 0x01FF) ||  // транспорт
           (id >= 0x0200 && id <= 0x02FF) ||  // доменные сервисы
           (id >= 0x1000);                    // приложения профилей
}

inline Stats runAll(const char* profileName, const HardwareManifest& m) {
    Stats s;
    Serial.printf("\n===== CONFORMANCE: профиль '%s' =====\n", profileName);

    // --- МАНИФЕСТ: safe_mode_pin ---------------------------------------------
    Serial.println("--- Манифест: safe_mode_pin ---");
    check(s, m.safeModePin >= 0,
          "safe_mode_pin задан (триггер Safe Mode по кнопке)");
    if (m.safeModePin >= 0) {
        check(s, m.safeModePin <= 39, "safe_mode_pin — валидный GPIO");
        check(s, !isPlatformPin(m.safeModePin),
              "safe_mode_pin не пин платформы");
        check(s, !isCriticalStrapPin(m.safeModePin),
              "safe_mode_pin не критичный страппинг (0/12)");
    }

    // --- МАНИФЕСТ: пины профиля ------------------------------------------------
    Serial.println("--- Манифест: пины ---");
    bool allValid = true, noDup = true, noPlatform = true, noStrap = true;
    for (uint8_t i = 0; i < m.gpioCount; ++i) {
        int8_t p = m.gpio[i].pin;
        if (p < 0 || p > 39) allValid = false;
        if (isPlatformPin(p)) noPlatform = false;
        if (isCriticalStrapPin(p)) noStrap = false;
        for (uint8_t j = (uint8_t)(i + 1); j < m.gpioCount; ++j) {
            if (m.gpio[j].pin == p) noDup = false;
        }
    }
    check(s, allValid,   "все пины — валидные GPIO");
    check(s, noDup,      "нет дублей пинов внутри манифеста");
    check(s, noPlatform, "нет пересечений с пинами платформы");
    check(s, noStrap,    "нет критичных страппинг-пинов (0/12)");

    // --- РЕЕСТР РЕСУРСОВ (A2) ---------------------------------------------------
    Serial.println("--- Реестр ресурсов ---");
    check(s, ResourceManager::getInstance().conflictCount() == 0,
          "ноль конфликтов ResourceManager после регистрации");

    // --- МОДУЛИ --------------------------------------------------------------------
    Serial.println("--- Модули ---");
    Kernel& k = Kernel::getInstance();
    bool namesOk = true, idsOk = true, ticksOk = true, noDupIds = true;
    uint8_t n = k.moduleCount();
    for (uint8_t i = 0; i < n; ++i) {
        const Kernel::ModuleSlot* slot = k.moduleAt(i);
        if (slot == nullptr || slot->module == nullptr) continue;
        IModule* mod = slot->module;
        if (mod->getName() == nullptr || mod->getName()[0] == '\0' ||
            mod->getVersion() == nullptr || mod->getVersion()[0] == '\0') {
            namesOk = false;
        }
        if (!isValidModuleId(mod->getModuleId())) idsOk = false;
        uint32_t t = mod->getTickIntervalMs();
        if (t == 0 || t > 60000) ticksOk = false;
        for (uint8_t j = (uint8_t)(i + 1); j < n; ++j) {
            const Kernel::ModuleSlot* other = k.moduleAt(j);
            if (other != nullptr && other->module != nullptr &&
                other->module->getModuleId() == mod->getModuleId()) {
                noDupIds = false;
            }
        }
    }
    check(s, n > 0,    "таблица модулей непуста");
    check(s, namesOk,  "у всех модулей имя и версия непустые");
    check(s, idsOk,    "ModuleId всех модулей в узаконенных диапазонах");
    check(s, noDupIds, "нет дублей ModuleId");
    check(s, ticksOk,  "интервалы tick разумны (1..60000 мс)");

    Serial.printf("===== ИТОГ: %u PASS, %u FAIL =====\n\n", s.pass, s.fail);
    return s;
}

} // namespace conformance
