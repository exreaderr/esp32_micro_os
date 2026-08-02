// ============================================================================
// HealthMonitor.cpp — реализация ПАЗ-ядра
// ============================================================================
#include "HealthMonitor.h"
#include "../core/Events.h"
#include "../core/DriverRegistry.h"
#include <esp_heap_caps.h>

HealthMonitor& HealthMonitor::getInstance() {
    static HealthMonitor instance;
    return instance;
}

// ============================================================================
// INIT/START: WDT на задаче loop (монолит: 10 с, оба ядра)
// ============================================================================
void HealthMonitor::init() {
    ensureMutex();
    _initialized = true;
}

void HealthMonitor::start() {
    EventBus& bus = EventBus::getInstance();
    // Подписки на системные факты (драйверы публикуют, ПАЗ оценивает)
    bus.subscribe(DRV_EVENT_TEMP_WARNING, this);
    bus.subscribe(DRV_EVENT_TEMP_CRITICAL, this);
    bus.subscribe(SH_EVENT_TICK_OVERRUN, this);

    // --- WDT: контролируем задачу loop() (Arduino loopTask). ---------------
    // Если loop зависнет дольше 10 с (любой модуль в tick/onEvent) — ребут.
    // ESP-IDF 5.x (core 3.x): init принимает конфиг-структуру (а не
    // (timeout, panic) как в 4.x). Если Arduino уже инициализировал TWDT —
    // ESP_ERR_INVALID_STATE, тогда просто переконфигурируем.
    esp_task_wdt_config_t wdtCfg = {
        .timeout_ms     = HM_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,        // idle-задачи не контролируем
        .trigger_panic  = true      // таймаут -> паника -> ребут (монолит)
    };
    esp_err_t rc = esp_task_wdt_init(&wdtCfg);
    if (rc == ESP_ERR_INVALID_STATE) {
        rc = esp_task_wdt_reconfigure(&wdtCfg);
    }
    if (rc == ESP_OK && esp_task_wdt_add(NULL) == ESP_OK) _wdtArmed = true;
    log(LogLevel::Info, "started, WDT %lus %s", (unsigned long)HM_WDT_TIMEOUT_S,
        _wdtArmed ? "armed" : "FAILED");
    _started = true;
}

void HealthMonitor::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    if (_wdtArmed) {
        esp_task_wdt_delete(NULL);
        _wdtArmed = false;
    }
    _started = false;
}

bool HealthMonitor::canHandleEvent(int32_t eventId) const {
    return eventId == DRV_EVENT_TEMP_WARNING ||
           eventId == DRV_EVENT_TEMP_CRITICAL ||
           eventId == SH_EVENT_TICK_OVERRUN;
}

// ============================================================================
// СОБЫТИЯ: трансляция системных фактов в вердикты ПАЗ
// ============================================================================
void HealthMonitor::onEvent(int32_t eventId, const ShEventData* data) {
    ShEventData d; d.clear();
    switch (eventId) {
        case DRV_EVENT_TEMP_WARNING:
            _warningCount++;
            safeStrCopy(d.payload, sizeof(d.payload), "CPU_TEMP_WARNING");
            if (data) d.code = data->code;   // температура x10
            postEvent(HEALTH_EVENT_WARNING, &d);
            break;
        case DRV_EVENT_TEMP_CRITICAL:
            _criticalCount++;
            safeStrCopy(d.payload, sizeof(d.payload), "CPU_TEMP_CRITICAL");
            if (data) d.code = data->code;
            postEvent(HEALTH_EVENT_CRITICAL, &d);
            break;
        case SH_EVENT_TICK_OVERRUN:
            _warningCount++;
            safeStrCopy(d.payload, sizeof(d.payload), "TICK_BUDGET_EXCEEDED");
            if (data) d.code = data->code;   // длительность, мс
            postEvent(HEALTH_EVENT_WARNING, &d);
            break;
    }
}

// ============================================================================
// TICK: WDT-reset + heap + зарегистрированные проверки
// ============================================================================
void HealthMonitor::tick() {
    // Подтверждение жизни loop-задачи. Выполняется из Kernel::loop — если
    // сюда не попали 10 с, WDT перезагрузит устройство.
    if (_wdtArmed) esp_task_wdt_reset();

    // Heap — раз в 30 с
    if (millis() - _lastHeapCheckMs > HM_HEAP_CHECK_MS) {
        _lastHeapCheckMs = millis();
        checkHeap();
    }

    runChecks();
}

// ============================================================================
// HEAP: минимальный остаток за всё время (монолит: мониторинг памяти)
// ============================================================================
void HealthMonitor::checkHeap() {
    // minimum_free — исторический минимум: поймает даже кратковременные
    // просадки, которые обычный free бы пропустил.
    uint32_t minFree = esp_get_minimum_free_heap_size();
    ShEventData d; d.clear();
    d.code = (int32_t)(minFree / 1024);   // в КБ для читаемости лога

    if (minFree < HM_HEAP_CRIT_BYTES) {
        _criticalCount++;
        safeStrCopy(d.payload, sizeof(d.payload), "HEAP_CRITICAL");
        postEvent(HEALTH_EVENT_CRITICAL, &d);
        log(LogLevel::Critical, "HEAP critical: %lu bytes min",
            (unsigned long)minFree);
    } else if (minFree < HM_HEAP_WARN_BYTES) {
        _warningCount++;
        safeStrCopy(d.payload, sizeof(d.payload), "HEAP_LOW");
        postEvent(HEALTH_EVENT_WARNING, &d);
        log(LogLevel::Warning, "HEAP low: %lu bytes min",
            (unsigned long)minFree);
    }
}

// ============================================================================
// РЕГИСТРАЦИЯ И ИСПОЛНЕНИЕ ПРОВЕРОК
// ============================================================================
bool HealthMonitor::registerCheck(IHealthCheck* check) {
    if (check == nullptr) return false;
    if (_checkCount >= HM_MAX_CHECKS) {
        log(LogLevel::Error, "health check table full, '%s' rejected",
            check->checkName());
        return false;
    }
    _checks[_checkCount] = { check, 0, HealthResult::Status::Ok, "" };
    _checkCount++;
    log(LogLevel::Info, "health check registered: %s", check->checkName());
    return true;
}

void HealthMonitor::runChecks() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < _checkCount; ++i) {
        CheckSlot& s = _checks[i];
        if (now - s.lastRunMs < s.check->intervalMs()) continue;
        s.lastRunMs = now;

        HealthResult r = s.check->run();
        safeStrCopy(s.lastMsg, sizeof(s.lastMsg), r.message);

        // События публикуем только на ПЕРЕХОДАХ статуса (как у драйверов —
        // шина для фактов, не для каждого чиха)
        if (r.status == s.lastStatus) continue;
        s.lastStatus = r.status;

        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload),
                    r.message[0] ? r.message : s.check->checkName());

        if (r.status == HealthResult::Status::Critical) {
            _criticalCount++;
            postEvent(HEALTH_EVENT_CRITICAL, &d);
            log(LogLevel::Critical, "check %s: CRITICAL %s",
                s.check->checkName(), r.message);
        } else if (r.status == HealthResult::Status::Warning) {
            _warningCount++;
            postEvent(HEALTH_EVENT_WARNING, &d);
            log(LogLevel::Warning, "check %s: WARNING %s",
                s.check->checkName(), r.message);
        } else {
            // Возврат в норму
            if (_criticalCount > 0) _criticalCount--;
            else if (_warningCount > 0) _warningCount--;
            postEvent(HEALTH_EVENT_RECOVERED, &d);
            log(LogLevel::Info, "check %s: recovered", s.check->checkName());
        }
    }
}

// ============================================================================
// СВОДКА ДЛЯ /api/health (вкладка ПАЗ панели)
// ============================================================================
uint16_t HealthMonitor::reportJson(char* buf, size_t n) const {
    // st: 0=Ok 1=Warning 2=Critical — страница раскрашивает сама
    int w = snprintf(buf, n,
        "{\"healthy\":%d,\"warnings\":%u,\"criticals\":%u,\"wdt\":%d,"
        "\"heap_free\":%u,\"uptime_s\":%lu,\"checks\":[",
        isSystemHealthy() ? 1 : 0,
        (unsigned)_warningCount, (unsigned)_criticalCount,
        _wdtArmed ? 1 : 0,
        (unsigned)esp_get_free_heap_size(),
        (unsigned long)(millis() / 1000));
    for (uint8_t i = 0; i < _checkCount && w > 0 && (size_t)w < n - 4; ++i) {
        const CheckSlot& s = _checks[i];
        w += snprintf(buf + w, n - w,
            "%s{\"name\":\"%s\",\"st\":%d,\"msg\":\"%s\",\"age_ms\":%lu}",
            i ? "," : "", s.check->checkName(), (int)s.lastStatus,
            s.lastMsg, (unsigned long)(millis() - s.lastRunMs));
    }
    if (w > 0 && (size_t)w < n - 3) w += snprintf(buf + w, n - w, "]}");
    return (uint16_t)((w > 0) ? w : 0);
}
