// ============================================================================
// StatusLedModule.cpp — реализация статус-LED (M5)
// ============================================================================
#include "StatusLedModule.h"
#include <services/ConfigService.h>
#include <services/HealthMonitor.h>

StatusLedModule& StatusLedModule::getInstance() {
    static StatusLedModule inst;
    return inst;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void StatusLedModule::init() {
    if (!cfgGetBool("led.enabled", false)) {
        log(LogLevel::Info, "led.enabled=0 — статус-LED выключен (нет ленты?)");
        return;
    }
    _pin    = (uint8_t)cfgGetUInt("led.pin", 21);
    _count  = (uint8_t)cfgGetUInt("led.count", 4);
    _bright = (uint8_t)cfgGetUInt("led.bright", 15);
    if (_count == 0) _count = 1;
    if (_count > MAX_PX) _count = MAX_PX;
    if (_bright == 0) _bright = 1;
    if (_bright > 100) _bright = 100;

    // Маппинг пикселей: "hm.sd,hm.bk,hm.ota,hm.fleet" — по имени проверки.
    char mapStr[96];
    cfgGetStr("led.map", mapStr, sizeof(mapStr), "hm.sd,hm.bk,hm.ota,hm.fleet");
    _mapLen = 0;
    char* p = mapStr;
    while (_mapLen < _count && p != nullptr && *p != '\0') {
        char* comma = strchr(p, ',');
        size_t len = (comma != nullptr) ? (size_t)(comma - p) : strlen(p);
        if (len >= sizeof(_map[0])) len = sizeof(_map[0]) - 1;
        memcpy(_map[_mapLen], p, len);
        _map[_mapLen][len] = '\0';
        _mapLen++;
        p = (comma != nullptr) ? comma + 1 : nullptr;
    }

    _strip = new Adafruit_NeoPixel(_count, _pin, NEO_GRB + NEO_KHZ800);
    _strip->begin();
    _strip->setBrightness((uint8_t)((_bright * 255) / 100));
    _strip->clear();
    _strip->show();
    _hwReady = true;
    log(LogLevel::Info, "статус-LED: GPIO%u, пикселей %u, яркость %u%%, маппинг %u",
        _pin, _count, _bright, _mapLen);
}

void StatusLedModule::start() {
    if (_hwReady) {
        render();
        _started = true;
        log(LogLevel::Info, "StatusLedModule started");
    }
}

void StatusLedModule::stop() {
    if (_strip != nullptr) {
        _strip->clear();
        _strip->show();
    }
    _started = false;
}

void StatusLedModule::tick() {
    if (!_hwReady) return;
    // Пульс «нет данных» — раз в 50 мс фаза; рендер — по тику (500 мс),
    // статусы ПАЗ меняются не чаще, лишний show() не греем.
    _pulsePhase++;
    render();
}

// ============================================================================
// РЕНДЕР
// ============================================================================
int8_t StatusLedModule::checkIndexByName(const char* name) const {
    HealthMonitor& hm = HealthMonitor::getInstance();
    for (uint8_t i = 0; i < hm.checkCount(); i++) {
        if (strcmp(hm.checkNameAt(i), name) == 0) return (int8_t)i;
    }
    return -1;
}

void StatusLedModule::render() {
    if (_strip == nullptr) return;
    HealthMonitor& hm = HealthMonitor::getInstance();
    // Синий пульс «нет данных»: 8 шагов туда-обратно (симметричная пила).
    const uint8_t pulseTbl[8] = { 8, 24, 48, 72, 96, 72, 48, 24 };
    const uint8_t pulse = pulseTbl[_pulsePhase % 8];

    for (uint8_t px = 0; px < _count; px++) {
        uint32_t color = 0;
        if (px < _mapLen && _map[px][0] != '\0') {
            int8_t ci = checkIndexByName(_map[px]);
            if (ci < 0 || hm.checkLastRunAt((uint8_t)ci) == 0) {
                color = _strip->Color(0, 0, pulse);          // нет данных
            } else {
                switch (hm.checkStatusAt((uint8_t)ci)) {
                    case HealthResult::Status::Ok:       color = _strip->Color(0, 64, 0);   break;
                    case HealthResult::Status::Warning:  color = _strip->Color(64, 40, 0);  break;
                    case HealthResult::Status::Critical: color = _strip->Color(96, 0, 0);   break;
                }
            }
        }
        _strip->setPixelColor(px, color);
    }
    _strip->show();
}
