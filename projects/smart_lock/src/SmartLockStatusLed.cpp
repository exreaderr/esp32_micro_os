// ============================================================================
// SmartLockStatusLed.cpp — реализация статус-ленты WS2812 (smart_lock)
// ============================================================================
#include "SmartLockStatusLed.h"
#include "SmartLockEvents.h"
#include "LockControl.h"
#include <services/ConfigService.h>
#include <services/HealthMonitor.h>
#include <core/ShTypes.h>
#include <core/Events.h>          // ACCESS_EVENT_DENIED
#include <cstring>                // strcmp

SmartLockStatusLed& SmartLockStatusLed::getInstance() {
    static SmartLockStatusLed inst;
    return inst;
}

void SmartLockStatusLed::init() {
    _enabled = cfgGetBool("led.enabled", false);
    if (!_enabled) {
        log(LogLevel::Info, "led.enabled=0 — статус-LED выключен (нет ленты?)");
        return;
    }
    uint8_t count = (uint8_t)cfgGetUInt("led.count", 3);
    uint8_t pin   = (uint8_t)cfgGetUInt("led.pin", 12);
    if (count < 1) count = 1;
    if (count > 16) count = 16;
    _strip = new Adafruit_NeoPixel(count, pin, NEO_GRB + NEO_KHZ800);
    if (_strip == nullptr) {
        log(LogLevel::Error, "led: no heap for strip — индикация выключена");
        _enabled = false;
        return;
    }
    _strip->begin();
    log(LogLevel::Info, "led: WS2812 init, pin=%u, count=%u", pin, count);
}

void SmartLockStatusLed::start() {
    if (!_enabled || _strip == nullptr) return;
    // Ритуал загрузки (как у шлюза): все синие до первой оценки.
    for (uint16_t i = 0; i < _strip->numPixels(); i++) {
        _strip->setPixelColor(i, colBlue());
    }
    _strip->show();
}

void SmartLockStatusLed::stop() {
    if (_strip == nullptr) return;
    _strip->clear();
    _strip->show();
}

void SmartLockStatusLed::tick() {
    if (!_enabled || _strip == nullptr) return;
    const uint16_t n = _strip->numPixels();
    const uint32_t now = millis();

    // --- LED0 «Замок»: зелёный, пока исполнитель держит дверь открытой ---
    if (n > 0) {
        const bool open = LockControl::getInstance().isRelayActive() ||
                          LockControl::getInstance().isTriggerHold();
        _strip->setPixelColor(0, open ? colGreen()
                                      : (_bootBlue ? colBlue() : colOff()));
    }

    // --- LED1 «Кнопка выхода»: ограничение / запрет ---
    if (n > 1) {
        uint32_t c = _bootBlue ? colBlue() : colOff();
        if (cfgGetBool("lock.exit_restrict_active", false)) {
            c = colYellow();   // ночное ограничение действует
        }
        if (now < _deniedFlashUntilMs) {
            // красная вспышка: нажатие в запрете (мигаем 2 Гц)
            c = ((now / 500) & 1) ? colRed() : colOff();
        }
        _strip->setPixelColor(1, c);
    }

    // --- LED2 «ПАЗ»: худший статус проверок HealthMonitor ---
    if (n > 2) {
        HealthMonitor& hm = HealthMonitor::getInstance();
        HealthResult::Status worst = HealthResult::Status::Ok;
        for (uint8_t i = 0; i < hm.checkCount(); i++) {
            HealthResult::Status s = hm.checkStatusAt(i);
            if (s == HealthResult::Status::Critical) { worst = s; break; }
            if (s == HealthResult::Status::Warning) worst = s;
        }
        _strip->setPixelColor(2,
            _bootBlue ? colBlue()
                      : (worst == HealthResult::Status::Critical ? colRed()
                        : worst == HealthResult::Status::Warning ? colYellow()
                                                                 : colGreen()));
    }

    _bootBlue = false;   // первый рабочий кадр отрисован
    _strip->show();
}

bool SmartLockStatusLed::canHandleEvent(int32_t id) const {
    // Отказ кнопке выхода по расписанию — это ACCESS_EVENT_DENIED с
    // payload "EXIT_BTN" (SmartLockApp::denyAccess, SCHEDULE_BLOCK).
    // Сырое sl_ev::exitButton идёт с d=nullptr и вердикта не несёт.
    return id == ACCESS_EVENT_DENIED;
}

void SmartLockStatusLed::onEvent(int32_t id, const ShEventData* d) {
    if (id == ACCESS_EVENT_DENIED && d != nullptr &&
        strcmp(d->payload, "EXIT_BTN") == 0) {
        _deniedFlashUntilMs = millis() + 3000;   // красная вспышка 3 с
    }
}
