// ============================================================================
// SmartLockStatusLed.h — СТАТУС-ЛЕНТА WS2812 (профильный модуль smart_lock)
// ============================================================================
// Зеркалит идею WgStatusLed шлюза (ветка weather_gate, 0.7.1) на замок —
// по просьбе владельца 11.09.2026: «может быть и замку добавить эту
// функцию?». Три светодиода, схема утверждена владельцем:
//
//   LED0 «Замок»    — зелёный, пока реле открыто (импульс/TRIGGER-удержание);
//                     погашен, когда замок заперт.
//   LED1 «Кнопка»   — жёлтый постоянный, пока действует ночное ограничение
//                     кнопки выхода (lock.exit_restrict_active);
//                     красная вспышка 3 с при нажатии В ЗАПРЕТЕ
//                     (sl_ev::exitButton, code=0).
//   LED2 «ПАЗ»      — худший статус проверок HealthMonitor:
//                     зелёный = все Ok, жёлтый = есть Warning,
//                     красный = есть Critical.
//
// Загрузка: все три СИНИЕ до первой оценки состояний (ритуал «сначала
// синие, по мере готовности — в рабочие цвета», как у шлюза).
//
// ПИН — ИЗ КОНФИГА (led.pin), по умолчанию GPIO12. В манифест НЕ
// объявляем сознательно: GPIO12 — страппинг (MTDI, при HIGH на буте —
// не стартует), conformance отвергает 0/12. Обоснование безопасности:
// DIN ленты WS2812 — высокоомный вход, на бут пин не влияет; плато
// «3.3V логика на 5V-ленту» здесь закрыто питанием ленты от 3.3V
// (3 светодиода — до ~180 мА, терпимо для AMS1117 платы).
// На WT32-ETH01 других свободных выходов нет (карта пинов профиля:
// заняты 2/4/5/14/15/17/32/33/35/36/39 + ETH).
//
// Всё выключается одним полем led.enabled=false (дефолт ВЫКЛ — как у
// шлюза: «нет ленты?» — молчим и ничего не трогаем).
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <Adafruit_NeoPixel.h>

class SmartLockStatusLed : public ModuleBase {
public:
    static SmartLockStatusLed& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "SlStatusLed"; }
    const char* getVersion() const override { return "0.5.17"; }
    ModuleId getModuleId() const override { return 0x1003; }   // профиль

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 250; }
    void onEvent(int32_t id, const ShEventData* d) override;
    bool canHandleEvent(int32_t id) const override;

private:
    SmartLockStatusLed() = default;

    Adafruit_NeoPixel* _strip = nullptr;   // heap, не BSS (урок outbox)
    bool     _enabled   = false;
    bool     _bootBlue  = true;            // все синие до первой оценки
    uint32_t _deniedFlashUntilMs = 0;      // красная вспышка LED1

    // Цвета (яркость приглушена: лента на виду, не прожектор)
    static uint32_t colOff()    { return Adafruit_NeoPixel::Color(0, 0, 0); }
    static uint32_t colBlue()   { return Adafruit_NeoPixel::Color(0, 0, 24); }
    static uint32_t colGreen()  { return Adafruit_NeoPixel::Color(0, 24, 0); }
    static uint32_t colYellow() { return Adafruit_NeoPixel::Color(20, 12, 0); }
    static uint32_t colRed()    { return Adafruit_NeoPixel::Color(28, 0, 0); }
};
