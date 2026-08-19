// ============================================================================
// HomeMasterProfile.cpp — реализация композиционного корня мастера
// ============================================================================
#include "HomeMasterProfile.h"
#include "HomeMasterEvents.h"
#include "SdService.h"
#include "BrokerService.h"
#include "BridgeService.h"
#include "JournalService.h"
#include "WeatherMirror.h"
#include "HomeMasterApp.h"
#include <core/ResourceManager.h>
#include <core/Kernel.h>

// ============================================================================
// МАНИФЕСТ ПЕРИФЕРИИ
// ============================================================================
void HomeMasterProfile::describeHardware(HardwareManifest& m) {
    // Вторая плата платформы: ядро поднимет W5500 вместо LAN8720 и
    // I2C на 16/17 вместо 32/33 (platform::selectBoard в Kernel::boot).
    m.boardId = platform::BoardId::Esp32S3PoeEth;

    // Safe Mode — кнопка BOOT (GPIO0, есть на плате, проводов не требует).
    // NB: удержание BOOT В МОМЕНТ сброса = download-режим ROM; для Safe Mode
    // BOOT нажимают ПОСЛЕ начала загрузки (как кнопку EXIT у smart_lock).
    m.safeModePin = 0;

    // addGpio не нужен: пины ETH/SD/I2C защищены ядром из BoardDesc,
    // профильной периферии на M0 нет (камера — M5).
}

// ============================================================================
// ДРАЙВЕРЫ
// ============================================================================
void HomeMasterProfile::registerDrivers(const HardwareManifest& m) {
    (void)m;   // DS3231 и EspTemp поднимает ядро как базу платформы
}

// ============================================================================
// МОДУЛИ ПРОФИЛЯ
// ============================================================================
void HomeMasterProfile::registerModules(Kernel& k) {
    // Диапазон событий профиля — из реестра (урок v4.2.2).
    hm_ev::g_base = ResourceManager::getInstance().claimEventRange("home_master");

    // «Память» раньше «головы»: SdService должен быть готов до политики.
    k.registerModule(&SdService::getInstance(),     7, 0);
    // M1: брокер — между памятью и политикой (политика в M2 будет его
    // потребителем: мост к вышестоящему брокеру).
    k.registerModule(&BrokerService::getInstance(), 8, 0);
    // M2: мост — после брокера (его хук), до политики (порядок init:
    // BridgeService::init читает BrokerService::running()).
    k.registerModule(&BridgeService::getInstance(), 9, 0);
    // M3.1: журнал — слушатель брокера (multi-hook), потребитель SD.
    // После моста: оба на fireHooks, порядок внутри слотов не важен.
    k.registerModule(&JournalService::getInstance(), 9, 1);
    k.registerModule(&HomeMasterApp::getInstance(), 10, 0);
}
