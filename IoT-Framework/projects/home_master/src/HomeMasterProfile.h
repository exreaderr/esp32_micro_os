// ============================================================================
// HomeMasterProfile.h — ПРОФИЛЬ МАСТЕРА УМНОГО ДОМА (композиционный корень)
// ============================================================================
// Второй потребитель ядра и ПЕРВАЯ вторая плата платформы (проверка
// переносимости A4): ESP32-S3-POE-ETH вместо WT32-ETH01. Всё железо платы
// (W5500, SD, I2C-пины DS3231) описано в platform/BoardDesc — профиль лишь
// выбирает boardId и добавляет свои модули.
//
// Роль устройства (концепция home_master): прозрачный транслятор на
// вышестоящий брокер, если он есть; брокер «в меру возможностей», если нет.
// M0 — bring-up: сеть, SD, RTC, HTTP. Брокер/мост/журнал — M1+.
// ============================================================================
#pragma once

#include <core/IDeviceProfile.h>

class HomeMasterProfile : public IDeviceProfile {
public:
    const char* profileId() const override { return "home_master"; }

    /// Плата + safeModePin (BOOT, GPIO0). Быстро, без железа.
    void describeHardware(HardwareManifest& m) override;

    /// Профильных драйверов на M0 нет (DS3231/EspTemp — база ядра).
    void registerDrivers(const HardwareManifest& m) override;

    /// Модули профиля: SdService (хранилище), HomeMasterApp (политика).
    void registerModules(Kernel& k) override;
};
