// ============================================================================
// WgStatusLedModule.h — СТАТУС-LED ШЛЮЗА (W6): пиксели WS2812 = лицо ПАЗ
// ============================================================================
// Порт эталона мастера (StatusLedModule, M5, утверждён владельцем
// 08.09.2026) с адаптацией дефолтов под шлюз (нота W6, 09.09.2026):
//   · источник — снимок проверок HealthMonitor (ядро 5.8.9: checkNameAt/
//     checkStatusAt/...), модуль ПАЗ не трогает, только отображает;
//   · цвета: зелёный = Ok, жёлтый = Warning, красный = Critical,
//     синий медленный пульс = проверка ещё не бежала («нет данных»),
//     всё погашено = led.enabled=0 (железа нет — модуль молчит, закон
//     самодостаточности);
//   · «мигающая» проверка не дёргает пиксель: статусы уже отфильтрованы
//     подтверждением переходов ядра (5.8.7, paz.confirm_bad/ok);
//   · дефолты шлюза (0.7.1, решение владельца 09.09): пин GPIO2 —
//     одиночный LED не распаян, нога свободна (страппинг: на время
//     UART-прошивки ленту отключать); GPIO5 НЕ существует на гребёнке
//     (эррата шелка IO5/IO35 закрыта: правильный ответ GPIO35, input-only,
//     занят под MISO). 3 пикселя, маппинг wg.radio,wg.bme280,wg.storm.
//   Питание ленты 3.3V (как у мастера), резистор ~470 Ом на data.
//
// Конфиг (группа «Статус-LED», поля CFG_CRITICAL — смена через ребут):
//   led.enabled  BOOL  false — мастер-выключатель (нет ленты = false);
//   led.pin      UINT  2     — data WS2812;
//   led.count    UINT  3     — пикселей (1..8);
//   led.bright   UINT  15    — яркость %, 1..100 (статус, не гирлянда);
//   led.map      STRING "wg.radio,wg.bme280,wg.storm" — проверка i-го
//                пикселя через запятую; пустое имя = пиксель погашен.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <services/IHealthCheck.h>
#include <Adafruit_NeoPixel.h>

class WgStatusLedModule : public ModuleBase {
public:
    static WgStatusLedModule& getInstance();

    const char* getName() const override { return "WgStatusLed"; }
    const char* getVersion() const override { return "0.1.0"; }   // 0.1.0: W6 — порт эталона мастера (3 пикселя, дефолты шлюза)
    ModuleId getModuleId() const override { return 0x1001; }   // wg: 0x1000=WeatherGateApp, 0x1001=WgStatusLed

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 500; }
    // События не нужны: снимок ПАЗ читаем по тику (дешевле подписки).
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

private:
    WgStatusLedModule() = default;

    static constexpr uint8_t MAX_PX = 8;

    void render();   // перерисовать ленту по текущему снимку ПАЗ

    // Найти индекс проверки ПАЗ по имени (маппинг из конфига); -1 — нет.
    int8_t checkIndexByName(const char* name) const;

    Adafruit_NeoPixel* _strip = nullptr;
    uint8_t  _pin = 2;                // GPIO2 свободен (решение владельца 09.09, W6)
    uint8_t  _count = 3;
    uint8_t  _bright = 15;            // %
    char     _map[MAX_PX][20];        // имя проверки на пиксель ("" = off)
    uint8_t  _mapLen = 0;
    bool     _hwReady = false;        // лента инициализирована
    uint32_t _lastPulseMs = 0;
    uint8_t  _pulsePhase = 0;         // фаза «нет данных» (синий пульс)
};
