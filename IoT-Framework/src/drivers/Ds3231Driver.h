// ============================================================================
// Ds3231Driver.h — ЧАСЫ РЕАЛЬНОГО ВРЕМЕНИ DS3231 (системная I2C-шина)
// ============================================================================
// Фаза 1. Второй драйвер — проверяет СИНХРОННЫЙ путь цепочки:
//   сервис (TimeService, Phase 2) -> getDateTime() -> BusManager.i2cLock()
//   -> транзакция -> busOk()/busFault() -> (при 3 сбоях) recovery шины.
//
// Что драйвер ДЕЛАЕТ: читает/пишет регистры RTC, следит за живостью,
//   конвертирует BCD.
// Чего драйвер НЕ ДЕЛАЕТ: не решает, какое время системное, не синхронит
//   NTP, не форматирует строки, не ведёт будильники/расписания — это
//   политика TimeService (принцип "драйвер ≠ сервис").
//
// Железо: DS3231 на 0x68, шина WT32-ETH01 (SDA=32, SCL=33, 400 кГц).
// ============================================================================
#pragma once

#include "../core/IDeviceDriver.h"
#include "../core/BusManager.h"
#include "BcdUtils.h"   // чистая BCD-логика (D2: host-тесты)
#include <ctime>

// Опрос живости: раз в 30 с (RTC не требует частого опроса — время читают
// синхронно по требованию TimeService)
constexpr uint32_t DS3231_POLL_MS = 30000;

class Ds3231Driver : public IDeviceDriver {
public:
    static Ds3231Driver& getInstance();

    // --- IDeviceDriver ---------------------------------------------------
    const char* driverName() const override { return "ds3231"; }
    bool init() override;
    void poll() override;
    uint32_t getPollIntervalMs() const override { return DS3231_POLL_MS; }
    bool isHealthy() const override { return _healthy; }

    // --- СИНХРОННЫЙ ДОСТУП (вызывает TimeService) --------------------------
    /// Прочитать текущее время RTC. true — время валидно (модуль жив и
    /// флаг потери питания OSF сброшен). false — данные недостоверны.
    bool getDateTime(struct tm& out);

    /// Записать время в RTC (после NTP-синхронизации). true — записано.
    bool setDateTime(const struct tm& t);

    /// Температура из встроенного сенсора DS3231 (±3°C, для диагностики
    /// корпуса; температура КРИСТАЛЛА ESP32 — отдельно, EspTempDriver).
    bool getTemperature(float& outC);

private:
    Ds3231Driver() = default;

    // --- НИЗКИЙ УРОВЕНЬ (всегда под мьютексом шины) ------------------------
    bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len);
    bool writeRegs(uint8_t reg, const uint8_t* buf, uint8_t len);

    // --- BCD-конвертация (делегаты к чистой BcdUtils.h — D2: host-тесты) ---
    static uint8_t bcdToBin(uint8_t b) { return bcd::toBin(b); }
    static uint8_t binToBcd(uint8_t b) { return bcd::toBcd(b); }

    bool     _healthy = false;
    uint32_t _lastOkMs = 0;   // последняя успешная транзакция (диагностика)
};
