// ============================================================================
// ScheduleService.h — ПЛАНИРОВЩИК ВРЕМЕННЫХ ПРАВИЛ (доменный сервис ядра)
// ============================================================================
// Ядерный бэклог 5.1.0. Реализует «Scheduler» из таблицы модулей базовой
// архитектуры: «временные интервалы: ночной режим, рабочие часы, будильники,
// cron-подобные задачи. Подписчик TimeService». В монолите v4.2.2 эта
// логика была размазана: isNightTime/isWorkingHours внутри RTCManager,
// тихие часы — внутри AudioManager, запрет кнопки выхода — внутри
// ButtonManager. Здесь — ОДИН владелец факта «какой сейчас период»
// (принцип 6), остальные подписываются на события или опрашивают API.
//
// Механика:
//   · правила — из конфига (sched.rule0..3, группа «Планировщик», авто-UI)
//     и/или от IScheduleProvider профиля; формат строки — см. ScheduleCore.h;
//   · вычисление — раз в минуту (tick 1 с, выход при неизменной минуте),
//     вся математика — чистая, в ScheduleCore.h (host-тесты);
//   · фронт интервала -> SCHED_EVENT_PERIOD_CHANGED (code: 0=вышли,
//     N=код периода при входе; payload: имя правила);
//   · точечное правило -> SCHED_EVENT_RULE_FIRED (один раз в свою минуту);
//   · Fail-Safe: время недостоверно (RTC мёртв, NTP не было) -> ВСЕ периоды
//     неактивны; выходы эмитятся один раз, точечные правила не стреляют.
//     При восстановлении времени — входы по текущему состоянию (подписчики
//     узнают действующий период без опроса).
//
// Чего сервис НЕ ДЕЛАЕТ: сам ничего не включает и не выключает — это
// датчик периодов. Политика (громкость, запреты, реле) — у подписчиков.
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "ScheduleCore.h"
#include "IScheduleProvider.h"

constexpr uint32_t SCHED_TICK_MS = 1000;   // минутное вычисление на сек. сетке

class ScheduleService : public ModuleBase {
public:
    static ScheduleService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "ScheduleService"; }
    const char* getVersion() const override { return "5.1.0"; }
    ModuleId getModuleId() const override { return 0x000F; }

    void registerExtensions() override;   // схема sched.* (группа «Планировщик»)
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return SCHED_TICK_MS; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- ТОЧКА РАСШИРЕНИЯ ПРОФИЛЯ ----------------------------------------
    /// Подключить провайдера статических правил (см. IScheduleProvider.h).
    /// До start(); nullptr — отключить. Перезагружает таблицу правил.
    void setProvider(IScheduleProvider* p);

    // --- API СЕРВИСА (опрос вместо подписки) -------------------------------
    /// Активен ли период с таким именем СЕЙЧАС ("ночь"). false — правила
    /// нет, оно выключено или время недостоверно.
    bool isRuleActive(const char* name) const;

    /// Активен ли период с таким кодом (1..255).
    bool isPeriodActive(uint8_t periodCode) const;

    // --- ДИАГНОСТИКА (веб/MQTT, Фаза 3) -------------------------------------
    uint8_t ruleCount() const { return _ruleCount; }
    const SchedRule* ruleAt(uint8_t i) const {
        return (i < _ruleCount) ? &_rules[i] : nullptr;
    }
    bool ruleStateAt(uint8_t i) const { return i < _ruleCount && _active[i]; }
    uint8_t droppedRules() const { return _dropped; }  // не влезло/битые строки

private:
    ScheduleService() = default;

    /// Перечитать правила из конфига + провайдера (по флагу _rulesDirty).
    void reloadRules();

    /// Минутное вычисление: фронты и точечные срабатывания.
    void evaluate(int nowMin, int tmWday, bool timeJustValid);

    /// Время стало недостоверным: выход из всех активных периодов (1 раз).
    void emitAllExits();

    void postPeriodEvent(int32_t eventId, const SchedRule& r, int32_t code);

    SchedRule  _rules[SCHED_MAX_RULES];
    bool       _active[SCHED_MAX_RULES]     = {};  // интервалы: состояние
    bool       _pointArmed[SCHED_MAX_RULES] = {};  // точечные: уже стреляло
                                                   // в эту минуту
    uint8_t    _ruleCount  = 0;
    uint8_t    _dropped    = 0;        // битые строки + переполнение таблицы
    bool       _rulesDirty = true;     // перечитать на ближайшем tick
    int16_t    _lastMin    = -1;       // последняя вычисленная минута
    bool       _wasTimeValid = false;
    IScheduleProvider* _provider = nullptr;
};
