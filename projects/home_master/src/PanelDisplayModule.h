// ============================================================================
// PanelDisplayModule.h — ЭКРАН ПО ТРЕБОВАНИЮ (M5): OLED SSD1306 + 3 кнопки
// ============================================================================
// Дизайн (утверждён владельцем 08.09.2026):
//   · OLED 0.96" SSD1306 128×64 на I2C-шине платы (16/17, та же шина, что
//     и DS3231 — адреса разные: 0x3C vs 0x68); питание 3V3;
//   · экран ВСЕГДА погашен; любая кнопка будит; таймаут гашения
//     oled.timeout (умолчание 30 с) от последнего нажатия;
//   · кнопки «+»/«−»/«ОК» (GPIO15/18/2, INPUT_PULLUP, активный низ,
//     антидребезг 40 мс): листаем домены ПАЗ, ОК — детали проверки;
//   · ПЕРВЫЙ ЭТАП — view-only. Архитектура действий заложена: длинное ОК
//     (≥1.5 с) — слот действия по домену (пока «ДЕЙСТВИЙ НЕТ»; вторым
//     этапом: «Обновить ПАЗ», «Перемонтировать SD», «Перезагрузить» —
//     с подтверждением).
//   · нет дисплея/кнопок — модуль молчит (oled.enabled=0 по умолчанию),
//     устройство работает полной программой (закон самодостаточности).
//
// Конфиг (группа «Экран», CFG_CRITICAL — ребут):
//   oled.enabled  BOOL  false — мастер-выключатель;
//   oled.addr     UINT  60    — I2C-адрес (0x3C=60);
//   oled.timeout  UINT  30    — гашение, с (5..300);
//   btn.plus/minus/ok UINT 15/18/2 — пины кнопок.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <Adafruit_SSD1306.h>

class PanelDisplayModule : public ModuleBase {
public:
    static PanelDisplayModule& getInstance();

    const char* getName() const override { return "PanelDisplay"; }
    const char* getVersion() const override { return "0.1.0"; }   // 0.1.0: M5 — OLED по требованию, меню ПАЗ, view-only + задел на действия
    ModuleId getModuleId() const override { return 0x110A; }   // hm: ... 0x1109=StatusLed, 0x110A=PanelDisplay

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 50; }   // кнопки
    // События не нужны: снимок ПАЗ читаем при отрисовке.
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

private:
    PanelDisplayModule() = default;

    enum class Page : uint8_t { List, Detail, Action };

    void wake();
    void sleep();
    void drawList();
    void drawDetail();
    void drawAction();
    void onButton(uint8_t btn, bool longPress);   // 0=+, 1=-, 2=ОК

    /// Человеческое имя проверки (как CHK_NAME веб-панели), иначе сырое.
    const char* friendlyName(const char* checkName) const;

    Adafruit_SSD1306* _disp = nullptr;
    bool     _hwReady = false;
    bool     _awake = false;
    Page     _page = Page::List;
    uint8_t  _sel = 0;              // выбранная проверка
    uint8_t  _scroll = 0;           // первая видимая строка
    uint32_t _lastBtnMs = 0;
    uint16_t _timeoutS = 30;

    // Кнопки
    uint8_t  _pinBtn[3] = { 15, 18, 2 };
    bool     _btnState[3] = { true, true, true };   // отпущена (pullup)
    uint32_t _btnMs[3] = { 0, 0, 0 };
    uint32_t _okDownMs = 0;
    bool     _okLongFired = false;
    bool     _skipRelease = false;   // отпускание кнопки-разбудившей — не действие
};
