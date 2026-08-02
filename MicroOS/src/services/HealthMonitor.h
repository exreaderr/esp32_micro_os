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
constexpr uint32_t HM_HEAP_WARN_BYTES= 20000;// heap ниже — WARNING
constexpr uint32_t HM_HEAP_CRIT_BYTES= 10000;// heap ниже — CRITICAL

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
    /// WDT, счётчики, heap. Возвращает длину записанного JSON.
    uint16_t reportJson(char* buf, size_t n) const;

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

    uint32_t  _lastHeapCheckMs = 0;
    uint8_t   _warningCount = 0;
    uint8_t   _criticalCount = 0;
    bool      _wdtArmed = false;
};
