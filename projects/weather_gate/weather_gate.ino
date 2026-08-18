// ============================================================================
// weather_gate.ino — ШЛЮЗ ПОГОДНОЙ СТАНЦИИ (МикроОС 5.0, WT32-ETH01)
// ============================================================================
// Профиль WeatherGateProfile. W1: скелет + Bme280Driver.
// W2: CC1101 RX-only + декодер Fine Offset. W3: weather-JSON
// (HTTP + MQTT retained) для smart_lock, DataLog + графики uPlot,
// ПАЗ-проверки, авто-высота. W4: даталог wx_* на home_master.
// W5: Замбретти.
// Host-тесты чистой логики — host/tests.cpp (testBme280,
// testFineOffset, testCc1101Core, testWeatherCore).
// ============================================================================
#include <MicroOS.h>
#include "src/WeatherGateProfile.h"

void setup() {
    Kernel::getInstance().run<WeatherGateProfile>();
}

void loop() {
    Kernel::getInstance().loop();
}
