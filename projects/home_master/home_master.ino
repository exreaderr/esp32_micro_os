// ============================================================================
// home_master.ino — МАСТЕР СИСТЕМЫ УМНОГО ДОМА на МикроОС 5.3.0
// ============================================================================
// Железо: Waveshare ESP32-S3-POE-ETH (ESP32-S3R8: 8 МБ PSRAM, 16 МБ flash,
// W5500 Ethernet + PoE 802.3af, слот microSD) + DS3231 (I2C 16/17).
// Питание: PoE + ИБЖ (DC-DC + АКБ с контроллером заряда, схема smart_counter).
//
// Этап M0 (bring-up): плата, W5500, SD, DS3231, /api/system + /api/dev/hm/*.
// Брокер/мост/журнал — этапы M1+ (см. «МикроОС_5.0_концепция_home_master.md»).
//
// Библиотека: MicroOS (установить в ~/Arduino/libraries/).
// Плата в IDE: ESP32S3 Dev Module, Flash 16MB, PSRAM: OPI PSRAM,
// USB CDC On Boot: Enabled, Partition: по partitions.csv комплекта.
// ============================================================================
#include <MicroOS.h>
#include "src/HomeMasterProfile.h"

void setup() {
    Kernel::getInstance().run<HomeMasterProfile>();
}

void loop() {
    Kernel::getInstance().loop();
}
