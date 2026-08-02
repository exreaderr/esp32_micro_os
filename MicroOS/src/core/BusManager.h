// ============================================================================
// BusManager.h — ВЛАДЕЛЕЦ СИСТЕМНЫХ ШИН (I2C, Phase 1: только I2C0)
// ============================================================================
// Фаза 1. Собирает воедино то, что в v4.2.2 было размазано по двум модулям:
//   · инициализация и сканирование шины   — DeviceManager (I2C-сканер);
//   · восстановление зависшей шины        — PazManager::restoreI2cBus
//                                           (9 тактов SCL вручную).
//
// Принцип: шина одна, владелец один. Драйверы (Ds3231Driver и будущие)
// НЕ трогают Wire напрямую — только через BusManager:
//   · доступ к шине — под общим мьютексом (i2cLock()/i2cUnlock());
//   · сбой транзакции — busFault(): менеджер сам решает, когда делать
//     recovery, и публикует DRV_EVENT_BUS_RECOVERED / DRV_EVENT_BUS_DEAD.
//
// Модуль ядра с ВЫСОКИМ приоритетом (инициализируется до драйверов).
// ============================================================================
#pragma once

#include <Wire.h>
#include "ModuleBase.h"

// Настройки recovery (из монолита v2.5.0, ПАЗ I2C):
constexpr uint8_t  BUS_MAX_FAULTS_BEFORE_RECOVERY = 3;   // сбоев подряд
constexpr uint32_t BUS_RECOVERY_COOLDOWN_MS       = 10000;// не чаще раза в 10 с
constexpr uint8_t  BUS_RECOVERY_CLOCK_PULSES      = 9;   // тактов SCL

class BusManager : public ModuleBase {
public:
    static BusManager& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "BusManager"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0005; }

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 30000; } // редкий
                                                                  // контроль
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- ДОСТУП К ШИНЕ (для драйверов) ---------------------------------------
    /// Объект Wire системной шины. Использовать ТОЛЬКО внутри
    /// i2cLock()/i2cUnlock() — шина общая для DS3231 и будущих устройств.
    TwoWire& i2c() { return Wire; }
    bool i2cLock(TickType_t timeoutMs = SH_MUTEX_TIMEOUT_MS) {
        return takeMutex(timeoutMs);
    }
    void i2cUnlock() { giveMutex(); }

    /// Есть ли устройство на адресе (быстрая проба, под мьютексом снаружи).
    bool probe(uint8_t addr7bit);

    /// Отметить сбой транзакции. После BUS_MAX_FAULTS_BEFORE_RECOVERY
    /// подряд — автоматический recovery (с кулдауном).
    void busFault();

    /// Отметить успешную транзакцию — сбрасывает счётчик сбоев.
    void busOk() { _consecutiveFaults = 0; }

    // --- СКАНИРОВАНИЕ И ДИАГНОСТИКА --------------------------------------------
    /// Скан шины: печать найденных адресов в лог + число в возврате.
    /// Используется при boot-диагностике и из recovery-UI.
    uint8_t scan();

    /// Принудительное восстановление шины (9 тактов SCL + повторный begin).
    /// true — после recovery шина отвечает (проба DS3231).
    bool recover();

    // --- СОСТОЯНИЕ ---------------------------------------------------------------
    bool isBusAlive() const { return _busAlive; }
    uint32_t recoveryCount() const { return _recoveryCount; }

private:
    BusManager() = default;

    uint8_t  _consecutiveFaults = 0;   // сбоев подряд (от драйверов)
    uint32_t _lastRecoveryMs = 0;      // кулдаун recovery
    uint32_t _recoveryCount = 0;       // всего восстановлений (метрика)
    bool     _busAlive = false;        // последний известный статус шины
};
