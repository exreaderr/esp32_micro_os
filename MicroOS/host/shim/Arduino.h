// ============================================================================
// Arduino.h — МИНИМАЛЬНЫЙ ШИМ ДЛЯ HOST-ТЕСТОВ (D2)
// ============================================================================
// Подменяет Arduino-окружение для компиляции чистых модулей ядра на хосте
// (g++). Реализует ТОЛЬКО то, что реально используют host-тестируемые
// модули: Serial (printf/println -> stdout), millis, макрос F().
// НЕ эмулирует: FreeRTOS, GPIO, прерывания — модули с такими зависимостями
// host-тестами не покрываются (это честная граница метода).
// ============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

// Макрос F() на ESP32 держит строку во flash; на хосте — тождественность
#define F(x) (x)

// millis(): на хосте время не течёт само — тесты при желании двигают его
// через hostSetMillis(); по умолчанию 0.
inline uint32_t& hostMillisRef() { static uint32_t v = 0; return v; }
inline uint32_t millis() { return hostMillisRef(); }
inline void hostSetMillis(uint32_t v) { hostMillisRef() = v; }

// Serial-заглушка: печать в stdout (видим протокол теста)
class HostSerial {
public:
    void printf(const char* fmt, ...) const {
        va_list ap; va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    void println(const char* s = "") const { printf("%s\n", s); }
    void print(const char* s) const { printf("%s", s); }
};
static const HostSerial Serial;
