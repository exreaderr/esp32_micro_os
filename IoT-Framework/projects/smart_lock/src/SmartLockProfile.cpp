// ============================================================================
// SmartLockProfile.cpp — реализация композиционного корня СКУД
// ============================================================================
#include "SmartLockProfile.h"
#include "SmartLockEvents.h"
#include "CardStore.h"
#include "LockControl.h"
#include "SmartLockApp.h"
#include <catalog/wiegand/WiegandDriver.h>
#include <drivers/DfPlayerDriver.h>
#include <core/DriverRegistry.h>
#include <core/ResourceManager.h>
#include <core/Kernel.h>

SmartLockPins SmartLockProfile::_pins;

// ============================================================================
// МАНИФЕСТ ПЕРИФЕРИИ
// ============================================================================
void SmartLockProfile::describeHardware(HardwareManifest& m) {
    // Быстро и без железа: вызывается ДО детекта Safe Mode.
    m.safeModePin   = (int8_t)_pins.buttonExit;  // GPIO14: ядро — при старте,
                                                 // профиль — в рабочем цикле
    m.dfPlayerUart  = _pins.dfUart;
    m.dfPlayerRx    = (int8_t)_pins.dfRx;
    m.dfPlayerTx    = (int8_t)_pins.dfTx;
    m.dfPlayerBusy  = (int8_t)_pins.dfBusy;

    // Периферия каталога/профиля — универсальным механизмом (уровень 3
    // драйверной модели: типизированных полей у неё нет)
    m.addGpio(_pins.wiegandD0,       "sl.wiegand.d0");
    m.addGpio(_pins.wiegandD1,       "sl.wiegand.d1");
    m.addGpio(_pins.lockRelay,       "sl.lock.relay");
    m.addGpio(_pins.doorSense,       "sl.door.sense");
    m.addGpio(_pins.localModeJumper, "sl.local_jumper");
    // buttonExit НЕ добавляем: пин уже сообщён ядру через safeModePin.
}

// ============================================================================
// ДРАЙВЕРЫ
// ============================================================================
void SmartLockProfile::registerDrivers(const HardwareManifest& m) {
    DriverRegistry& dr = DriverRegistry::getInstance();
    (void)m;   // пины — из SmartLockPins (манифест уже валидирован RM)

    // Wiegand (каталог профильной периферии): автоопределение W26–W56
    // (H10301, Corp1000 и др.), параметры антишума — из монолита.
    WiegandDriver::getInstance().configure(
        _pins.wiegandD0, _pins.wiegandD1,
        WiegandDriver::Config{});   // 25 мс кадр, 1500 мс антидребезг,
                                    // strictParity — умолчания = монолит
    dr.add(&WiegandDriver::getInstance());

    // DFPlayer: железо — клон MP3-TF-16P (MH2024K-24SS). Компат-пресет
    // клона + точечные правки монолита (watchdog 12 с).
    auto dfCfg = DfPlayerDriver::cloneMP3TF16P();
    dfCfg.stuckTimeoutMs = 12000;
    DfPlayerDriver::getInstance().configure(
        _pins.dfUart, _pins.dfRx, _pins.dfTx, _pins.dfBusy, dfCfg);
    dr.add(&DfPlayerDriver::getInstance());
}

// ============================================================================
// МОДУЛИ ПРОФИЛЯ
// ============================================================================
void SmartLockProfile::registerModules(Kernel& k) {
    // Диапазон событий профиля — из реестра (урок v4.2.2: жёсткие ID =
    // коллизии). До этой строки sl_ev::* недействительны.
    sl_ev::g_base = ResourceManager::getInstance().claimEventRange("smart_lock");

    // «Память» -> «руки» -> «голова». Приоритеты в профильной зоне
    // (ядро занимает 0–5): исполнитель раньше политики — init LockControl
    // должен отработать до init SmartLockApp (тот читает джампер).
    k.registerModule(&CardStore::getInstance(),   8, 0);
    k.registerModule(&LockControl::getInstance(), 8, 0);
    k.registerModule(&SmartLockApp::getInstance(), 9, 0);
}
