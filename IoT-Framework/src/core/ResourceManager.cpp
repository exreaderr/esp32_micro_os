// ============================================================================
// ResourceManager.cpp — реализация реестра ресурсов
// ============================================================================
#include "ResourceManager.h"
#include "Events.h"
#include <cstdarg>

ResourceManager& ResourceManager::getInstance() {
    static ResourceManager instance;
    return instance;
}

// ============================================================================
// ВСПОМОГАТЕЛЬНОЕ
// ============================================================================

/// Безопасное копирование имени владельца (без String, фиксированный буфер).
static void rmCopy(char* dst, size_t dstSize, const char* src) {
    size_t i = 0;
    for (; src && src[i] && i < dstSize - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static bool rmEquals(const char* a, const char* b) {
    size_t i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return false; ++i; }
    return a[i] == b[i];
}

bool ResourceManager::conflict(const char* kind, uint32_t id,
                               const char* newOwner, const char* existingOwner) {
    _conflicts++;
    Serial.printf("[RM] CONFLICT: %s %lu requested by '%s', already held by '%s'\n",
                  kind, (unsigned long)id, newOwner, existingOwner);
    return false;
}

// ============================================================================
// GPIO
// ============================================================================
bool ResourceManager::claimGpio(uint8_t pin, const char* owner) {
    if (pin >= RM_MAX_GPIO) {
        Serial.printf("[RM] ERROR: invalid GPIO %u for '%s'\n", pin, owner);
        _conflicts++;
        return false;
    }
    if (_gpioUsed[pin]) {
        // Идемпотентность: тот же владелец повторно занял свой пин
        if (rmEquals(_gpioOwner[pin], owner)) return true;
        return conflict("GPIO", pin, owner, _gpioOwner[pin]);
    }
    _gpioUsed[pin] = true;
    rmCopy(_gpioOwner[pin], RM_OWNER_LEN, owner);
    return true;
}

bool ResourceManager::isGpioFree(uint8_t pin) const {
    return pin < RM_MAX_GPIO && !_gpioUsed[pin];
}

const char* ResourceManager::gpioOwner(uint8_t pin) const {
    return (pin < RM_MAX_GPIO && _gpioUsed[pin]) ? _gpioOwner[pin] : nullptr;
}

// ============================================================================
// I2C
// ============================================================================
bool ResourceManager::claimI2cAddress(uint8_t addr7bit, const char* owner) {
    for (uint8_t i = 0; i < _i2cCount; ++i) {
        if (_i2cAddr[i] != addr7bit) continue;
        if (rmEquals(_i2cOwner[i], owner)) return true;      // идемпотентно
        return conflict("I2C addr", addr7bit, owner, _i2cOwner[i]);
    }
    if (_i2cCount >= RM_MAX_I2C) {
        Serial.println(F("[RM] ERROR: I2C table full"));
        _conflicts++;
        return false;
    }
    _i2cAddr[_i2cCount] = addr7bit;
    rmCopy(_i2cOwner[_i2cCount], RM_OWNER_LEN, owner);
    _i2cCount++;
    return true;
}

// ============================================================================
// UART
// ============================================================================
bool ResourceManager::claimUart(uint8_t uartNum, const char* owner) {
    if (uartNum >= RM_MAX_UART) {
        Serial.printf("[RM] ERROR: invalid UART %u for '%s'\n", uartNum, owner);
        _conflicts++;
        return false;
    }
    if (_uartUsed[uartNum]) {
        if (rmEquals(_uartOwner[uartNum], owner)) return true;
        return conflict("UART", uartNum, owner, _uartOwner[uartNum]);
    }
    _uartUsed[uartNum] = true;
    rmCopy(_uartOwner[uartNum], RM_OWNER_LEN, owner);
    return true;
}

// ============================================================================
// NVS NAMESPACE
// ============================================================================
bool ResourceManager::claimNvsNamespace(const char* nsName, const char* owner) {
    for (uint8_t i = 0; i < _nvsCount; ++i) {
        if (!rmEquals(_nvsNs[i], nsName)) continue;
        if (rmEquals(_nvsOwner[i], owner)) return true;      // идемпотентно
        return conflict("NVS ns", i, owner, _nvsOwner[i]);
    }
    if (_nvsCount >= RM_MAX_NVS) {
        Serial.println(F("[RM] ERROR: NVS table full"));
        _conflicts++;
        return false;
    }
    rmCopy(_nvsNs[_nvsCount], RM_OWNER_LEN, nsName);
    rmCopy(_nvsOwner[_nvsCount], RM_OWNER_LEN, owner);
    _nvsCount++;
    return true;
}

// ============================================================================
// ДИАПАЗОНЫ СОБЫТИЙ
// ============================================================================
int32_t ResourceManager::claimEventRange(const char* owner) {
    // Повторный запрос тем же владельцем — вернуть его базу (идемпотентно)
    for (uint8_t i = 0; i < _eventCount; ++i) {
        if (rmEquals(_eventOwner[i], owner)) return _eventBase[i];
    }
    if (_eventCount >= RM_MAX_EVENTS) {
        Serial.printf("[RM] ERROR: event ranges exhausted for '%s'\n", owner);
        _conflicts++;
        return -1;
    }
    // Шаг 0x40: 64 события на владельца — с запасом для любого профиля
    int32_t base = SH_EVENT_APP_BASE + _eventCount * 0x40;
    _eventBase[_eventCount] = base;
    rmCopy(_eventOwner[_eventCount], RM_OWNER_LEN, owner);
    _eventCount++;
    return base;
}

// ============================================================================
// ОТЧЁТ
// ============================================================================
size_t ResourceManager::report(char* buf, size_t bufSize) const {
    size_t pos = 0;
    // Макрос-добавлялка с контролем границ буфера
    auto append = [&](const char* fmt, ...) {
        if (pos >= bufSize) return;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf + pos, bufSize - pos, fmt, ap);
        va_end(ap);
        if (n > 0) pos += ((size_t)n < bufSize - pos) ? (size_t)n : bufSize - pos;
    };

    append("=== RESOURCE MAP ===\n[GPIO]\n");
    for (uint8_t p = 0; p < RM_MAX_GPIO; ++p) {
        if (_gpioUsed[p]) append("  GPIO%-3u -> %s\n", p, _gpioOwner[p]);
    }
    append("[I2C]\n");
    for (uint8_t i = 0; i < _i2cCount; ++i) {
        append("  0x%02X   -> %s\n", _i2cAddr[i], _i2cOwner[i]);
    }
    append("[UART]\n");
    for (uint8_t u = 0; u < RM_MAX_UART; ++u) {
        if (_uartUsed[u]) append("  UART%u  -> %s\n", u, _uartOwner[u]);
    }
    append("[NVS]\n");
    for (uint8_t i = 0; i < _nvsCount; ++i) {
        append("  %-12s -> %s\n", _nvsNs[i], _nvsOwner[i]);
    }
    append("[EVENT RANGES]\n");
    for (uint8_t i = 0; i < _eventCount; ++i) {
        append("  0x%04lX+0x40 -> %s\n", (unsigned long)_eventBase[i], _eventOwner[i]);
    }
    append("Conflicts total: %lu\n", (unsigned long)_conflicts);
    return pos;
}
