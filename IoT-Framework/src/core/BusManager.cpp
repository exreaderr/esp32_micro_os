// ============================================================================
// BusManager.cpp — реализация владельца системной I2C-шины
// ============================================================================
#include "BusManager.h"
#include "Events.h"
#include "../platform/BaseProfile.h"

BusManager& BusManager::getInstance() {
    static BusManager instance;
    return instance;
}

// ============================================================================
// INIT: поднять шину и проверить наличие DS3231
// ============================================================================
void BusManager::init() {
    ensureMutex();

    // Пины — из выбранной платы (5.3.0: WT32-ETH01 32/33, S3-POE-ETH 16/17),
    // частота — общая платформенная 400 кГц.
    // Пины уже заняты за "platform.i2c_*" в реестре ресурсов ядром —
    // драйверы профилей занять их не смогут (A2).
    Wire.begin(platform::board().i2cSda, platform::board().i2cScl);
    Wire.setClock(platform::I2C_FREQ_HZ);
    Wire.setTimeOut(50);   // таймаут транзакции, мс: без него зависший
                           // slave подвешивает Wire.endTransmission()

    _busAlive = probe(platform::DS3231_ADDR);
    if (_busAlive) {
        log(LogLevel::Info, "I2C up: DS3231 found at 0x%02X",
            platform::DS3231_ADDR);
    } else {
        // RTC — критичное железо, но не фатальное: TimeService уйдёт на
        // NTP-only, ПАЗ будет дожимать recovery. Просто фиксируем.
        log(LogLevel::Warning, "DS3231 NOT found on boot, bus recovery pending");
    }
    _initialized = true;
}

void BusManager::start() {
    _started = true;
}

void BusManager::stop() {
    _started = false;
}

// ============================================================================
// TICK: редкий контроль живости (раз в 30 с)
// ============================================================================
void BusManager::tick() {
    if (!takeMutex()) return;

    bool alive = probe(platform::DS3231_ADDR);
    if (alive != _busAlive) {
        _busAlive = alive;
        // Публикуем факты — решает TimeService (события TIME_EVENT_RTC_*),
        // здесь только состояние самой шины.
        ShEventData d; d.clear();
        d.code = 0;   // шина 0
        postEvent(alive ? DRV_EVENT_BUS_RECOVERED : DRV_EVENT_BUS_DEAD, &d);
        log(alive ? LogLevel::Info : LogLevel::Error,
            "I2C bus %s", alive ? "alive" : "DEAD");
    }

    giveMutex();
}

// ============================================================================
// ДОСТУП К ШИНЕ
// ============================================================================
bool BusManager::probe(uint8_t addr7bit) {
    // ВНИМАНИЕ: вызывающий держит мьютекс (или вызывает из init/tick,
    // где мьютекс уже взят).
    Wire.beginTransmission(addr7bit);
    return Wire.endTransmission() == 0;
}

void BusManager::busFault() {
    _consecutiveFaults++;
    log(LogLevel::Warning, "I2C fault #%u", _consecutiveFaults);

    if (_consecutiveFaults >= BUS_MAX_FAULTS_BEFORE_RECOVERY) {
        uint32_t now = millis();
        if (now - _lastRecoveryMs > BUS_RECOVERY_COOLDOWN_MS) {
            _lastRecoveryMs = now;
            recover();
        }
    }
}

// ============================================================================
// СКАНИРОВАНИЕ
// ============================================================================
uint8_t BusManager::scan() {
    if (!takeMutex()) return 0;
    uint8_t found = 0;
    Serial.println(F("[I2C] scan:"));
    for (uint8_t addr = 1; addr < 127; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  0x%02X found\n", addr);
            found++;
        }
    }
    if (found == 0) Serial.println(F("  (empty)"));
    giveMutex();
    return found;
}

// ============================================================================
// RECOVERY: 9 тактов SCL вручную (из ПАЗ монолита v2.5.0)
// ============================================================================
// Зависший slave может держать SDA в нуле. Девять тактов SCL завершают
// его незаконченную транзакцию; затем STOP-условие и повторный begin().
// ============================================================================
bool BusManager::recover() {
    log(LogLevel::Warning, "I2C recovery: manual clock pulses...");

    // Отпускаем Wire и переключаем пины на ручное управление
    Wire.end();
    pinMode(platform::board().i2cScl, OUTPUT);
    pinMode(platform::board().i2cSda, INPUT_PULLUP);

    for (uint8_t i = 0; i < BUS_RECOVERY_CLOCK_PULSES; ++i) {
        digitalWrite(platform::board().i2cScl, HIGH);
        delayMicroseconds(5);
        digitalWrite(platform::board().i2cScl, LOW);
        delayMicroseconds(5);
    }
    // STOP-условие: SDA LOW->HIGH при SCL HIGH
    pinMode(platform::board().i2cSda, OUTPUT);
    digitalWrite(platform::board().i2cSda, LOW);
    digitalWrite(platform::board().i2cScl, HIGH);
    delayMicroseconds(5);
    digitalWrite(platform::board().i2cSda, HIGH);
    delayMicroseconds(5);

    // Повторный подъём шины штатным драйвером
    Wire.begin(platform::board().i2cSda, platform::board().i2cScl);
    Wire.setClock(platform::I2C_FREQ_HZ);
    Wire.setTimeOut(50);

    _recoveryCount++;
    _consecutiveFaults = 0;

    _busAlive = probe(platform::DS3231_ADDR);
    ShEventData d; d.clear();
    d.code = 0;
    if (_busAlive) {
        log(LogLevel::Info, "I2C recovery SUCCESS (total %lu)",
            (unsigned long)_recoveryCount);
        postEvent(DRV_EVENT_BUS_RECOVERED, &d);
    } else {
        log(LogLevel::Error, "I2C recovery FAILED (total %lu)",
            (unsigned long)_recoveryCount);
        postEvent(DRV_EVENT_BUS_DEAD, &d);
    }
    return _busAlive;
}
