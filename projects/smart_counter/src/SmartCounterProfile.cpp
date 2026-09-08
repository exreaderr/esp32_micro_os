// ============================================================================
// SmartCounterProfile.cpp — реализация композиционного корня smart_counter
// ============================================================================
#include "SmartCounterProfile.h"
#include "SmartCounterEvents.h"
#include "SmartCounterApp.h"
#include <core/DriverRegistry.h>
#include <core/ResourceManager.h>
#include <core/Kernel.h>

SmartCounterPins SmartCounterProfile::_pins;

// ============================================================================
// МАНИФЕСТ ПЕРИФЕРИИ
// ============================================================================
void SmartCounterProfile::describeHardware(HardwareManifest& m) {
    // Быстро и без железа: вызывается ДО детекта Safe Mode.
    m.safeModePin = (int8_t)_pins.safeMode;   // GPIO34, кнопка на GND,
                                              // ВНЕШНИЙ pull-up 10 кОм (манифест)

    // Периферия профиля — универсальным механизмом (уровень 3 драйверной
    // модели). safeModePin НЕ добавляем: пин уже сообщён ядру.
    // Импульсные пины в манифест НЕ заносим: их занимает ядерный
    // CounterService::attachPcnt под владельцем "cnt.pcnt.<имя>" (A2 —
    // конфликт = отказ привязки, что мы и хотим). Двойной claim под
    // разными владельцами дал бы ложный CONFLICT (RM идемпотентен
    // только для того же владельца).
    m.addGpio(_pins.battAdc,    "sc.adc.batt");
    m.addGpio(_pins.acSense,    "sc.ac.sense");
    m.addGpio(_pins.rs485Rx,    "sc.rs485.rx");
    m.addGpio(_pins.rs485Tx,    "sc.rs485.tx");
    m.addGpio(_pins.rs485En,    "sc.rs485.en");
}

// ============================================================================
// ДРАЙВЕРЫ
// ============================================================================
void SmartCounterProfile::registerDrivers(const HardwareManifest& m) {
    (void)m;   // пины — из SmartCounterPins (манифест уже валидирован RM)

    // C1: профильных драйверов нет. DS3231 (время, RTC=UTC) и EspTemp
    // поднимает ядро как базовые драйверы платформы.
    // C2: MercuryDriver — драйвер ПРОФИЛЯ (projects/smart_counter/src),
    // RS-485/UART2 9600 8N1, неблокирующий FSM (ScMercuryCore).
}

// ============================================================================
// МОДУЛИ ПРОФИЛЯ
// ============================================================================
void SmartCounterProfile::registerModules(Kernel& k) {
    // Диапазон событий профиля — из реестра. До этой строки sc_ev::*
    // недействительны.
    sc_ev::g_base = ResourceManager::getInstance().claimEventRange("smart_counter");

    // Единственный прикладной модуль (C1). Приоритет 9 — в профильной
    // зоне, после ядерных сервисов (образец: WeatherGateApp).
    k.registerModule(&SmartCounterApp::getInstance(), 9, 500);
}
