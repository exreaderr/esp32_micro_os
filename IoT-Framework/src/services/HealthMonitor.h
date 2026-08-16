// ============================================================================
// HealthMonitor.h — ПАЗ-ЯДРО МикроОС 5.0 (ПротивоАварийная Защита)
// ============================================================================
// Фаза 1. Наследник PazManager v4.2.2, очищенный от доменной логики:
//   · WDT 10 с на задаче loop (монолит: WDT оба ядра);
//   · исполнение зарегистрированных проверок IHealthCheck по расписанию;
//   · системные проверки из коробки: heap, температура кристалла
//     (подписка на EspTempDriver), живость шины и драйверов;
//   · вердикты -> события HEALTH_EVENT_* -> лог/HA/телеметрия;
//   · аудит: счётчики warning/critical (Phase 2 — в AuditLog, B3).
//
// Чего здесь НЕТ (урок v4.2.2): залипание реле, дверь, карты — доменные
// проверки регистрирует профиль через registerCheck(). ПАЗ не знает, ЧТО
// проверяет — только КАК часто и что делать с вердиктом.
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "IHealthCheck.h"
#include <esp_task_wdt.h>

// Бюджеты
constexpr uint8_t  HM_MAX_CHECKS     = 16;   // проверок в системе
constexpr uint32_t HM_WDT_TIMEOUT_S  = 10;   // WDT 10 с (монолит v2.5.0)
constexpr uint32_t HM_HEAP_CHECK_MS  = 30000;// контроль heap раз в 30 с
constexpr uint32_t HM_HEAP_WARN_BYTES= 20000;// ТЕКУЩИЙ heap ниже — WARNING
constexpr uint32_t HM_HEAP_CRIT_BYTES= 10000;// ТЕКУЩИЙ heap ниже — CRITICAL
// Семантика 5.8.0 (урок ночи 14→15.08.2026: нырок до 5332 Б в 19:38
// защёлкнул CRITICAL по watermark и долбил событие каждые 30 с всю ночь —
// 1488 строк шума в журнале М3, «крит: 211» на панели). Теперь:
//   · СОСТОЯНИЕ — по ТЕКУЩЕМУ свободному объёму с гистерезисом, события
//     только на переходах (устойчивое истощение = эскалация);
//   · watermark (since-boot минимум) — ОДНОРАЗОВЫЙ факт при новом минимуме
//     с шагом против микрокасаний (транзиентный нырок = запись в журнале
//     паник, а не вечная тревога).
constexpr uint32_t HM_HEAP_HYST_BYTES    = 4096; // гистерезис возврата
constexpr uint32_t HM_HEAP_MIN_STEP_BYTES= 1024; // новый минимум < −1 КБ
// Журнал паник + дежурные датчиков (залежь №2, Phase 3)
constexpr uint8_t  HM_JOURNAL_SIZE   = 10;   // глубина кольца журнала паник
constexpr uint8_t  HM_WATCH_SLOTS    = 4;    // дежурных «залипших датчиков»
constexpr uint32_t HM_WATCH_CHECK_MS = 5000; // период опроса дежурных

/// Запись журнала паник. Хранится кольцом в RAM + NDJSON на LittleFS
/// (/paz_journal.ndjson): авария, пережившая ребут, расскажет о себе сама.
struct PazJournalEntry {
    uint32_t unix;        // абсолютное время (0 — часы ещё не выставлены)
    uint32_t uptime_s;    // аптайм на момент события
    uint16_t heap_kb;     // свободная heap на момент события
    char     src[20];     // источник: "heap", "cpu_temp", имя проверки/дежурного
    char     msg[64];     // суть (без кавычек — журнал сам их санирует;
                          // 64 — кириллица ест по 2 байта/символ, 44 резало слова)
};

class HealthMonitor : public ModuleBase {
public:
    static HealthMonitor& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "HealthMonitor"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0007; }

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 100; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- ТОЧКА РАСШИРЕНИЯ ---------------------------------------------------
    /// Регистрация проверки (вызывается из registerExtensions модулей).
    /// Владение объектом остаётся у регистранта; объект должен жить вечно
    /// (статический) — ПАЗ его не удаляет.
    bool registerCheck(IHealthCheck* check);

    /// Сводка ПАЗ для /api/health (вкладка панели): проверки со статусами,
    /// WDT, счётчики, heap, хвост журнала паник. Возвращает длину JSON.
    uint16_t reportJson(char* buf, size_t n) const;

    // --- ЖУРНАЛ ПАНИК (залежь №2) ------------------------------------------
    /// Фиксация аварии: кольцо в RAM + строка в /paz_journal.ndjson.
    /// Вызывают: критические переходы проверок, heap-авария, термопаника,
    /// зависший дежурный датчик (critical). Может звать и профиль — для
    /// доменных катастроф (не для рядовых предупреждений!).
    void journalAdd(const char* src, const char* msg);
    /// Копия журнала наружу, НОВЫЕ ПЕРВЫМИ. Возвращает число записей.
    size_t journalCopy(PazJournalEntry* out, size_t max) const;

    // --- ДЕЖУРНЫЙ ДАТЧИКОВ (sensor-stuck, залежь №2) ------------------------
    /// Сторож «залипшего» значения: живой датчик ОБЯЗАН менять показания;
    /// зависший страшнее мёртвого — мёртвый виден, зависший врёт.
    /// Указатель на сырое целое (int16: cpuTenths и т.п.) — сравнение
    /// побитово, без float-подводных камней. Значение не меняется дольше
    /// maxStuckMs -> событие (warning/critical) + журнал для critical.
    bool registerSensorWatch(const char* name, const int16_t* rawPtr,
                             uint32_t maxStuckMs, bool critical);

    // --- СВОДНОЕ СОСТОЯНИЕ ------------------------------------------------------
    bool isSystemHealthy() const { return _criticalCount == 0; }
    uint8_t warningCount() const { return _warningCount; }
    uint8_t criticalCount() const { return _criticalCount; }

private:
    HealthMonitor() = default;

    // --- Встроенная проверка heap (системная, всегда первая) -----------------
    void checkHeap();

    // --- Исполнение зарегистрированных проверок --------------------------------
    void runChecks();

    struct CheckSlot {
        IHealthCheck* check;
        uint32_t      lastRunMs;
        HealthResult::Status lastStatus;   // для событий переходов
        char          lastMsg[32];         // последнее сообщение (для /api/health)
    };

    CheckSlot _checks[HM_MAX_CHECKS];
    uint8_t   _checkCount = 0;

    // --- Журнал паник (RAM-кольцо; диск — /paz_journal.ndjson) ---------------
    void loadJournal();               // загрузка с диска + компакция (len>10)
    void journalPersist(const PazJournalEntry& e);

    PazJournalEntry _journal[HM_JOURNAL_SIZE];
    uint8_t  _journalHead = 0;        // слот СЛЕДУЮЩЕЙ записи
    uint8_t  _journalCount = 0;
    bool     _journalLoaded = false;  // ленивая загрузка (порядок модулей)

    // --- Дежурные датчиков -----------------------------------------------------
    struct WatchSlot {
        const char*    name;
        const int16_t* rawPtr;
        int16_t        lastRaw;
        uint32_t       lastChangeMs;
        uint32_t       maxStuckMs;
        bool           critical;
        bool           stuck;
    };
    void checkWatches(uint32_t now);

    WatchSlot _watches[HM_WATCH_SLOTS];
    uint8_t   _watchCount = 0;
    uint32_t  _lastWatchCheckMs = 0;

    uint32_t  _lastHeapCheckMs = 0;
    // Heap: стейт-машина по ТЕКУЩЕМУ free (0=Ok 1=Warn 2=Crit) и
    // последний доложенный watermark (one-shot «новый минимум» с шагом)
    uint8_t   _heapState = 0;
    uint32_t  _heapMinReported = 0xFFFFFFFFUL;
    uint8_t   _warningCount = 0;
    uint8_t   _criticalCount = 0;
    bool      _wdtArmed = false;
};
