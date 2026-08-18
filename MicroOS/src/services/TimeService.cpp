// ============================================================================
// TimeService.cpp — реализация системного времени
// ============================================================================
#include "TimeService.h"
#include "ConfigService.h"
#include "../core/Events.h"
#include "../core/DriverRegistry.h"
#include "../core/SntpCore.h"
#include "../drivers/Ds3231Driver.h"
#include <sys/time.h>
#include <lwip/udp.h>
#include <lwip/tcpip.h>
#include <lwip/dns.h>

// ============================================================================
// NTP-транспорт (5.5.8): ВЕСЬ lwIP — только внутри tcpip_thread.
// Урок 5.5.7: dns_gethostbyname/udp_* из чужих задач (ETH-event, loopTask)
// под LWIP_TCPIP_CORE_LOCKING без лока → assert udp_new_ip_type (udp.c:1278),
// бутлуп обоих устройств ВКЛЮЧАЯ Safe Mode (доказано addr2line по бэктрейсу
// бенча 10.08.2026). Архитектура: loopTask постит задание через
// tcpip_callback (без блокировки) и наблюдает volatile-фазы; колбэки
// tcpip_thread исполняют lwIP и ставят ТОЛЬКО флаги. Ровно так устроен
// штатный lwIP sntp — потому он никогда и не падал.
// ============================================================================
namespace {
enum NtpPhase : uint8_t {
    PH_NONE = 0,     // нет задания
    PH_DISPATCHING,  // задание в очереди tcpip_thread
    PH_RESOLVING,    // DNS в полёте
    PH_SENT,         // запрос ушёл на провод
    PH_RX_READY,     // ответ лежит в rxBuf
    PH_DNS_FAIL,     // резолв не удался
    PH_SEND_FAIL,    // pcb/pbuf/udp_sendto отказали
};
struct NtpJob {
    char server[64] = {0};
    volatile NtpPhase phase = PH_NONE;
    volatile uint32_t resolvedIp = 0;  // как в lwIP (ip4_addr.addr)
    volatile uint32_t sentAtMs = 0;
    volatile uint32_t rxAtMs = 0;
    uint8_t rxBuf[sh_sntp::PACKET_LEN] = {0};
};
NtpJob s_job;
udp_pcb* s_pcb = nullptr;   // один pcb на всю жизнь устройства (как у sntp)

// --- дальше ТОЛЬКО контекст tcpip_thread ---
void ntpDoSend(const ip_addr_t* addr) {
    uint8_t pkt[sh_sntp::PACKET_LEN];
    sh_sntp::buildRequest(pkt);
    pbuf* p = pbuf_alloc(PBUF_TRANSPORT, sh_sntp::PACKET_LEN, PBUF_RAM);
    if (p == nullptr) { s_job.phase = PH_SEND_FAIL; return; }
    pbuf_take(p, pkt, sh_sntp::PACKET_LEN);
    err_t err = udp_sendto(s_pcb, p, addr, 123);
    pbuf_free(p);
    if (err == ERR_OK) {
        s_job.resolvedIp = addr->u_addr.ip4.addr;
        s_job.sentAtMs = millis();
        s_job.phase = PH_SENT;
    } else {
        s_job.phase = PH_SEND_FAIL;
    }
}
void ntpDnsCb(const char* name, const ip_addr_t* ipaddr, void* arg) {
    (void)name; (void)arg;
    if (ipaddr == nullptr) { s_job.phase = PH_DNS_FAIL; return; }
    ntpDoSend(ipaddr);
}
void ntpUdpRecv(void* arg, udp_pcb* pcb, pbuf* p, const ip_addr_t* addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;
    if (p != nullptr) {
        if (p->tot_len >= sh_sntp::PACKET_LEN) {
            pbuf_copy_partial(p, s_job.rxBuf, sh_sntp::PACKET_LEN, 0);
            s_job.rxAtMs = millis();
            s_job.phase = PH_RX_READY;
        }
        pbuf_free(p);
    }
}
void ntpTcpipStart(void* arg) {   // задание из loopTask, исполнение здесь
    (void)arg;
    if (s_pcb == nullptr) {
        s_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
        if (s_pcb != nullptr) {
            udp_bind(s_pcb, IP_ANY_TYPE, 0);       // эфемерный порт
            udp_recv(s_pcb, ntpUdpRecv, nullptr);  // приём armed навсегда
        }
    }
    if (s_pcb == nullptr) { s_job.phase = PH_SEND_FAIL; return; }

    ip4_addr_t ip4;
    if (ip4addr_aton(s_job.server, &ip4) == 1) {   // числовой IP — DNS не нужен
        ip_addr_t addr;
        ip_addr_set_ip4_u32(&addr, ip4.addr);
        ntpDoSend(&addr);
        return;
    }
    // hostname — async DNS (попутно бесплатный UDP-пробник: DNS = UDP/53)
    ip_addr_t addr;
    err_t err = dns_gethostbyname(s_job.server, &addr, ntpDnsCb, nullptr);
    if (err == ERR_OK) ntpDoSend(&addr);
    else if (err == ERR_INPROGRESS) s_job.phase = PH_RESOLVING;
    else s_job.phase = PH_DNS_FAIL;
}
} // namespace

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
        // IP появился (или сменился) — свежая точка запуска NTP,
        // новая сетевая сессия: счётчики честности обнуляем.
        // 5.5.8: контекст здесь — задача ETH-event, lwIP ЗАПРЕЩЁН.
        // Только флаг; исполнит tick в контексте loopTask.
        _ntpSynced = false;
        _ntpAttempts = 0;
        _ntpPending = true;
    }
}

// ============================================================================
// NTP (5.5.7+): СВОЙ SNTP-клиент поверх UDP — каждый шаг под логом.
// Замена lwIP configTime: тот на W5500 молчал бесследно (запросы не
// покидали устройство — доказано Torch'ем на MikroTik, бенч 10.08.2026).
// 5.5.8: lwIP исполняется только в tcpip_thread (см. транспорт выше);
// здесь — только автомат состояний, лог и пост заданий. ТОЛЬКО loopTask.
// ============================================================================
void TimeService::requestNtp() {
    if (!cfgGetBool("sys.ntp_enabled", true)) {
        _ntpState = NtpState::IDLE;
        return;
    }
    cfgGetStr("sys.ntp_server", _ntpServer, sizeof(_ntpServer), "pool.ntp.org");
    safeStrCopy(s_job.server, sizeof(s_job.server), _ntpServer);
    s_job.phase = PH_DISPATCHING;
    if (tcpip_callback(ntpTcpipStart, nullptr) != ERR_OK) {
        s_job.phase = PH_NONE;
        _ntpState = NtpState::WAIT_RETRY;
        _ntpStateSinceMs = millis();
        log(LogLevel::Warning,
            "NTP: tcpip_thread НЕ ПРИНЯЛ задание (очередь переполнена?) — "
            "переарм через %u с", (unsigned)(NTP_RETRY_INTERVAL_MS / 1000));
        return;
    }
    _ntpState = NtpState::DISPATCHING;
    _ntpStateSinceMs = millis();
}

bool TimeService::forceNtpSync() {
    if (!cfgGetBool("sys.ntp_enabled", true)) return false;
    _ntpSynced = false;    // переармить ожидание: автомат снова ловит ответ
    _ntpPending = true;    // 5.5.8: контекст — задача HTTP, lwIP запрещён.
    return true;           // исполнит tick (loopTask) на ближайшем проходе
}

const char* TimeService::ntpStateStr() const {
    switch (_ntpState) {
        case NtpState::DISPATCHING: return "dispatching";
        case NtpState::RESOLVING:   return "resolving";
        case NtpState::REQUESTED:   return "requested";
        case NtpState::SYNCED:      return "synced";
        case NtpState::WAIT_RETRY:  return "wait_retry";
        default:                    return "idle";
    }
}

// ============================================================================
// TICK: почасовая синхронизация RTC -> системное время (монолит: hourly)
// ============================================================================
void TimeService::tick() {
    if (_rtcAlive && millis() - _lastSyncMs > TIME_SYNC_INTERVAL_MS) {
        syncFromRtc("hourly");
    }

    // --- NTP-автомат (5.5.7+): DISPATCHING → RESOLVING → REQUESTED →
    // SYNCED / WAIT_RETRY. Честный детект: synced ставит ТОЛЬКО реальный
    // ответ сервера. Пороговый детект 5.0–5.5.6 ушёл: при живом RTC он
    // давал ложный «synced» без всякого ответа — самодиагностика обязана
    // говорить правду.

    // Заявки от событий/API — исполняем здесь (loopTask), не в их контексте
    if (_ntpPending) {
        _ntpPending = false;
        if (_ntpState != NtpState::DISPATCHING &&
            _ntpState != NtpState::RESOLVING &&
            _ntpState != NtpState::REQUESTED) {
            requestNtp();   // в полёте — дубль не нужен, летящий и есть свежий
        }
    }

    switch (_ntpState) {
    case NtpState::DISPATCHING:
    case NtpState::RESOLVING: {
        const bool wasResolving = (_ntpState == NtpState::RESOLVING);
        const uint32_t timeout = wasResolving ? NTP_DNS_TIMEOUT_MS
                                              : NTP_DISPATCH_TIMEOUT_MS;
        const NtpPhase ph = s_job.phase;
        if (ph == PH_SENT || ph == PH_RX_READY) {
            // PH_RX_READY здесь — норма: по LAN ответ может прийти раньше
            // ближайшего тика loopTask (урок бенча 5.5.8). Ответ разберёт
            // ветка REQUESTED следующим тиком; RTT посчитается по меткам
            // tcpip_thread, так что задержка тика на точность не влияет.
            IPAddress rip(s_job.resolvedIp);
            if (wasResolving) {
                log(LogLevel::Info, "NTP: DNS %s -> %s", _ntpServer,
                    rip.toString().c_str());
            }
            _ntpAttempts++;
            _ntpState = NtpState::REQUESTED;
            _ntpStateSinceMs = millis();
            log(LogLevel::Info, "NTP: запрос -> %s:123 (%s), попытка #%lu",
                rip.toString().c_str(), _ntpServer,
                (unsigned long)_ntpAttempts);
        } else if (ph == PH_RESOLVING && !wasResolving) {
            // Переход DISPATCHING -> RESOLVING (5.5.9: в 5.5.8 этой ветки
            // не было — hostname навсегда застревал в DISPATCHING, бенч
            // 11.08.2026: «tcpip_thread не исполнил задание» по кругу)
            _ntpState = NtpState::RESOLVING;
            _ntpStateSinceMs = millis();
            log(LogLevel::Info, "NTP: резолв %s (async DNS)...", _ntpServer);
        } else if (ph == PH_DNS_FAIL) {
            _ntpState = NtpState::WAIT_RETRY;
            _ntpStateSinceMs = millis();
            log(LogLevel::Warning,
                "NTP: DNS %s НЕ РЕЗОЛВИТСЯ; переарм через %u с",
                _ntpServer, (unsigned)(NTP_RETRY_INTERVAL_MS / 1000));
        } else if (ph == PH_SEND_FAIL) {
            _ntpState = NtpState::WAIT_RETRY;
            _ntpStateSinceMs = millis();
            log(LogLevel::Warning,
                "NTP: ОТПРАВКА НЕ УДАЛАСЬ (%s) — UDP-стек отказал; "
                "переарм через %u с",
                _ntpServer, (unsigned)(NTP_RETRY_INTERVAL_MS / 1000));
        } else if (millis() - _ntpStateSinceMs > timeout) {
            _ntpState = NtpState::WAIT_RETRY;
            _ntpStateSinceMs = millis();
            if (wasResolving) {
                log(LogLevel::Warning,
                    "NTP: DNS %s — таймаут резолва %u с; переарм через %u с",
                    _ntpServer, (unsigned)(NTP_DNS_TIMEOUT_MS / 1000),
                    (unsigned)(NTP_RETRY_INTERVAL_MS / 1000));
            } else {
                log(LogLevel::Warning,
                    "NTP: tcpip_thread не исполнил задание за %u с — "
                    "стек мёртв? переарм через %u с",
                    (unsigned)(NTP_DISPATCH_TIMEOUT_MS / 1000),
                    (unsigned)(NTP_RETRY_INTERVAL_MS / 1000));
            }
        }
        break;
    }

    case NtpState::REQUESTED: {
        if (s_job.phase == PH_RX_READY) {
            uint32_t unix = 0;
            if (sh_sntp::parseReply(s_job.rxBuf, sh_sntp::PACKET_LEN, unix)) {
                _ntpLastRttMs = s_job.rxAtMs - s_job.sentAtMs;
                _ntpSynced = true;
                _ntpState = NtpState::SYNCED;
                _ntpResyncAtMs = millis() + NTP_RESYNC_INTERVAL_MS;
                // Полный цикл NTP -> системное -> DS3231, как в монолите
                setSystemTime((time_t)unix);
                log(LogLevel::Info,
                    "NTP synced: %s, RTT %lu мс — время и RTC обновлены",
                    _ntpServer, (unsigned long)_ntpLastRttMs);
            } else {
                log(LogLevel::Warning,
                    "NTP: мусорный ответ (%u B) — проигнорирован",
                    (unsigned)sh_sntp::PACKET_LEN);
                s_job.phase = PH_SENT;   // перевооружить приём до таймаута
            }
        } else if (millis() - _ntpStateSinceMs > NTP_REPLY_TIMEOUT_MS) {
            _ntpState = NtpState::WAIT_RETRY;
            _ntpStateSinceMs = millis();
            log(LogLevel::Warning,
                "NTP: %s молчит %u с (попытка #%lu) — переарм через %u с",
                _ntpServer, (unsigned)(NTP_REPLY_TIMEOUT_MS / 1000),
                (unsigned long)_ntpAttempts,
                (unsigned)(NTP_RETRY_INTERVAL_MS / 1000));
        }
        break;
    }

    case NtpState::WAIT_RETRY:
        if (millis() - _ntpStateSinceMs >= NTP_RETRY_INTERVAL_MS) {
            requestNtp();
        }
        break;

    case NtpState::SYNCED:
        if ((long)(millis() - _ntpResyncAtMs) >= 0) {
            log(LogLevel::Info, "NTP: суточный ресинк");
            requestNtp();
        }
        break;

    default:
        break;
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
