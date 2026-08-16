// ============================================================================
// SmartLockProfile.h — ПРОФИЛЬ КОНТРОЛЛЕРА СКУД (композиционный корень)
// ============================================================================
// Перенесено из монолита smart_lock v2.5.0 (WT32-ETH01 + DS3231).
// Единственное место, где профиль знает про свою периферию.
// Ядро (core/, services/) этот файл не включает и не знает о нём.
//
// Точка входа устройства (main.cpp боевой прошивки):
//   void setup() { Kernel::getInstance().run<SmartLockProfile>(); }
// ============================================================================
#pragma once

#include <core/IDeviceProfile.h>

// ============================================================================
// АППАРАТНАЯ КАРТА ПРОФИЛЯ (из smart_lock v2.5.0, часть 1)
// ============================================================================
// База WT32-ETH01 (LAN8720, PHY power GPIO16) и DS3231 (I2C 0x68, SDA=32,
// SCL=33) описана в platform/BaseProfile — здесь только периферия СКУД.
struct SmartLockPins {
    // Wiegand-считыватель (ISR, FALLING, INPUT_PULLUP) — драйвер КАТАЛОГА
    // (profiles/drivers/wiegand, уровень 3 драйверной модели)
    uint8_t wiegandD0 = 4;    // GPIO4  — Data0
    uint8_t wiegandD1 = 2;    // GPIO2  — Data1

    // Исполнительная часть и датчики
    uint8_t lockRelay   = 15; // GPIO15 — реле замка (активный уровень зависит
                              // от lock.fail_secure: Fail-Secure → HIGH=открыт,
                              // Fail-Safe → LOW=открыт)
    uint8_t doorSense   = 39; // GPIO39 — геркон двери (HIGH = открыта), чистый вход
    uint8_t buttonExit  = 14; // GPIO14 — кнопка выхода (INPUT_PULLUP, LOW=нажата);
                              // она же = safeModePin: ядро слушает её при старте,
                              // профиль — в рабочем цикле. addGpio НЕ делаем:
                              // пин уже сообщён ядру через манифест.

    // DFPlayer Mini на UART2 (9600 8N1), железо — клон MP3-TF-16P
    uint8_t dfUart      = 2;
    uint8_t dfRx        = 17; // GPIO17 — RX ESP32 (к TX плеера)
    uint8_t dfTx        = 5;  // GPIO5  — TX ESP32 (к RX плеера)
    uint8_t dfBusy      = 36; // GPIO36 — BUSY (LOW во время воспроизведения)

    // Аппаратный джампер автономности (внешняя подтяжка 10 кОм к 3.3В,
    // замыкание на GND = строгий локальный режим без сети)
    uint8_t localModeJumper = 35;
};

// ============================================================================
// ПРОФИЛЬ
// ============================================================================
class SmartLockProfile : public IDeviceProfile {
public:
    const char* profileId() const override { return "smart_lock"; }

    /// Манифест: пины SmartLockPins -> HardwareManifest (быстро, без железа —
    /// вызывается ДО детекта Safe Mode).
    void describeHardware(HardwareManifest& m) override;

    /// Драйверы: WiegandDriver (каталог) + DfPlayerDriver (клон-пресет).
    /// Ds3231/EspTemp поднимает ядро как базу платформы.
    void registerDrivers(const HardwareManifest& m) override;

    /// Модули профиля: CardStore (база карт), LockControl (исполнитель),
    /// SmartLockApp (политика). Здесь же — claimEventRange для sl_ev.
    void registerModules(Kernel& k) override;

    /// Статический доступ к пинам для модулей профиля.
    static const SmartLockPins& pins() { return _pins; }

private:
    static SmartLockPins _pins;
};
