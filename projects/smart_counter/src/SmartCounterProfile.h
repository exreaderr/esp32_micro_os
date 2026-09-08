// ============================================================================
// SmartCounterProfile.h — ПРОФИЛЬ УМНОГО СЧЁТЧИКА (композиционный корень)
// ============================================================================
// МикроОС 5.0, плата WT32-ETH01. Четвёртый профиль платформы.
// Единственное место, где профиль знает про свою периферию.
//
// Точка входа устройства (smart_counter.ino):
//   void setup() { Kernel::getInstance().run<SmartCounterProfile>(); }
//
// ПИН-МАНИФЕСТ (утверждён концепт-нотой C0, рецензия ядра 01.09.2026):
//   Импульсы:   вода GPIO14 (PCNT), газ GPIO35 (PCNT, input-only);
//               оба занимает CounterService::attachPcnt (см. Profile.cpp)
//   Аналог:     АКБ ИБП GPIO36 (ADC, input-only), сеть 220В GPIO39 (input-only)
//   RS-485:     RX=5, TX=17 (UART2, 9600 8N1), DE/RE=2
//   Safe Mode:  GPIO34, кнопка на GND (активный низ)
//   Резерв:     GPIO4/13/15 (не занимаем — паспорт узла)
//   Ядерные:    I2C 32/33 (DS3231 0x68), RMII 18/19/21/22/23/25/26/27,
//               PHY power 16, GPIO0/1/3 — НЕ ТРОГАТЬ.
//
// АППАРАТНЫЕ ОГОВОРКИ (паспорт узла):
//   · GPIO34/35/36/39 — input-only БЕЗ внутренних подтяжек: Safe Mode
//     на GPIO34 требует ВНЕШНИЙ pull-up 10 кОм к 3V3 (иначе кнопка
//     «плывёт» — устройство уйдёт в Safe Mode от наводки).
//   · Страппинг-пины 0/12 в профиле не используются никогда.
//   · MAX803S на EN: порог 2.93 В, таймаут 140–560 мс, open-drain —
//     страж flash от записей на «умирающем» питании (концепт-нота §5).
// ============================================================================
#pragma once

#include <core/IDeviceProfile.h>

struct SmartCounterPins {
    uint8_t waterPulse = 14;   // PCNT, геркон счётчика ХВС (как у smart_lock)
    uint8_t gasPulse   = 35;   // PCNT, input-only
    uint8_t battAdc    = 36;   // input-only, делитель АКБ (sc.bat.coeff)
    uint8_t acSense    = 39;   // input-only, оптрон сети 220В (ПАЗ питания)
    uint8_t rs485Rx    = 5;    // UART2 RX (Меркурий-206)
    uint8_t rs485Tx    = 17;   // UART2 TX
    uint8_t rs485En    = 2;    // DE/RE трансивера; страппинг — держать LOW при буте
    uint8_t safeMode   = 34;   // input-only, ВНЕШНИЙ pull-up 10 кОм обязателен
};

class SmartCounterProfile : public IDeviceProfile {
public:
    const char* profileId() const override { return "smart_counter"; }

    /// Манифест: пины -> HardwareManifest. Быстро, без железа (до Safe Mode).
    void describeHardware(HardwareManifest& m) override;

    /// Драйверы профиля. C1: нет (DS3231/EspTemp поднимает ядро как базу
    /// платформы). C2: MercuryDriver (RS-485, драйвер профиля — каталог
    /// «зарабатывается» вторым потребителем, решение рецензии C0).
    void registerDrivers(const HardwareManifest& m) override;

    /// Модули профиля: SmartCounterApp. Здесь же — claimEventRange для sc_ev.
    void registerModules(Kernel& k) override;

    /// Статический доступ к пинам для модулей и драйверов профиля.
    static const SmartCounterPins& pins() { return _pins; }

private:
    static SmartCounterPins _pins;
};
