// ============================================================================
// Ds3231Driver.cpp — реализация драйвера DS3231
// ============================================================================
// Карта регистров DS3231 (даташит Maxim):
//   0x00–0x06  сек/мин/час/день_недели/дата/месяц/год (BCD)
//   0x0E       control
//   0x0F       status (bit7 = OSF — oscillator stopped flag)
//   0x11–0x12  температура (MSB signed, LSB 2 старших бита = 0.25°C)
// ============================================================================
#include "Ds3231Driver.h"
#include "../core/Events.h"
#include "../platform/BaseProfile.h"
#include <Arduino.h>

Ds3231Driver& Ds3231Driver::getInstance() {
    static Ds3231Driver instance;
    return instance;
}

// ============================================================================
// INIT: проба через BusManager (шина к этому моменту поднята)
// ============================================================================
bool Ds3231Driver::init() {
    BusManager& bus = BusManager::getInstance();
    if (!bus.i2cLock()) return false;
    _healthy = bus.probe(platform::DS3231_ADDR);
    bus.i2cUnlock();

    if (_healthy) {
        _lastOkMs = millis();
        bus.busOk();
    } else {
        bus.busFault();   // RTC не отвечает на boot — счётчик ПАЗ шины
    }
    return _healthy;
}

// ============================================================================
// POLL: редкий контроль живости (TimeService читает время сам, по требованию)
// ============================================================================
void Ds3231Driver::poll() {
    BusManager& bus = BusManager::getInstance();
    if (!bus.i2cLock()) return;
    bool alive = bus.probe(platform::DS3231_ADDR);
    bus.i2cUnlock();

    if (alive) {
        _lastOkMs = millis();
        bus.busOk();
        if (!_healthy) {
            _healthy = true;
            // Шина/RTC вернулись — BusManager уже опубликовал
            // DRV_EVENT_BUS_RECOVERED; TimeService сам пересинхронит время.
        }
    } else {
        if (_healthy) _healthy = false;
        bus.busFault();
    }
}

// ============================================================================
// НИЗКИЙ УРОВЕНЬ: транзакции (вызывающий обязан держать i2cLock)
// ============================================================================
bool Ds3231Driver::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
    BusManager& bus = BusManager::getInstance();
    bus.i2c().beginTransmission(platform::DS3231_ADDR);
    bus.i2c().write(reg);
    if (bus.i2c().endTransmission(false) != 0) return false;   // repeated start
    if (bus.i2c().requestFrom(platform::DS3231_ADDR, len) != len) return false;
    for (uint8_t i = 0; i < len; ++i) buf[i] = bus.i2c().read();
    return true;
}

bool Ds3231Driver::writeRegs(uint8_t reg, const uint8_t* buf, uint8_t len) {
    BusManager& bus = BusManager::getInstance();
    bus.i2c().beginTransmission(platform::DS3231_ADDR);
    bus.i2c().write(reg);
    for (uint8_t i = 0; i < len; ++i) bus.i2c().write(buf[i]);
    return bus.i2c().endTransmission() == 0;
}

// ============================================================================
// ВРЕМЯ: чтение
// ============================================================================
bool Ds3231Driver::getDateTime(struct tm& out) {
    BusManager& bus = BusManager::getInstance();
    if (!bus.i2cLock()) return false;

    uint8_t r[7];
    bool ok = readRegs(0x00, r, 7);
    uint8_t status = 0;
    if (ok) ok = readRegs(0x0F, &status, 1);
    bus.i2cUnlock();

    if (!ok) { bus.busFault(); _healthy = false; return false; }
    bus.busOk(); _healthy = true; _lastOkMs = millis();

    // OSF=1: генератор стоял (меняли батарейку/обесточка) — время лжёт
    if (status & 0x80) return false;

    out.tm_sec  = bcdToBin(r[0] & 0x7F);
    out.tm_min  = bcdToBin(r[1] & 0x7F);
    out.tm_hour = bcdToBin(r[2] & 0x3F);        // 24-часовой формат
    out.tm_wday = bcdToBin(r[3] & 0x07) % 7;
    out.tm_mday = bcdToBin(r[4] & 0x3F);
    out.tm_mon  = bcdToBin(r[5] & 0x1F) - 1;    // tm_mon: 0–11
    out.tm_year = bcdToBin(r[6]) + 100;         // tm_year: с 1900
    out.tm_isdst = -1;
    return true;
}

// ============================================================================
// ВРЕМЯ: запись (+ сброс OSF, чтобы чтение снова стало достоверным)
// ============================================================================
bool Ds3231Driver::setDateTime(const struct tm& t) {
    uint8_t r[7];
    r[0] = binToBcd((uint8_t)t.tm_sec);
    r[1] = binToBcd((uint8_t)t.tm_min);
    r[2] = binToBcd((uint8_t)t.tm_hour);        // 24-часовой режим
    r[3] = binToBcd((uint8_t)(t.tm_wday == 0 ? 7 : t.tm_wday));
    r[4] = binToBcd((uint8_t)t.tm_mday);
    r[5] = binToBcd((uint8_t)(t.tm_mon + 1));
    r[6] = binToBcd((uint8_t)(t.tm_year - 100));

    BusManager& bus = BusManager::getInstance();
    if (!bus.i2cLock()) return false;
    bool ok = writeRegs(0x00, r, 7);
    if (ok) {
        uint8_t status = 0;
        ok = readRegs(0x0F, &status, 1) &&
             writeRegs(0x0F, &(status &= (uint8_t)~0x80), 1);  // сброс OSF
    }
    bus.i2cUnlock();

    if (!ok) { bus.busFault(); return false; }
    bus.busOk(); _lastOkMs = millis();
    return true;
}

// ============================================================================
// ТЕМПЕРАТУРА КОРПУСА RTC (диагностика, не путать с кристаллом ESP32!)
// ============================================================================
bool Ds3231Driver::getTemperature(float& outC) {
    BusManager& bus = BusManager::getInstance();
    if (!bus.i2cLock()) return false;
    uint8_t r[2];
    bool ok = readRegs(0x11, r, 2);
    bus.i2cUnlock();

    if (!ok) { bus.busFault(); return false; }
    bus.busOk();
    outC = (float)(int8_t)r[0] + (float)(r[1] >> 6) * 0.25f;
    return true;
}
