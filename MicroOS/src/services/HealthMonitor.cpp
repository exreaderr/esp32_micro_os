// ============================================================================
// HealthMonitor.cpp — реализация ПАЗ-ядра
// ============================================================================
#include "HealthMonitor.h"
#include "TimeService.h"
#include "StorageService.h"
#include "TelemetryService.h"
#include "ConfigService.h"
#include "../core/Events.h"
#include "../core/DriverRegistry.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdlib>

// Путь журнала паник (NDJSON: строка = событие; append не атомарен —
// цена порчи хвоста при обесточке приемлема, см. StorageService).
static constexpr const char* HM_JOURNAL_PATH = "/paz_journal.ndjson";

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
    bus.subscribe(DRV_EVENT_TEMP_PANIC, this);
    bus.subscribe(SH_EVENT_TICK_OVERRUN, this);

    // --- Залежь №2: журнал паник + первый дежурный датчик -------------------
    loadJournal();
    // CPU temp: дежурный смотрит на ПУЛЬС ЧТЕНИЙ драйвера (cpuSeq), а не
    // на значение (урок ночи 14→15.08.2026: кристалл в терморавновесии
    // часами сидит на одной десятой градуса — watch по значению давал
    // 11 ложных STUCK в час на мастере и столько же на замке; остановка
    // ЧТЕНИЙ — вот настоящий stuck). Период обновления = tel.period_s
    // (умолч. 15 с); залипание = 4 пропущенных обновления, не < минуты.
    // WARNING, не CRITICAL: замёрзший термометр сам по себе кристалл
    // не жжёт (есть WDT и heap).
    uint32_t stuckMs = cfgGetUInt("tel.period_s", 15) * 4000UL;
    if (stuckMs < 60000UL) stuckMs = 60000UL;
    registerSensorWatch("cpu_temp",
        &TelemetryService::getInstance().snapshot().cpuSeq,
        stuckMs, false);

    // --- WDT: контролируем задачу loop() (Arduino loopTask). ---------------
    // Если loop зависнет дольше 10 с (любой модуль в tick/onEvent) — ребут.
    // ESP-IDF 5.x (core 3.x): init принимает конфиг-структуру (а не
    // (timeout, panic) как в 4.x). Arduino core инициализирует TWDT до нас —
    // сначала СПРАШИВАЕМ статус (5.8.0: раньше лупили init вслепую и ловили
    // E-строки «TWDT already initialized» / «task not found» в каждый бут).
    esp_task_wdt_config_t wdtCfg = {
        .timeout_ms     = HM_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,        // idle-задачи не контролируем
        .trigger_panic  = true      // таймаут -> паника -> ребут (монолит)
    };
    esp_err_t st = esp_task_wdt_status(NULL);   // ESP_OK = core уже подписал loop
    esp_err_t rc;
    if (st == ESP_OK) {
        rc = esp_task_wdt_reconfigure(&wdtCfg);
        _wdtArmed = (rc == ESP_OK);
    } else {
        rc = esp_task_wdt_init(&wdtCfg);
        if (rc == ESP_ERR_INVALID_STATE) rc = esp_task_wdt_reconfigure(&wdtCfg);
        if (rc == ESP_OK && esp_task_wdt_add(NULL) == ESP_OK) _wdtArmed = true;
    }
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
           eventId == DRV_EVENT_TEMP_PANIC ||
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
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "критическая температура %ld.%d C",
                         (long)(data ? data->code / 10 : 0),
                         (int)(data ? abs(data->code % 10) : 0));
                journalAdd("cpu_temp", msg);
            }
            break;
        case DRV_EVENT_TEMP_PANIC:
            // Последний рубеж: кристалл варится. Даже если дальше всё
            // «рассосётся» само — факт ОБЯЗАН остаться в журнале паник.
            _criticalCount++;
            safeStrCopy(d.payload, sizeof(d.payload), "CPU_TEMP_PANIC");
            if (data) d.code = data->code;
            postEvent(HEALTH_EVENT_CRITICAL, &d);
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "ТЕРМИЧЕСКАЯ ПАНИКА %ld.%d C",
                         (long)(data ? data->code / 10 : 0),
                         (int)(data ? abs(data->code % 10) : 0));
                journalAdd("cpu_temp", msg);
            }
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

    // Дежурные «залипших датчиков» — раз в 5 с
    uint32_t now = millis();
    if (now - _lastWatchCheckMs >= HM_WATCH_CHECK_MS) {
        _lastWatchCheckMs = now;
        checkWatches(now);
    }

    runChecks();
}

// ============================================================================
// HEAP (семантика 5.8.0): состояние — по ТЕКУЩЕМУ free с гистерезисом,
// watermark — одноразовым фактом. Урок ночи 14→15.08.2026: проверка шла
// по since-boot минимуму, который НЕ умеет восстанавливаться — один нырок
// в 19:38 защёлкнул CRITICAL и устройство долбило событие каждые 30 с
// всю ночь (1488 строк шума в журнал М3, счётчик «крит» раздуло до 211).
// Теперь: устойчивое истощение = переходы состояния (как у runChecks),
// транзиентный нырок = одна запись в журнале паник + одно событие-факт.
// ============================================================================
void HealthMonitor::checkHeap() {
    uint32_t curFree = esp_get_free_heap_size();
    uint32_t minFree = esp_get_minimum_free_heap_size();

    // --- ФАКТ: новый исторический минимум (шаг 1 КБ против микрокасаний:
    // в поле нырки 5536->5380->5340 Б за сутки — фактически одно дно) ---
    if (minFree + HM_HEAP_MIN_STEP_BYTES < _heapMinReported) {
        bool firstSample = (_heapMinReported == 0xFFFFFFFFUL);
        _heapMinReported = minFree;
        if (!firstSample && minFree < HM_HEAP_WARN_BYTES) {
            // Счётчики НЕ трогаем: это факт-репер, а не активное состояние
            ShEventData d; d.clear();
            d.code = (int32_t)(minFree / 1024);
            safeStrCopy(d.payload, sizeof(d.payload), "HEAP_MIN");
            postEvent(HEALTH_EVENT_WARNING, &d);
            log(LogLevel::Info, "HEAP new min: %lu bytes",
                (unsigned long)minFree);
            char msg[64];
            snprintf(msg, sizeof(msg), "heap новый минимум %lu байт",
                     (unsigned long)minFree);
            journalAdd("heap", msg);
        }
    }

    // --- СОСТОЯНИЕ: переходы по текущему free, гистерезис на возврат ---
    uint8_t newState = _heapState;
    switch (_heapState) {
        case 0:   // Ok
            if (curFree < HM_HEAP_CRIT_BYTES)      newState = 2;
            else if (curFree < HM_HEAP_WARN_BYTES) newState = 1;
            break;
        case 1:   // Warn
            if (curFree < HM_HEAP_CRIT_BYTES)      newState = 2;
            else if (curFree >= HM_HEAP_WARN_BYTES + HM_HEAP_HYST_BYTES)
                                                   newState = 0;
            break;
        default:  // Crit
            if (curFree >= HM_HEAP_WARN_BYTES + HM_HEAP_HYST_BYTES)
                                                   newState = 0;
            else if (curFree >= HM_HEAP_CRIT_BYTES + HM_HEAP_HYST_BYTES)
                                                   newState = 1;
            break;
    }
    if (newState == _heapState) return;

    uint8_t prev = _heapState;
    _heapState = newState;
    ShEventData d; d.clear();
    d.code = (int32_t)(curFree / 1024);   // в КБ для читаемости лога

    if (newState == 2) {                  // -> CRITICAL
        _criticalCount++;
        if (prev == 1 && _warningCount > 0) _warningCount--;
        safeStrCopy(d.payload, sizeof(d.payload), "HEAP_CRITICAL");
        postEvent(HEALTH_EVENT_CRITICAL, &d);
        log(LogLevel::Critical, "HEAP critical: %lu bytes free",
            (unsigned long)curFree);
        char msg[64];
        snprintf(msg, sizeof(msg), "heap критически мало: %lu байт",
                 (unsigned long)curFree);
        journalAdd("heap", msg);
    } else if (newState == 1) {           // -> WARNING (из Ok или из Crit)
        _warningCount++;
        if (prev == 2 && _criticalCount > 0) _criticalCount--;
        safeStrCopy(d.payload, sizeof(d.payload), "HEAP_LOW");
        postEvent(HEALTH_EVENT_WARNING, &d);
        log(LogLevel::Warning, "HEAP low: %lu bytes free",
            (unsigned long)curFree);
        if (prev == 2) journalAdd("heap", "heap вышел из критики");
    } else {                              // -> Ok
        if (prev == 2 && _criticalCount > 0) _criticalCount--;
        if (prev == 1 && _warningCount > 0) _warningCount--;
        safeStrCopy(d.payload, sizeof(d.payload), "HEAP_RECOVERED");
        postEvent(HEALTH_EVENT_RECOVERED, &d);
        log(LogLevel::Info, "HEAP recovered: %lu bytes free",
            (unsigned long)curFree);
        if (prev == 2) journalAdd("heap", "heap восстановлено");
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
        HealthResult::Status prevStatus = s.lastStatus;  // до перезаписи!
        s.lastStatus = r.status;

        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload),
                    r.message[0] ? r.message : s.check->checkName());

        if (r.status == HealthResult::Status::Critical) {
            _criticalCount++;
            postEvent(HEALTH_EVENT_CRITICAL, &d);
            log(LogLevel::Critical, "check %s: CRITICAL %s",
                s.check->checkName(), r.message);
            journalAdd(s.check->checkName(),
                       r.message[0] ? r.message : "CRITICAL");
        } else if (r.status == HealthResult::Status::Warning) {
            _warningCount++;
            postEvent(HEALTH_EVENT_WARNING, &d);
            log(LogLevel::Warning, "check %s: WARNING %s",
                s.check->checkName(), r.message);
        } else {
            // Возврат в норму. Восстановление ИЗ КРИТИКИ — тоже журналируем:
            // по паре «упало/поднялось» видно длительность аварии.
            bool wasCritical = (prevStatus == HealthResult::Status::Critical);
            if (_criticalCount > 0) _criticalCount--;
            else if (_warningCount > 0) _warningCount--;
            postEvent(HEALTH_EVENT_RECOVERED, &d);
            log(LogLevel::Info, "check %s: recovered", s.check->checkName());
            if (wasCritical) journalAdd(s.check->checkName(), "восстановлено");
        }
    }
}

// ============================================================================
// ЖУРНАЛ ПАНИК (залежь №2): RAM-кольцо + NDJSON на LittleFS
// ============================================================================
// Зачем (урок монолита v4.2.2): авария, после которой устройство ребутнулось
// или «рассосалась» сама, не оставляла следа — по утру «ничего не было».
// Журнал отвечает на вопрос «ЧТО случилось ночью?» без монитора порта.

// Мини-парсеры NDJSON-строки (без ArduinoJson — его в проекте нет).
static bool jGetU32(const char* line, const char* key, uint32_t& out) {
    const char* p = strstr(line, key);
    if (p == nullptr) return false;
    out = (uint32_t)strtoul(p + strlen(key), nullptr, 10);
    return true;
}
static bool jGetStr(const char* line, const char* key, char* out, size_t n) {
    const char* p = strstr(line, key);
    if (p == nullptr) return false;
    p += strlen(key);
    const char* q = strchr(p, '"');
    if (q == nullptr) return false;
    size_t len = (size_t)(q - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

void HealthMonitor::journalAdd(const char* src, const char* msg) {
    if (!_journalLoaded) loadJournal();   // лениво: порядок модулей не важен

    PazJournalEntry& e = _journal[_journalHead];
    e.unix = (uint32_t)TimeService::getInstance().getUnixTime();
    if (e.unix < 1700000000UL) e.unix = 0;  // часы не выставлены — не врём
    e.uptime_s = millis() / 1000;
    e.heap_kb  = (uint16_t)(esp_get_free_heap_size() / 1024);
    safeStrCopy(e.src, sizeof(e.src), src ? src : "?");
    safeStrCopy(e.msg, sizeof(e.msg), msg ? msg : "?");
    // UTF-8-граница: safeStrCopy режет по БАЙТАМ и может рассечь
    // кириллический символ (2 байта) — в панели вылез бы «». Если было
    // усечение, откатываемся до начала незавершённого символа.
    if (msg != nullptr && strlen(msg) >= sizeof(e.msg)) {
        size_t ml = strlen(e.msg);
        while (ml > 0 && (e.msg[ml - 1] & 0xC0) == 0x80) ml--;   // «хвосты»
        if (ml > 0 && (uint8_t)e.msg[ml - 1] >= 0xC0) {
            uint8_t lead = (uint8_t)e.msg[ml - 1];
            uint8_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : 2;
            if ((uint8_t)(strlen(e.msg) - (ml - 1)) < need) e.msg[ml - 1] = '\0';
        }
    }
    // Санитация под NDJSON: кавычка/слэш в тексте ломали бы парсинг строки
    for (char* p = e.src; *p; ++p) if (*p == '"' || *p == '\\') *p = '\'';
    for (char* p = e.msg; *p; ++p) if (*p == '"' || *p == '\\') *p = '\'';

    _journalHead = (uint8_t)((_journalHead + 1) % HM_JOURNAL_SIZE);
    if (_journalCount < HM_JOURNAL_SIZE) _journalCount++;

    journalPersist(e);
}

void HealthMonitor::journalPersist(const PazJournalEntry& e) {
    char line[192];   // msg до 63 + русский UTF-8: 128 байт уже не хватало
    int n = snprintf(line, sizeof(line),
        "{\"unix\":%lu,\"uptime_s\":%lu,\"heap_kb\":%u,"
        "\"src\":\"%s\",\"msg\":\"%s\"}\n",
        (unsigned long)e.unix, (unsigned long)e.uptime_s,
        (unsigned)e.heap_kb, e.src, e.msg);
    if (n <= 0) return;
    // false = FS не готова/сбой записи: журнал — не критичные данные
    // (см. StorageService::appendFile), RAM-кольцо всё равно работает.
    StorageService::getInstance().appendFile(HM_JOURNAL_PATH, line);
}

void HealthMonitor::loadJournal() {
    _journalLoaded = true;
    _journalHead = 0;
    _journalCount = 0;

    StorageService& fs = StorageService::getInstance();
    size_t size = fs.fileSize(HM_JOURNAL_PATH);
    if (size == 0) return;

    // Между загрузками файл мог разрастись (компакция — только при старте):
    // читаем ХВОСТ — там самые свежие записи. Буфер ОДИН на разбор и
    // компакцию (разбор заканчивается до начала перезаписи). Из КУЧИ,
    // на время загрузки: статикой 2 КБ BSS не влезли бы в dram0_0_seg
    // (урок линковки 5.0.9: запас сегмента — сотни байт, не килобайты).
    const size_t cap = HM_JOURNAL_SIZE * 192;
    uint8_t* buf = (uint8_t*)malloc(cap + 1);
    if (buf == nullptr) {
        log(LogLevel::Warning, "paz journal: no heap, пропуск загрузки");
        return;
    }
    size_t skip = (size > cap) ? size - cap : 0;
    File f = fs.openRead(HM_JOURNAL_PATH);
    if (!f) { free(buf); return; }
    if (skip > 0) f.seek(skip);
    size_t got = f.read(buf, cap);
    f.close();
    if (got == 0) { free(buf); return; }
    buf[got] = '\0';

    char* text = (char*)buf;
    if (skip > 0) {   // обрубок первой строки — в помойку
        char* nl = strchr(text, '\n');
        if (nl == nullptr) { free(buf); return; }
        text = nl + 1;
    }

    // Построчно в кольцо: старые первыми — в кольце осядут 10 последних.
    uint16_t lines = 0;
    char* save = nullptr;
    for (char* ln = strtok_r(text, "\n", &save); ln != nullptr;
         ln = strtok_r(nullptr, "\n", &save)) {
        if (ln[0] != '{') continue;
        lines++;
        PazJournalEntry& e = _journal[_journalHead];
        memset(&e, 0, sizeof(e));
        jGetU32(ln, "\"unix\":", e.unix);
        jGetU32(ln, "\"uptime_s\":", e.uptime_s);
        uint32_t hb = 0;
        jGetU32(ln, "\"heap_kb\":", hb);
        e.heap_kb = (uint16_t)hb;
        jGetStr(ln, "\"src\":\"", e.src, sizeof(e.src));
        jGetStr(ln, "\"msg\":\"", e.msg, sizeof(e.msg));
        _journalHead = (uint8_t)((_journalHead + 1) % HM_JOURNAL_SIZE);
        if (_journalCount < HM_JOURNAL_SIZE) _journalCount++;
    }

    // Компакция: записей было больше глубины кольца ИЛИ файл разросся
    // за пределы хвоста — переписываем сохранённым хвостом (атомарно),
    // иначе журнал пухнет вечно. Разбор завершён — буфер свободен.
    if (lines > HM_JOURNAL_SIZE || skip > 0) {
        size_t w = 0;
        for (uint8_t i = 0; i < _journalCount; ++i) {
            uint8_t idx = (_journalCount < HM_JOURNAL_SIZE)
                ? i : (uint8_t)((_journalHead + i) % HM_JOURNAL_SIZE);
            const PazJournalEntry& e = _journal[idx];
            w += snprintf((char*)buf + w, (cap + 1) - w,
                "{\"unix\":%lu,\"uptime_s\":%lu,\"heap_kb\":%u,"
                "\"src\":\"%s\",\"msg\":\"%s\"}\n",
                (unsigned long)e.unix, (unsigned long)e.uptime_s,
                (unsigned)e.heap_kb, e.src, e.msg);
        }
        fs.atomicWrite(HM_JOURNAL_PATH, (const char*)buf);
        log(LogLevel::Info, "paz journal compacted: %u -> %u lines",
            (unsigned)lines, (unsigned)_journalCount);
    }
    free(buf);
    if (_journalCount > 0) {
        log(LogLevel::Info, "paz journal: %u entries restored",
            (unsigned)_journalCount);
    }
}

size_t HealthMonitor::journalCopy(PazJournalEntry* out, size_t max) const {
    if (out == nullptr || max == 0) return 0;
    size_t n = (_journalCount < max) ? _journalCount : max;
    for (size_t i = 0; i < n; ++i) {
        // head — слот СЛЕДУЮЩЕЙ записи; самая свежая — head-1
        int idx = (int)_journalHead - 1 - (int)i;
        while (idx < 0) idx += HM_JOURNAL_SIZE;
        out[i] = _journal[idx];
    }
    return n;
}

// ============================================================================
// ДЕЖУРНЫЙ ДАТЧИКОВ (sensor-stuck, залежь №2)
// ============================================================================
bool HealthMonitor::registerSensorWatch(const char* name,
                                        const int16_t* rawPtr,
                                        uint32_t maxStuckMs, bool critical) {
    if (name == nullptr || rawPtr == nullptr || maxStuckMs == 0) return false;
    if (_watchCount >= HM_WATCH_SLOTS) {
        log(LogLevel::Error, "sensor watch table full, '%s' rejected", name);
        return false;
    }
    WatchSlot& w = _watches[_watchCount++];
    w.name         = name;
    w.rawPtr       = rawPtr;
    w.lastRaw      = *rawPtr;
    w.lastChangeMs = millis();
    w.maxStuckMs   = maxStuckMs;
    w.critical     = critical;
    w.stuck        = false;
    log(LogLevel::Info, "sensor watch: %s (stuck > %lu ms, %s)", name,
        (unsigned long)maxStuckMs, critical ? "critical" : "warning");
    return true;
}

void HealthMonitor::checkWatches(uint32_t now) {
    for (uint8_t i = 0; i < _watchCount; ++i) {
        WatchSlot& w = _watches[i];
        if (w.rawPtr == nullptr) continue;
        int16_t raw = *w.rawPtr;

        if (raw != w.lastRaw) {
            // Значение шевельнулось — датчик жив (или ожил)
            if (w.stuck) {
                w.stuck = false;
                ShEventData d; d.clear();
                snprintf(d.payload, sizeof(d.payload),
                         "WATCH_RECOVERED:%s", w.name);
                if (w.critical) { if (_criticalCount > 0) _criticalCount--; }
                else            { if (_warningCount > 0) _warningCount--; }
                postEvent(HEALTH_EVENT_RECOVERED, &d);
                log(LogLevel::Info, "sensor watch %s: recovered", w.name);
            }
            w.lastRaw = raw;
            w.lastChangeMs = now;
            continue;
        }

        if (!w.stuck && (now - w.lastChangeMs) >= w.maxStuckMs) {
            // Фронт «залип»: значение не менялось дольше бюджета
            w.stuck = true;
            ShEventData d; d.clear();
            snprintf(d.payload, sizeof(d.payload), "WATCH_STUCK:%s", w.name);
            if (w.critical) {
                _criticalCount++;
                postEvent(HEALTH_EVENT_CRITICAL, &d);
                journalAdd(w.name, "датчик завис (показания замерли)");
            } else {
                _warningCount++;
                postEvent(HEALTH_EVENT_WARNING, &d);
            }
            log(LogLevel::Warning, "sensor watch %s: STUCK > %lu ms",
                w.name, (unsigned long)w.maxStuckMs);
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
    // Хвост журнала паник — 4 свежие записи (новые первыми)
    if (w > 0 && (size_t)w < n - 16) {
        w += snprintf(buf + w, n - w, "],\"journal\":[");
        PazJournalEntry je[4];
        size_t jn = journalCopy(je, 4);
        for (size_t i = 0; i < jn && w > 0 && (size_t)w < n - 4; ++i) {
            w += snprintf(buf + w, n - w,
                "%s{\"unix\":%lu,\"up\":%lu,\"heap\":%u,"
                "\"src\":\"%s\",\"msg\":\"%s\"}",
                i ? "," : "",
                (unsigned long)je[i].unix, (unsigned long)je[i].uptime_s,
                (unsigned)je[i].heap_kb, je[i].src, je[i].msg);
        }
    }
    if (w > 0 && (size_t)w < n - 3) w += snprintf(buf + w, n - w, "]}");
    return (uint16_t)((w > 0) ? w : 0);
}
