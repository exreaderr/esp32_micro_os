// ============================================================================
// ScheduleService.cpp — реализация планировщика временных правил
// ============================================================================
#include "ScheduleService.h"
#include "ConfigService.h"
#include "TimeService.h"
#include "../core/Events.h"
#include "../core/EventBus.h"

ScheduleService& ScheduleService::getInstance() {
    static ScheduleService instance;
    return instance;
}

// ============================================================================
// КОНФИГ-СХЕМА (авто-UI админки, группа «Планировщик»)
// ============================================================================
void ScheduleService::registerExtensions() {
    ConfigService::getInstance().addFields("Планировщик", {
        { "sched.enabled", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Планировщик", "Планировщик включён" },
        // Формат правила — в подсказке первого поля (одна строка, поля
        // через '|'): имя|С ЧЧ:ММ|ПО ЧЧ:ММ|дни 1..7 или *|код 1..255.
        // Пустое «ПО» — точечное срабатывание (будильник).
        { "sched.rule0", ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Планировщик", "Правило 1: имя|С|ПО|дни|код (ПО пусто — точка)" },
        { "sched.rule1", ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Планировщик", "Правило 2" },
        { "sched.rule2", ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Планировщик", "Правило 3" },
        { "sched.rule3", ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Планировщик", "Правило 4" },
    });
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void ScheduleService::init() {
    _initialized = true;
    log(LogLevel::Info, "init: rules pending (first tick)");
}

void ScheduleService::start() {
    // Перезагрузка правил при смене sched.* — через событие конфига,
    // без опроса NVS (принцип: реакция на факт, а не polling).
    EventBus::getInstance().subscribe(CFG_EVENT_CHANGED, this);
    _rulesDirty = true;
    _started = true;
    log(LogLevel::Info, "start");
}

void ScheduleService::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    _started = false;
    log(LogLevel::Info, "stop");
}

// ============================================================================
// TICK — минутное вычисление
// ============================================================================
void ScheduleService::tick() {
    if (!_started || !cfgGetBool("sched.enabled", true)) return;

    if (_rulesDirty) reloadRules();

    TimeService& ts = TimeService::getInstance();
    struct tm lt;
    const bool valid = ts.isTimeValid() && ts.getLocalTime(lt);

    if (!valid) {
        // Fail-Safe: недостоверное время — ни одного активного периода.
        // Выходы эмитим ОДИН раз на переходе, не каждую секунду.
        if (_wasTimeValid) emitAllExits();
        _wasTimeValid = false;
        _lastMin = -1;
        return;
    }

    const int nowMin = lt.tm_hour * 60 + lt.tm_min;
    if (_wasTimeValid && nowMin == _lastMin) return;  // та же минута

    const bool justValid = !_wasTimeValid;  // время появилось — фронты
                                            // от состояния «всё выключено»
    _wasTimeValid = true;
    _lastMin = (int16_t)nowMin;
    evaluate(nowMin, lt.tm_wday, justValid);
}

void ScheduleService::evaluate(int nowMin, int tmWday, bool justValid) {
    for (uint8_t i = 0; i < _ruleCount; ++i) {
        SchedRule& r = _rules[i];
        if (r.type == SCHED_RULE_INTERVAL) {
            bool now = false;
            const bool was = justValid ? false : _active[i];
            SchedEdge edge = sched::intervalEdge(was, r, nowMin, tmWday, now);
            _active[i] = now;
            if (edge == SCHED_EDGE_ENTER) {
                postPeriodEvent(SCHED_EVENT_PERIOD_CHANGED, r, r.periodCode);
                log(LogLevel::Info, "enter '%s' (code=%u)", r.name, r.periodCode);
            } else if (edge == SCHED_EDGE_EXIT) {
                postPeriodEvent(SCHED_EVENT_PERIOD_CHANGED, r, 0);
                log(LogLevel::Info, "exit '%s'", r.name);
            }
        } else {   // SCHED_RULE_POINT
            if (sched::pointDue(r, nowMin, tmWday)) {
                if (!_pointArmed[i]) {
                    _pointArmed[i] = true;
                    postPeriodEvent(SCHED_EVENT_RULE_FIRED, r, r.periodCode);
                    log(LogLevel::Info, "fire '%s' (code=%u)", r.name, r.periodCode);
                }
            } else {
                _pointArmed[i] = false;   // перевзвод на следующие сутки
            }
        }
    }
}

void ScheduleService::emitAllExits() {
    for (uint8_t i = 0; i < _ruleCount; ++i) {
        if (_rules[i].type == SCHED_RULE_INTERVAL && _active[i]) {
            postPeriodEvent(SCHED_EVENT_PERIOD_CHANGED, _rules[i], 0);
            log(LogLevel::Info, "exit '%s' (time lost)", _rules[i].name);
        }
        _active[i] = false;
        _pointArmed[i] = false;
    }
}

// ============================================================================
// ПРАВИЛА: конфиг + провайдер
// ============================================================================
void ScheduleService::reloadRules() {
    _rulesDirty = false;
    _ruleCount = 0;
    uint8_t bad = 0;

    char buf[SCHED_RULE_STR_LEN];
    for (uint8_t i = 0; i < SCHED_CFG_RULES; ++i) {
        char key[12];
        snprintf(key, sizeof(key), "sched.rule%u", (unsigned)i);
        cfgGetStr(key, buf, sizeof(buf), "");
        if (!buf[0]) continue;                 // пустое поле — дыра, не ошибка
        SchedRule r;
        if (sched::parseRule(buf, r) && _ruleCount < SCHED_MAX_RULES) {
            _rules[_ruleCount++] = r;
        } else {
            ++bad;
            log(LogLevel::Warning, "bad rule '%s' in %s", buf, key);
        }
    }

    if (_provider) {
        const uint8_t n = _provider->getScheduleRuleCount();
        for (uint8_t i = 0; i < n; ++i) {
            SchedRule r;
            if (_provider->getScheduleRule(i, r) &&
                _ruleCount < SCHED_MAX_RULES) {
                r.enabled = true;
                _rules[_ruleCount++] = r;
            } else {
                ++bad;
            }
        }
    }

    _dropped = bad;
    // Состояния не сбрасываем: при переименовании/замене правила худший
    // случай — один лишний/пропущенный фронт на границе периода.
    log(LogLevel::Info, "rules: %u active, %u dropped", _ruleCount, _dropped);
}

void ScheduleService::setProvider(IScheduleProvider* p) {
    _provider = p;
    _rulesDirty = true;   // перечитаем на ближайшем tick (и до start() —
                          // флаг переживает, start его не трогает)
}

// ============================================================================
// СОБЫТИЯ
// ============================================================================
void ScheduleService::postPeriodEvent(int32_t eventId, const SchedRule& r,
                                      int32_t code) {
    ShEventData d; d.clear();
    d.code = code;
    safeStrCopy(d.payload, sizeof(d.payload), r.name);
    postEvent(eventId, &d);
}

void ScheduleService::onEvent(int32_t eventId, const ShEventData* data) {
    if (eventId != CFG_EVENT_CHANGED || !data) return;
    // payload — ключ параметра; реагируем только на свои (sched.*).
    if (strncmp(data->payload, "sched.", 6) == 0) _rulesDirty = true;
}

bool ScheduleService::canHandleEvent(int32_t eventId) const {
    return eventId == CFG_EVENT_CHANGED;
}

// ============================================================================
// API ОПРОСА
// ============================================================================
bool ScheduleService::isRuleActive(const char* name) const {
    if (!name || !_wasTimeValid) return false;
    for (uint8_t i = 0; i < _ruleCount; ++i)
        if (_active[i] && strcmp(_rules[i].name, name) == 0) return true;
    return false;
}

bool ScheduleService::isPeriodActive(uint8_t periodCode) const {
    if (!periodCode || !_wasTimeValid) return false;
    for (uint8_t i = 0; i < _ruleCount; ++i)
        if (_active[i] && _rules[i].periodCode == periodCode) return true;
    return false;
}
