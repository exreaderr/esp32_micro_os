// ============================================================================
// CounterService.cpp — реализация персистентных счётчиков
// ============================================================================
#include "CounterService.h"
#include "ConfigService.h"
#include "StorageService.h"
#include "../core/Events.h"
#include "../core/EventBus.h"
#include "../core/ResourceManager.h"
#include <soc/soc_caps.h>
#if SOC_PCNT_SUPPORTED
#include <driver/pcnt.h>
#endif
// Периферии PCNT нет на ESP32-C3/C2/H2 (узел шины C3 SuperMini, 5.4.1):
// бэкенд импульсных счётчиков на таких кристаллах компилируется в
// заглушку «отказ с логом», остальной сервис (программные каналы,
// ярусы хранения) живёт полноценно.

// NVS namespace автономных счётчиков (A2: регистрация в ResourceManager)
constexpr const char* CNT_NVS_NS = "cnt";

CounterService& CounterService::getInstance() {
    static CounterService instance;
    return instance;
}

// ============================================================================
// КОНФИГ-СХЕМА (авто-UI админки, группа «Счётчики»)
// ============================================================================
void CounterService::registerExtensions() {
    ConfigService::getInstance().addFields("Счётчики", {
        { "cnt.flush_every", ConfigType::UINT, "10", 1, 1000, CFG_NONE,
          "Счётчики", "Сброс в NVS каждые N инкрементов" },
        { "cnt.flush_interval_s", ConfigType::UINT, "600", 10, 86400, CFG_NONE,
          "Счётчики", "...или каждые T секунд (что раньше)" },
    });
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void CounterService::init() {
    ResourceManager::getInstance().claimNvsNamespace(CNT_NVS_NS, "counter");
    _initialized = true;
    log(LogLevel::Info, "init");
}

void CounterService::start() {
    EventBus& bus = EventBus::getInstance();
    bus.subscribe(SH_EVENT_SHUTDOWN, this);   // сбросить RAM в NVS до ребута
    bus.subscribe(CFG_EVENT_CHANGED, this);   // правка ключа извне (редко)
    _started = true;
    log(LogLevel::Info, "start");
}

void CounterService::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    _started = false;
    log(LogLevel::Info, "stop");
}

// ============================================================================
// TICK — поллинг PCNT + проверка политики сброса
// ============================================================================
void CounterService::tick() {
    if (!_started) return;
    const uint32_t now = millis();
    for (uint8_t i = 0; i < _count; ++i) {
        Entry& e = _entries[i];
        if (e.pcntGpio >= 0) pollPcnt(e);
        if (e.pending == 0) continue;
        if (cnt::shouldFlush(e.pending,
                             cfgGetUInt("cnt.flush_every",
                                        CNT_DEFAULT_FLUSH_EVERY),
                             now - e.lastFlushMs,
                             cfgGetUInt("cnt.flush_interval_s",
                                        CNT_DEFAULT_FLUSH_INTERVAL_S) * 1000U)) {
            flushEntry(e, now);
        }
    }
}

// ============================================================================
// ЯДРО МЕХАНИКИ
// ============================================================================
CounterService::Entry* CounterService::find(const char* name) {
    if (!name) return nullptr;
    for (uint8_t i = 0; i < _count; ++i)
        if (strncmp(_entries[i].name, name, CNT_NAME_LEN) == 0)
            return &_entries[i];
    return nullptr;
}

CounterService::Entry* CounterService::findOrCreate(const char* name,
                                                    bool cfgBound) {
    Entry* e = find(name);
    if (e) return e;
    if (!name || !*name || strlen(name) >= CNT_NAME_LEN ||
        _count >= CNT_MAX_COUNTERS) {
        ++_dropped;
        return nullptr;
    }
    e = &_entries[_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, CNT_NAME_LEN - 1);
    e->pcntGpio = -1;
    e->pcntUnit = -1;
    e->used     = true;
    e->cfgBound = cfgBound;
    e->lastFlushMs = millis();
    // Ленивая загрузка базы: cfg-ключ — из ConfigService, автономный —
    // из NVS "cnt". Нет записи — с нуля, это не ошибка (первый старт).
    if (cfgBound) {
        e->ram = cfgGetUInt(name, 0);
    } else {
        uint32_t base = 0;
        if (StorageService::getInstance().nvsRestore(CNT_NVS_NS, name,
                                                     &base, sizeof(base)) ==
            sizeof(base)) {
            e->ram = base;
        }
    }
    log(LogLevel::Info, "counter '%s': base=%lu (%s)", e->name,
        (unsigned long)e->ram, cfgBound ? "cfg" : "nvs");
    return e;
}

void CounterService::addDelta(Entry& e, uint32_t delta) {
    if (delta == 0) return;
    const uint32_t prev = e.ram;
    e.ram += delta;
    e.pending += delta;
    if (e.ram < prev) {   // 32-битное оборачивание — факт для телеметрии
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), e.name);
        postEvent(CNT_EVENT_ROLLOVER, &d);
        log(LogLevel::Warning, "counter '%s' rolled over 2^32", e.name);
    }
}

void CounterService::flushEntry(Entry& e, uint32_t nowMs) {
    if (e.cfgBound) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)e.ram);
        // setInternal: поле CFG_READONLY, обычный set() его отвергает
        // (постмортем 5.0.x — счётчик молча оставался нулём).
        ConfigService::getInstance().setInternal(e.name, buf);
    } else {
        StorageService::getInstance().nvsBackup(CNT_NVS_NS, e.name,
                                                &e.ram, sizeof(e.ram));
    }
    e.pending = 0;
    e.lastFlushMs = nowMs;
    ++_flushes;
    log(LogLevel::Debug, "flush '%s' = %lu", e.name, (unsigned long)e.ram);
}

void CounterService::flushAll(uint32_t nowMs) {
    for (uint8_t i = 0; i < _count; ++i)
        if (_entries[i].pending > 0) flushEntry(_entries[i], nowMs);
}

void CounterService::reloadCfgBase(Entry& e) {
    // Ключ поправили извне (восстановление бэкапа, сервисная правка):
    // RAM-тень перечитывает базу, незаписанные инкременты теряются —
    // редкая штатная ситуация, чинится следующим батчем.
    e.ram = cfgGetUInt(e.name, 0);
    e.pending = 0;
    log(LogLevel::Info, "counter '%s': base reloaded = %lu",
        e.name, (unsigned long)e.ram);
}

// ============================================================================
// ПУБЛИЧНЫЙ API
// ============================================================================
uint32_t CounterService::incrementCfg(const char* cfgKey, uint32_t delta) {
    Entry* e = findOrCreate(cfgKey, /*cfgBound=*/true);
    if (!e) return 0;
    addDelta(*e, delta);
    return e->ram;
}

uint32_t CounterService::valueCfg(const char* cfgKey) {
    Entry* e = findOrCreate(cfgKey, /*cfgBound=*/true);
    return e ? e->ram : 0;
}

uint32_t CounterService::increment(const char* name, uint32_t delta) {
    Entry* e = findOrCreate(name, /*cfgBound=*/false);
    if (!e) return 0;
    addDelta(*e, delta);
    return e->ram;
}

uint32_t CounterService::value(const char* name) {
    Entry* e = findOrCreate(name, /*cfgBound=*/false);
    return e ? e->ram : 0;
}

bool CounterService::reset(const char* name) {
    Entry* e = find(name);
    if (!e) return false;
    e->ram = 0;
    e->pending = 1;              // чтобы flushEntry прошёл по общему пути
    flushEntry(*e, millis());
    log(LogLevel::Info, "counter '%s' reset", e->name);
    return true;
}

uint32_t CounterService::pendingTotal() const {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < _count; ++i) sum += _entries[i].pending;
    return sum;
}

// ============================================================================
// PCNT-БЭКЕНД (импульсные счётчики: вода/газ/электричество)
// ============================================================================
bool CounterService::attachPcnt(const char* name, int8_t gpio,
                                uint16_t glitchFilterNs) {
#if !SOC_PCNT_SUPPORTED
    // Кристалл без PCNT (C3/C2/H2): честный отказ, профиль деградирует
    // по своему сценарию (для счётчиков импульсов такие платы не целевые).
    log(LogLevel::Warning, "pcnt: '%s' on GPIO%d rejected — no PCNT on this chip",
        name ? name : "?", gpio);
    (void)glitchFilterNs;
    return false;
#else
    if (gpio < 0) return false;
    Entry* e = findOrCreate(name, /*cfgBound=*/false);
    if (!e) return false;
    if (e->pcntGpio >= 0) return false;            // уже привязан

    // A2: пин — через реестр, конфликт с профилем = отказ.
    char owner[24];
    snprintf(owner, sizeof(owner), "cnt.pcnt.%s", e->name);
    if (!ResourceManager::getInstance().claimGpio((uint8_t)gpio, owner)) {
        log(LogLevel::Warning, "pcnt: GPIO%d busy, '%s' rejected", gpio, owner);
        return false;
    }

    // Юнит = индекс записи в таблице (<= 8 юнитов у ESP32).
    const pcnt_unit_t unit = (pcnt_unit_t)(e - _entries);
    pcnt_config_t cfg = {};
    cfg.pulse_gpio_num = gpio;
    cfg.ctrl_gpio_num  = PCNT_PIN_NOT_USED;
    cfg.lctrl_mode     = PCNT_MODE_KEEP;
    cfg.hctrl_mode     = PCNT_MODE_KEEP;
    cfg.pos_mode       = PCNT_COUNT_INC;   // нарастающий фронт — считаем
    cfg.neg_mode       = PCNT_COUNT_DIS;
    cfg.counter_h_lim  = 32767;            // максимум 16-битного регистра;
    cfg.counter_l_lim  = -32768;           // wrap ловит pcntDelta
    cfg.unit           = unit;
    cfg.channel        = PCNT_CHANNEL_0;
    if (pcnt_unit_config(&cfg) != ESP_OK) {
        log(LogLevel::Error, "pcnt: unit_config failed for '%s'", e->name);
        return false;
    }

    if (glitchFilterNs > 0) {
        // Фильтр в тактах APB (80 МГц): 1000 нс = 80 тактов, потолок 1023.
        uint32_t cycles = ((uint32_t)glitchFilterNs * 80U) / 1000U;
        if (cycles > 1023) cycles = 1023;
        pcnt_set_filter_value(unit, (uint16_t)cycles);
        pcnt_filter_enable(unit);
    }

    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);
    pcnt_counter_resume(unit);

    e->pcntGpio = gpio;
    e->pcntUnit = (int8_t)unit;
    e->pcntLast = 0;
    log(LogLevel::Info, "pcnt: '%s' on GPIO%d, unit %d, filter %u ns",
        e->name, gpio, (int)unit, glitchFilterNs);
    return true;
#endif // SOC_PCNT_SUPPORTED
}

void CounterService::pollPcnt(Entry& e) {
#if SOC_PCNT_SUPPORTED
    int16_t now = 0;
    if (pcnt_get_counter_value((pcnt_unit_t)e.pcntUnit, &now) != ESP_OK)
        return;
    const int32_t delta = cnt::pcntDelta(now, e.pcntLast);
    e.pcntLast = now;
    if (delta > 0) {   // отрицательная дельта = внешний clear, пропускаем
        addDelta(e, (uint32_t)delta);
    }
#else
    (void)e;   // без PCNT сюда не попасть: attachPcnt всегда отказывает
#endif
}

// ============================================================================
// СОБЫТИЯ
// ============================================================================
void CounterService::onEvent(int32_t eventId, const ShEventData* data) {
    if (eventId == SH_EVENT_SHUTDOWN) {
        flushAll(millis());   // штатный ребут — ничего не теряем
        return;
    }
    if (eventId == CFG_EVENT_CHANGED && data) {
        Entry* e = find(data->payload);
        if (e && e->cfgBound) reloadCfgBase(*e);
    }
}

bool CounterService::canHandleEvent(int32_t eventId) const {
    return eventId == SH_EVENT_SHUTDOWN || eventId == CFG_EVENT_CHANGED;
}
