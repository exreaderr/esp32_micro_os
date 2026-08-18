// ============================================================================
// smart_lock.ino — КОНТРОЛЛЕР СКУД на МикроОС 5.0 (боевая прошивка)
// ============================================================================
// Железо: WT32-ETH01 (LAN8720) + DS3231 + Wiegand-считыватель + реле замка
// + геркон + кнопка EXIT + DFPlayer Mini (клон MP3-TF-16P).
// Карта пинов: projects/smart_lock/src/SmartLockProfile.h (SmartLockPins).
//
// Библиотека: MicroOS (установить в ~/Arduino/libraries/).
// Весь запуск — одна строка: ядро поднимает модули, драйверы, шину,
// Safe Mode и профиль само.
// ============================================================================
#include <MicroOS.h>
#include "src/SmartLockProfile.h"

void setup() {
    Kernel::getInstance().run<SmartLockProfile>();
}

void loop() {
    Kernel::getInstance().loop();
}
