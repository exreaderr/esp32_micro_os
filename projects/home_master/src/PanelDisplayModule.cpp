// ============================================================================
// PanelDisplayModule.cpp — реализация экрана по требованию (M5)
// ============================================================================
#include "PanelDisplayModule.h"
#include "CyrFont5x7.h"
#include <services/ConfigService.h>
#include <services/HealthMonitor.h>
#include <Wire.h>

PanelDisplayModule& PanelDisplayModule::getInstance() {
    static PanelDisplayModule inst;
    return inst;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void PanelDisplayModule::init() {
    if (!cfgGetBool("oled.enabled", false)) {
        log(LogLevel::Info, "oled.enabled=0 — экран выключен (нет дисплея?)");
        return;
    }
    _timeoutS = (uint16_t)cfgGetUInt("oled.timeout", 30);
    if (_timeoutS < 5) _timeoutS = 5;
    if (_timeoutS > 300) _timeoutS = 300;
    _pinBtn[0] = (uint8_t)cfgGetUInt("btn.plus", 15);
    _pinBtn[1] = (uint8_t)cfgGetUInt("btn.minus", 18);
    _pinBtn[2] = (uint8_t)cfgGetUInt("btn.ok", 2);
    uint8_t addr = (uint8_t)cfgGetUInt("oled.addr", 0x3C);

    // Шина уже поднята ядром (DS3231 на 16/17); повторный begin с теми же
    // пинами безвреден и делает модуль самодостаточным на стенде.
    Wire.begin(17, 16);
    _disp = new Adafruit_SSD1306(128, 64, &Wire, -1);
    if (!_disp->begin(SSD1306_SWITCHCAPVCC, addr)) {
        log(LogLevel::Warning, "SSD1306 не ответил по 0x%02X — экран отключён", addr);
        delete _disp;
        _disp = nullptr;
        return;
    }
    _disp->clearDisplay();
    _disp->display();   // погашен с рождения — будит кнопка
    _hwReady = true;

    for (uint8_t i = 0; i < 3; i++) pinMode(_pinBtn[i], INPUT_PULLUP);
    log(LogLevel::Info, "экран: SSD1306 0x%02X, кнопки %u/%u/%u, таймаут %u с",
        addr, _pinBtn[0], _pinBtn[1], _pinBtn[2], _timeoutS);
}

void PanelDisplayModule::start() {
    if (_hwReady) {
        _started = true;
        log(LogLevel::Info, "PanelDisplayModule started (экран погашен, будит кнопка)");
    }
}

void PanelDisplayModule::stop() {
    sleep();
    _started = false;
}

// ============================================================================
// КНОПКИ + ТАЙМАУТ
// ============================================================================
void PanelDisplayModule::tick() {
    if (!_hwReady) return;
    uint32_t now = millis();

    for (uint8_t i = 0; i < 3; i++) {
        bool raw = digitalRead(_pinBtn[i]);           // LOW = нажата
        if (raw != _btnState[i] && (now - _btnMs[i]) >= 40) {
            _btnMs[i] = now;
            _btnState[i] = raw;
            if (!raw) {                               // фронт нажатия
                if (i == 2) { _okDownMs = now; _okLongFired = false; }
                if (!_awake) { wake(); _skipRelease = true; }  // будим ЛЮБОЙ
                else if (i != 2) onButton(i, false);  // +/- короткие сразу
            } else {                                  // отпускание
                if (_skipRelease) { _skipRelease = false; continue; }
                if (i == 2 && _awake && !_okLongFired) onButton(2, false);
            }
        }
    }
    // Длинное ОК — слот действий (задел второго этапа)
    if (_awake && !_btnState[2] && !_okLongFired && (now - _okDownMs) >= 1500) {
        _okLongFired = true;
        onButton(2, true);
    }
    // Таймаут гашения
    if (_awake && (now - _lastBtnMs) >= (uint32_t)_timeoutS * 1000UL) sleep();
}

void PanelDisplayModule::onButton(uint8_t btn, bool longPress) {
    _lastBtnMs = millis();
    HealthMonitor& hm = HealthMonitor::getInstance();
    uint8_t n = hm.checkCount();

    if (_page == Page::List) {
        if (btn == 0 && n > 0) { _sel = (_sel + 1) % n; }
        else if (btn == 1 && n > 0) { _sel = (_sel == 0) ? n - 1 : _sel - 1; }
        else if (btn == 2) _page = longPress ? Page::Action : Page::Detail;
        // скролл за выбором (6 строк на экран)
        if (_sel < _scroll) _scroll = _sel;
        if (_sel >= _scroll + 6) _scroll = _sel - 5;
        if (_page == Page::Detail) drawDetail();
        else if (_page == Page::Action) drawAction();
        else drawList();
    } else {
        // Detail/Action: любая кнопка — назад к списку
        _page = Page::List;
        drawList();
    }
}

// ============================================================================
// СОН / ПРОБУЖДЕНИЕ
// ============================================================================
void PanelDisplayModule::wake() {
    _awake = true;
    _lastBtnMs = millis();
    _page = Page::List;
    drawList();
}

void PanelDisplayModule::sleep() {
    if (_disp != nullptr) {
        _disp->clearDisplay();
        _disp->display();
    }
    _awake = false;
}

// ============================================================================
// ОТРИСОВКА
// ============================================================================
const char* PanelDisplayModule::friendlyName(const char* n) const {
    // Зеркало CHK_NAME веб-панели (правило: человеческие имена везде).
    if (strcmp(n, "hm.sd") == 0)    return "SD-карта";
    if (strcmp(n, "hm.bk") == 0)    return "Бэкапы парка";
    if (strcmp(n, "hm.ota") == 0)   return "OTA-зеркало";
    if (strcmp(n, "hm.fleet") == 0) return "Парк";
    if (strcmp(n, "heap") == 0)     return "Память";
    if (strcmp(n, "cpu_temp") == 0) return "Температура";
    return n;
}

void PanelDisplayModule::drawList() {
    if (_disp == nullptr) return;
    HealthMonitor& hm = HealthMonitor::getInstance();
    uint8_t n = hm.checkCount();
    _disp->clearDisplay();

    cyr5x7::print(*_disp, 0, 0, "ПАЗ МАСТЕРА", SSD1306_WHITE);
    char pg[8];
    snprintf(pg, sizeof(pg), "%u/%u", n ? _sel + 1 : 0, n);
    _disp->setCursor(128 - 6 * (int)strlen(pg), 0);
    _disp->setTextColor(SSD1306_WHITE);
    _disp->print(pg);
    _disp->drawLine(0, 9, 127, 9, SSD1306_WHITE);

    for (uint8_t row = 0; row < 6; row++) {
        uint8_t idx = _scroll + row;
        if (idx >= n) break;
        int16_t y = 12 + row * 9;
        bool selRow = (idx == _sel);
        if (selRow) _disp->fillRect(0, y - 1, 128, 9, SSD1306_WHITE);
        uint16_t fg = selRow ? SSD1306_BLACK : SSD1306_WHITE;

        const char* st = (hm.checkLastRunAt(idx) == 0) ? "?"
                       : (hm.checkStatusAt(idx) == HealthResult::Status::Ok) ? "OK"
                       : (hm.checkStatusAt(idx) == HealthResult::Status::Warning) ? "!" : "X";
        char line[24];
        snprintf(line, sizeof(line), "%s %s", st, friendlyName(hm.checkNameAt(idx)));
        cyr5x7::print(*_disp, 2, y, line, fg);
    }
    _disp->display();
}

void PanelDisplayModule::drawDetail() {
    if (_disp == nullptr) return;
    HealthMonitor& hm = HealthMonitor::getInstance();
    _disp->clearDisplay();
    if (_sel >= hm.checkCount()) { _disp->display(); return; }

    cyr5x7::print(*_disp, 0, 0, friendlyName(hm.checkNameAt(_sel)), SSD1306_WHITE);
    _disp->drawLine(0, 9, 127, 9, SSD1306_WHITE);

    const char* st = (hm.checkLastRunAt(_sel) == 0) ? "НЕТ ДАННЫХ"
                   : (hm.checkStatusAt(_sel) == HealthResult::Status::Ok) ? "OK"
                   : (hm.checkStatusAt(_sel) == HealthResult::Status::Warning) ? "ВНИМАНИЕ" : "КРИТИКА";
    cyr5x7::print(*_disp, 0, 14, st, SSD1306_WHITE);

    const char* msg = hm.checkMsgAt(_sel);
    if (msg[0] != '\0') cyr5x7::print(*_disp, 0, 26, msg, SSD1306_WHITE);

    uint32_t last = hm.checkLastRunAt(_sel);
    char age[24];
    if (last == 0) snprintf(age, sizeof(age), "ещё не бежала");
    else snprintf(age, sizeof(age), "%lu с назад", (unsigned long)((millis() - last) / 1000));
    cyr5x7::print(*_disp, 0, 40, age, SSD1306_WHITE);
    cyr5x7::print(*_disp, 0, 54, "кнопка — назад", SSD1306_WHITE);
    _disp->display();
}

void PanelDisplayModule::drawAction() {
    if (_disp == nullptr) return;
    _disp->clearDisplay();
    cyr5x7::print(*_disp, 0, 0, "ДЕЙСТВИЯ", SSD1306_WHITE);
    _disp->drawLine(0, 9, 127, 9, SSD1306_WHITE);
    // Задел второго этапа: таблица действий по домену проверки
    // («Обновить ПАЗ», «Перемонтировать SD», «Перезагрузить» — с
    // подтверждением). Пока доменных действий не назначено.
    cyr5x7::print(*_disp, 0, 20, "ДЕЙСТВИЙ НЕТ", SSD1306_WHITE);
    cyr5x7::print(*_disp, 0, 54, "кнопка — назад", SSD1306_WHITE);
    _disp->display();
}
