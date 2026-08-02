// ============================================================================
// WiegandDriver.h — УНИВЕРСАЛЬНЫЙ СЧИТЫВАТЕЛЬ WIEGAND (26–56 бит)
// ============================================================================
// СТАТУС: драйвер КАТАЛОГА ПРОФИЛЬНОЙ ПЕРИФЕРИИ (profiles/drivers/), НЕ ядра.
// Критерий классификации: Wiegand-считыватель нужен только устройствам
// со считывателем (СКУД, ворота, сейф) — большинству экосистемы он не нужен,
// поэтому в неизменное ядро не входит и flash устройств без него не ест.
// Подключение: профиль регистрирует его в DriverRegistry и вызывает
// configure() со своими пинами (образец — profiles/smart_lock).
//
// Цепочка: фронт на D0/D1 (ISR) -> накопление бит -> poll() ->
//   декодер форматов -> EventBus.post(DRV_EVENT_WIEGAND_CARD).
//
// УНИВЕРСАЛЬНОСТЬ (требование: один драйвер на все реализации протокола):
//   Протокол Wiegand — это не один формат, а семейство. Все варианты
//   описываются единой схемой:
//
//     [P_even][ headCover бит ][ body ][ tailCover бит ][P_odd]
//
//   · ведущий бит — ЧЁТНЫЙ паритет над следующими headCover битами;
//   · замыкающий  — НЕЧЁТНЫЙ паритет над предыдущими tailCover битами;
//   · 37-битный H10302 — без паритетов вообще (голые данные).
//
//   Поэтому драйвер хранит ТАБЛИЦУ ДЕСКРИПТОРОВ, а не if'ы форматов.
//   Новый экзотический формат = одна строка в таблице, код не трогаем.
//
// Тайминги/антидребезг/антишум — из монолита smart_lock v2.5.0 (часть 1):
//   кадр 25 мс, окно повтора 1500 мс, наводки < 4 бит.
// ============================================================================
#pragma once

#include "../../core/IDeviceDriver.h"
#include "../../core/EventBus.h"
#include "WiegandFormats.h"   // чистая логика форматов (D2: host-тесты)
#include <Arduino.h>

// Таблица форматов, WiegandCard и декодер перенесены в WiegandFormats.h —
// чистый C++ без зависимостей, тестируется на хосте (D2). Здесь остаются
// только делегаты, сохраняющие прежний API драйвера.

class WiegandDriver : public IDeviceDriver {
public:
    // --- КОНФИГУРАЦИЯ (заполняет профиль до фазы init) ----------------------
    struct Config {
        uint32_t frameTimeoutMs = 25;    // таймаут кадра (монолит: 25 мс)
        uint32_t repeatWindowMs = 1500;  // антидребезг той же карты
        uint8_t  minNoiseBits   = 4;     // кадры короче — наводки
        bool     strictParity   = true;  // false — принимать карту даже при
                                         // ошибке паритета (дёшевые считыватели;
                                         // событие всё равно помечается)
        bool     rawFallback    = false; // true — неизвестные длины кадров
                                         // публиковать как сырые данные
                                         // (по умолчанию — в шум)
    };

    static WiegandDriver& getInstance();

    /// Привязка к пинам/параметрам из манифеста профиля.
    void configure(uint8_t pinD0, uint8_t pinD1, const Config& cfg);

    // --- IDeviceDriver ---------------------------------------------------
    const char* driverName() const override { return "wiegand"; }
    bool init() override;
    void poll() override;
    uint32_t getPollIntervalMs() const override { return 5; }
    bool isHealthy() const override { return _healthy; }

    // --- ДЕКОДЕР (public: host-тесты и сервисы могут декодировать без ISR) ---
    /// Распознать формат и проверить паритет кадра.
    static WiegandCard decode(uint64_t bits, uint8_t count);

    /// Поиск формата по длине кадра. nullptr — длина неизвестна.
    static const WiegandFormat* findFormat(uint8_t totalBits);

private:
    WiegandDriver() = default;

    // --- ISR (IRAM): только накопление бит ----------------------------------
    static void IRAM_ATTR isrD0();
    static void IRAM_ATTR isrD1();
    inline void IRAM_ATTR onBit(uint8_t bit);

    // --- ОБРАБОТКА КАДРА (poll, контекст задачи) ----------------------------
    void processFrame(uint64_t bits, uint8_t count);
    void publishCard(const WiegandCard& card);

    // --- ДАННЫЕ ISR -----------------------------------------------------------
    volatile uint64_t _isrBits;
    volatile uint8_t  _isrCount;
    volatile uint32_t _isrLastBitMs;

    // --- КОНФИГ/СОСТОЯНИЕ ------------------------------------------------------
    Config _cfg;
    uint8_t _pinD0 = 0xFF;
    uint8_t _pinD1 = 0xFF;
    bool    _configured = false;
    bool    _healthy = false;

    // Антидребезг карты
    uint64_t _lastCardData = 0;
    uint32_t _lastCardMs = 0;
};
