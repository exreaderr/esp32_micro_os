// ============================================================================
// StatusLedModule.h — СТАТУС-LED МАСТЕРА (M5): 4 пикселя WS2812 = лицо ПАЗ
// ============================================================================
// Дизайн (утверждён владельцем 08.09.2026):
//   · 4 пикселя WS2812 на GPIO21 (питание 3.3V — порог data 2.31V чисто;
//     резистор ~470 Ом на data), назначение пикселей — В КОНФИГЕ, не в коде;
//   · источник — снимок проверок HealthMonitor (ядро 5.8.9: checkNameAt/
//     checkStatusAt/...), модуль ПАЗ не трогает, только отображает;
//   · цвета: зелёный = Ok, жёлтый = Warning, красный = Critical,
//     синий медленный пульс = проверка ещё не бежала («нет данных»),
//     всё погашено = led.enabled=0 (железа нет — модуль молчит, закон
//     самодостаточности: устройство без ленты работает полной программой).
//   · «мигающая» проверка не дёргает пиксель: статусы уже отфильтрованы
//     подтверждением переходов ядра (5.8.7, paz.confirm_bad/ok).
//
// Конфиг (группа «Статус-LED», поля CFG_CRITICAL — смена пинов/маппинга
// через ребут):
//   led.enabled  BOOL  false — мастер-выключатель (нет ленты = false);
//   led.pin      UINT  21    — data WS2812;
//   led.count    UINT  4     — пикселей (1..8);
//   led.bright   UINT  15    — яркость %, 1..100 (статус, не гирлянда);
//   led.map      STRING "hm.sd,hm.bk,hm.ota,hm.fleet" — проверка i-го
//                пикселя через запятую; пустое имя = пиксель погашен.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <services/IHealthCheck.h>
#include <Adafruit_NeoPixel.h>

class StatusLedModule : public ModuleBase {
public:
    static StatusLedModule& getInstance();

    const char* getName() const override { return "StatusLed"; }
    const char* getVersion() const override { return "0.1.0"; }   // 0.1.0: M5 — 4 пикселя WS2812 по снимку ПАЗ (ядро 5.8.9)
    ModuleId getModuleId() const override { return 0x1109; }   // hm: ... 0x1108=OtaMirror, 0x1109=StatusLed, 0x110A=PanelDisplay

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 500; }
    // События не нужны: снимок ПАЗ читаем по тику (дешевле подписки).
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

private:
    StatusLedModule() = default;

    static constexpr uint8_t MAX_PX = 8;

    void render();   // перерисовать ленту по текущему снимку ПАЗ

    // Найти индекс проверки ПАЗ по имени (маппинг из конфига); -1 — нет.
    int8_t checkIndexByName(const char* name) const;

    Adafruit_NeoPixel* _strip = nullptr;
    uint8_t  _pin = 21;
    uint8_t  _count = 4;
    uint8_t  _bright = 15;              // %
    char     _map[MAX_PX][20];          // имя проверки на пиксель ("" = off)
    uint8_t  _mapLen = 0;
    bool     _hwReady = false;          // лента инициализирована
    uint32_t _lastPulseMs = 0;
    uint8_t  _pulsePhase = 0;           // фаза «нет данных» (синий пульс)
};
