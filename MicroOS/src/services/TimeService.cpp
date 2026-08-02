// ============================================================================
// TimeService.cpp — реализация системного времени
// ============================================================================
#include "TimeService.h"
#include "ConfigService.h"
#include "../core/Events.h"
#include "../core/DriverRegistry.h"
#include "../drivers/Ds3231Driver.h"
#include <sys/time.h>

TimeService& TimeService::getInstance() {
    static TimeService instance;
    return instance;
}

// ============================================================================
// КОНФИГ-СХЕМА (NTP, Фаза 3)
// ============================================================================
void TimeService::registerExtensions() {
    ConfigService::getInstance().addFields("Система", {
        { "sys.ntp_enabled", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Система", "NTP-синхронизация при появлении сети" },
        { "sys.ntp_server", ConfigType::STRING, "pool.ntp.org", 0, 0, CFG_NONE,
          "Система", "NTP-сервер" },
        // CFG_NONE: getLocalTime читает пояс из конфига на каждый вызов —
        // смена применяется мгновенно, рестарт не нужен (урок 5.0.x: после
        // детерминированного gmtime_r пояс перестал быть состоянием системы).
        { "sys.tz_offset", ConfigType::INT, "3", -12, 12, CFG_NONE,
          "Система", "Смещение часового пояса, ч (GMT+)" },
    });
}

// ============================================================================
// INIT: первичное чтение RTC
// ============================================================================
void TimeService::init() {
    // RTC — главный источник. Если мёртв — стартуем недостоверно и ждём
    // восстановления шины (событие) или NTP (Phase 2).
    syncFromRtc("boot");
    _initialized = true;
    log(LogLevel::Info, "init: time %s",
        _timeValid ? "VALID (from RTC)" : "INVALID (RTC dead, waiting)");
}

// ============================================================================
// START: подписки на события шины
// ============================================================================
void TimeService::start() {
    EventBus& bus = EventBus::getInstance();
    // Шина восстановилась/умерла -> пересинхронизация и события RTC_*
    bus.subscribe(DRV_EVENT_BUS_RECOVERED, this);
    bus.subscribe(DRV_EVENT_BUS_DEAD, this);
    // Появился IP -> запуск NTP (Фаза 3)
    bus.subscribe(NET_EVENT_IP_CHANGED, this);
    _started = true;
}

void TimeService::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    _started = false;
}

bool TimeService::canHandleEvent(int32_t eventId) const {
    return eventId == DRV_EVENT_BUS_RECOVERED || eventId == DRV_EVENT_BUS_DEAD ||
           eventId == NET_EVENT_IP_CHANGED;
}

// ============================================================================
// СОБЫТИЯ
// ============================================================================
void TimeService::onEvent(int32_t eventId, const ShEventData* data) {
    (void)data;
    if (eventId == DRV_EVENT_BUS_RECOVERED) {
        _rtcAlive = true;
        syncFromRtc("bus recovered");
        ShEventData d; d.clear();
        postEvent(TIME_EVENT_RTC_RESTORED, &d);
    } else if (eventId == DRV_EVENT_BUS_DEAD) {
        _rtcAlive = false;
        // Достоверность теряем не мгновенно: системное время ещё идёт
        // (зеркало), но факт смерти RTC фиксируем для ПАЗ и журнала.
        ShEventData d; d.clear();
        postEvent(TIME_EVENT_RTC_LOST, &d);
        log(LogLevel::Warning, "RTC lost (bus dead), running on mirror");
    } else if (eventId == NET_EVENT_IP_CHANGED) {
        // IP появился (или сменился) — свежая точка запуска NTP
        _ntpSynced = false;
        requestNtp();
    }
}

// ============================================================================
// NTP: запуск (асинхронный, ответ поймаем в tick)
// ============================================================================
void TimeService::requestNtp() {
    if (!cfgGetBool("sys.ntp_enabled", true)) return;

    char server[CFG_VALUE_LEN];
    cfgGetStr("sys.ntp_server", server, sizeof(server), "pool.ntp.org");
    long offsetSec = (long)cfgGetInt("sys.tz_offset", 3) * 3600L;

    // configTime — неблокирующий запуск SNTP: ответ придёт асинхронно,
    // системное время взлетит само; мы зафиксируем факт в tick().
    configTime(offsetSec, 0, server);
    _ntpRequested = true;
    log(LogLevel::Info, "NTP requested: %s (GMT%+ld)", server, offsetSec / 3600);
}

bool TimeService::forceNtpSync() {
    if (!cfgGetBool("sys.ntp_enabled", true)) return false;
    _ntpSynced = false;   // переармить ожидание: tick() снова зафиксирует факт
    requestNtp();
    return _ntpRequested;
}

// ============================================================================
// TICK: почасовая синхронизация RTC -> системное время (монолит: hourly)
// ============================================================================
void TimeService::tick() {
    if (_rtcAlive && millis() - _lastSyncMs > TIME_SYNC_INTERVAL_MS) {
        syncFromRtc("hourly");
    }

    // --- Завершение NTP: SNTP уже выставил системное время ----------------
    // Порог 2025-01-01: время меньше — значит ответа ещё не было (RTC
    // мог дать время и раньше; NTP опознаём по "вдруг стало >= нынешнего").
    if (_ntpRequested && !_ntpSynced) {
        time_t now = time(nullptr);
        if (now >= 1735689600) {   // 2025-01-01 00:00:00 UTC
            _ntpSynced = true;
            // Фиксируем в RTC (переживёт обесточку) — полный цикл
            // NTP -> системное -> DS3231, как в монолите.
            setSystemTime(now);
            log(LogLevel::Info, "NTP synced, RTC updated");
        }
    }
}

// ============================================================================
// СИНХРОНИЗАЦИЯ ИЗ RTC
// ============================================================================
void TimeService::syncFromRtc(const char* reason) {
    auto* rtc = DriverRegistry::getInstance().findAs<Ds3231Driver>("ds3231");
    if (rtc == nullptr) {
        _rtcAlive = false;
        return;
    }

    struct tm t;
    if (!rtc->getDateTime(t)) {
        _rtcAlive = rtc->isHealthy();
        if (!_rtcAlive) _timeValid = false;
        return;
    }
    _rtcAlive = true;

    // RTC хранит UTC-wall (контракт 5.0.x, детерминировано): чтение —
    // чистая функция гражданского времени, БЕЗ mktime (тот зависит от
    // TZ-окружения libc и «плавал» бы при смене пояса в конфиге).
    time_t unix = (time_t)sh_time::secondsFromCivil(
        t.tm_year + 1900, (unsigned)t.tm_mon + 1, (unsigned)t.tm_mday,
        (unsigned)t.tm_hour, (unsigned)t.tm_min, (unsigned)t.tm_sec);

    struct timeval tv = { .tv_sec = unix, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    _timeValid = true;
    _lastSyncMs = millis();

    ShEventData d; d.clear();
    safeStrCopy(d.payload, sizeof(d.payload), reason);
    postEvent(TIME_EVENT_SYNCED, &d);
    log(LogLevel::Info, "time synced from RTC (%s)", reason);
}

// ============================================================================
// API
// ============================================================================
time_t TimeService::getUnixTime() const {
    if (!_timeValid) return 0;
    return time(nullptr);
}

bool TimeService::getLocalTime(struct tm& out) const {
    if (!_timeValid) return false;
    // ДЕТЕРМИНИРОВАННО: локальное = UTC + sys.tz_offset, БЕЗ localtime_r.
    // Урок 5.0.x: TZ-окружение libc на устройстве не выставлено (configTime
    // его не применяет) — localtime_r отдавал UTC, и расписание кнопки
    // выхода «съезжало» на смещение пояса (у пользователя — инверсия
    // запрета: днём запрещена, ночью разрешена). gmtime_r от сдвинутого
    // времени не зависит от окружения вообще.
    const time_t now   = time(nullptr);
    const time_t local = now + (time_t)cfgGetInt("sys.tz_offset", 3) * 3600L;
    gmtime_r(&local, &out);
    return true;
}

bool TimeService::setSystemTime(time_t unixUtc) {
    if (unixUtc <= 0) return false;

    // 1. Системное время
    struct timeval tv = { .tv_sec = unixUtc, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    // 2. RTC (если жив) — чтобы время пережило ребут. Пишем UTC-wall
    // (контракт чтения — см. syncFromRtc): gmtime_r не зависит от TZ.
    auto* rtc = DriverRegistry::getInstance().findAs<Ds3231Driver>("ds3231");
    if (rtc && rtc->isHealthy()) {
        struct tm t;
        gmtime_r(&unixUtc, &t);
        rtc->setDateTime(t);
    }

    _timeValid = true;
    _lastSyncMs = millis();

    ShEventData d; d.clear();
    safeStrCopy(d.payload, sizeof(d.payload), "external");
    postEvent(TIME_EVENT_SYNCED, &d);
    return true;
}

// ============================================================================
// ИНТЕРВАЛЫ HH:MM (для расписаний, переход через полночь)
// ============================================================================
bool TimeService::isTimeInInterval(const char* startHHMM,
                                   const char* endHHMM) const {
    struct tm t;
    if (!getLocalTime(t)) return false;

    // Парсинг "HH:MM" в минуты от полуночи; при битом формате — false
    int sh = 0, sm = 0, eh = 0, em = 0;
    if (sscanf(startHHMM, "%d:%d", &sh, &sm) != 2) return false;
    if (sscanf(endHHMM,   "%d:%d", &eh, &em) != 2) return false;

    int nowMin   = t.tm_hour * 60 + t.tm_min;
    int startMin = sh * 60 + sm;
    int endMin   = eh * 60 + em;

    // Логика — в чистой статической функции (D2: покрыта host-тестами)
    return minutesInInterval(nowMin, startMin, endMin);
}
