// ============================================================================
// CounterService.h — ПЕРСИСТЕНТНЫЕ СЧЁТЧИКИ (RAM-тень + батч-NVS + PCNT)
// ============================================================================
// Ядерный бэклог 5.1.0. Единый владелец «долгоживущих» чисел: циклы замка,
// наработка реле, показания импульсных счётчиков (вода/газ/электричество
// в будущих профилях). Заменяет идиому «cfgGetUInt + setInternal на каждый
// инкремент» — та жгла flash на каждом цикле замка (см. CounterCore.h).
//
// Механика:
//   · значение живёт в RAM; инкремент — O(1), без flash;
//   · сброс в NVS батчами: каждые cnt.flush_every инкрементов ИЛИ каждые
//     cnt.flush_interval_s секунд (что наступит раньше), плюс принудительно
//     по SH_EVENT_SHUTDOWN — штатный ребут ничего не теряет;
//   · потолок потерь при обесточке: min(N инкрементов, T секунд) — честная
//     цена за ресурс flash, задокументирована здесь;
//   · два рода счётчиков:
//       - cfgBound (incrementCfg): теневое значение конфиг-ключа
//         (lock.cycle_count). Панель/API/бэкап видят поле как раньше —
//         миграции нет, просто пишется реже;
//       - автономный (increment): namespace "cnt" в NVS (A2: регистрируется
//         в ResourceManager);
//   · PCNT-бэкенд (attachPcnt): импульсы считает АППАРАТНЫЙ блок ESP32 без
//     CPU и прерываний; сервис раз в секунду снимает wrap-safe дельту
//     (CounterCore.h) и дописывает в счётчик. Пин проходит через
//     ResourceManager::claimGpio — конфликт с профилем = отказ.
//
// Чего сервис НЕ ДЕЛАЕТ: тарификация, суточные обнуляторы, MQTT-отправка
// показаний — политика профилей поверх API.
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "CounterCore.h"

constexpr uint32_t CNT_TICK_MS = 1000;   // поллинг PCNT + проверка политики

class CounterService : public ModuleBase {
public:
    static CounterService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "CounterService"; }
    const char* getVersion() const override { return "5.1.0"; }
    ModuleId getModuleId() const override { return 0x0010; }

    void registerExtensions() override;   // схема cnt.* (группа «Счётчики»)
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return CNT_TICK_MS; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- СЧЁТЧИКИ, ПРИВЯЗАННЫЕ К КОНФИГ-КЛЮЧУ ------------------------------
    /// Инкремент теневого значения конфиг-поля (например lock.cycle_count).
    /// Первое обращение лениво читает базу из ConfigService. Возвращает
    /// новое RAM-значение (0 — таблица счётчиков переполнена, инкремент
    /// ПОТЕРЯН: смотреть droppedIncrements()).
    uint32_t incrementCfg(const char* cfgKey, uint32_t delta = 1);

    /// Текущее значение (RAM-тень; в NVS может отставать на pending).
    uint32_t valueCfg(const char* cfgKey);

    // --- АВТОНОМНЫЕ СЧЁТЧИКИ (NVS namespace "cnt") --------------------------
    uint32_t increment(const char* name, uint32_t delta = 1);
    uint32_t value(const char* name);

    /// Сброс в ноль: пишет в NVS НЕМЕДЛЕННО (редкая операция, батч не нужен).
    bool reset(const char* name);

    // --- PCNT-БЭКЕНД --------------------------------------------------------
    /// Считать импульсы на gpio аппаратным блоком PCNT (фронт нарастающий).
    /// glitchFilterNs — аппаратный антидребезг (0 — выкл; максимум ~12.8 мкс
    /// — для геркона дополнительно программное подавление в профиле).
    /// false — нет свободных юнитов/счётчиков, пин занят (ResourceManager),
    /// имя длиннее 15 символов.
    bool attachPcnt(const char* name, int8_t gpio, uint16_t glitchFilterNs = 10000);

    // --- ДИАГНОСТИКА ---------------------------------------------------------
    uint8_t  counterCount() const { return _count; }
    uint32_t flushCount() const { return _flushes; }           // записей в NVS
    uint32_t droppedIncrements() const { return _dropped; }    // потеряно (переполнение)
    uint32_t pendingTotal() const;                             // незаписанных инкрементов

private:
    CounterService() = default;

    struct Entry {
        char     name[CNT_NAME_LEN];  // cfg-ключ или имя автономного
        uint32_t ram;                 // теневое значение
        uint32_t pending;             // незаписанных инкрементов
        uint32_t lastFlushMs;         // millis() последнего сброса
        int8_t   pcntGpio;            // -1 — PCNT не привязан
        int16_t  pcntLast;            // последнее аппаратное чтение
        int8_t   pcntUnit;            // -1 — юнит не выделен
        bool     cfgBound;            // писать через ConfigService
        bool     used;
    };

    Entry* find(const char* name);
    /// Найти или завести запись; cfgBound выбирает источник базы при ленивой
    /// загрузке (ConfigService / NVS "cnt"). nullptr — таблица полна.
    Entry* findOrCreate(const char* name, bool cfgBound);

    void pollPcnt(Entry& e);              // снять дельту аппаратного блока
    void addDelta(Entry& e, uint32_t delta);  // общий путь инкремента
    void flushEntry(Entry& e, uint32_t nowMs);
    void flushAll(uint32_t nowMs);        // SH_EVENT_SHUTDOWN
    void reloadCfgBase(Entry& e);         // CFG_EVENT_CHANGED по ключу

    Entry    _entries[CNT_MAX_COUNTERS];
    uint8_t  _count = 0;
    uint32_t _flushes = 0;
    uint32_t _dropped = 0;
};
