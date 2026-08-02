// ============================================================================
// DriverRegistry.cpp — реализация реестра драйверов
// ============================================================================
#include "DriverRegistry.h"

DriverRegistry& DriverRegistry::getInstance() {
    static DriverRegistry instance;
    return instance;
}

// ============================================================================
// РЕГИСТРАЦИЯ (вызывается при загрузке, до фаз жизненного цикла)
// ============================================================================
bool DriverRegistry::add(IDeviceDriver* driver) {
    if (driver == nullptr) return false;
    if (_count >= DRIVER_REGISTRY_MAX) {
        log(LogLevel::Error, "registry full, driver '%s' rejected",
            driver->driverName());
        return false;
    }
    // Дубликат по имени — отклоняем: два драйвера одного устройства означают
    // либо ошибку профиля, либо попытку занять то же железо.
    if (find(driver->driverName()) != nullptr) {
        log(LogLevel::Error, "duplicate driver name '%s'", driver->driverName());
        return false;
    }
    _slots[_count] = { driver, 0, false };
    _count++;
    log(LogLevel::Info, "driver registered: %s", driver->driverName());
    return true;
}

IDeviceDriver* DriverRegistry::find(const char* name) const {
    for (uint8_t i = 0; i < _count; ++i) {
        const char* n = _slots[i].driver->driverName();
        size_t k = 0;
        while (n[k] && name[k]) { if (n[k] != name[k]) goto next; ++k; }
        if (n[k] == name[k]) return _slots[i].driver;
        next:;
    }
    return nullptr;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void DriverRegistry::init() {
    // Инициализируем драйверы в порядке регистрации. Ошибка одного не
    // останавливает остальные — устройство обязано работать в деградированном
    // виде (уровни деградации, A3).
    _failedCount = 0;
    for (uint8_t i = 0; i < _count; ++i) {
        DriverSlot& s = _slots[i];
        bool ok = s.driver->init();
        s.failed = !ok;
        if (!ok) {
            _failedCount++;
            log(LogLevel::Error, "driver init FAILED: %s",
                s.driver->driverName());
        } else {
            log(LogLevel::Info, "driver init ok: %s", s.driver->driverName());
        }
    }
    _initialized = true;
}

void DriverRegistry::start() {
    _started = true;
    log(LogLevel::Info, "started, %u drivers, %u failed", _count, _failedCount);
}

void DriverRegistry::stop() {
    _started = false;
}

// ============================================================================
// TICK: опрос драйверов по индивидуальным периодам
// ============================================================================
void DriverRegistry::tick() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < _count; ++i) {
        DriverSlot& s = _slots[i];
        if (s.failed) continue;                        // мёртвых не дёргаем
        if (now - s.lastPollMs < s.driver->getPollIntervalMs()) continue;
        s.lastPollMs = now;
        s.driver->poll();
    }
}

// ============================================================================
// ЗДОРОВЬЕ
// ============================================================================
bool DriverRegistry::allHealthy() const {
    for (uint8_t i = 0; i < _count; ++i) {
        if (!_slots[i].driver->isHealthy()) return false;
    }
    return true;
}
