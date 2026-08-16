// ============================================================================
// TimeService.h — СИСТЕМНОЕ ВРЕМЯ МикроОС 5.0
// ============================================================================
// Фаза 1. ЕДИНСТВЕННЫЙ источник времени в системе (принцип 6 базовой
// архитектуры: один владелец факта). Устраняет дубли v4.2.2: TIME_SYNC vs
// RTC_SYNCED, TEMPERATURE_UPDATE vs TEMP_UPDATE.
//
// Иерархия источников (как в монолите v2.5.0, RTC + RAM-зеркало + NTP):
//   1. DS3231 (Ds3231Driver) — главный; переживает обесточку (батарейка);
//   2. системное время ESP32 — зеркало: синхронится из RTC раз в час
//      (монолит: hourly sync) и после каждого восстановления шины;
//   3. NTP (Фаза 3, порция 2): запускается по событию NET_EVENT_IP_CHANGED,
//      при успехе пишет и в системное время, и в RTC (setSystemTime).
//      Смещение пояса — sys.tz_offset (монолит: целочисленный GMT-offset).
//
// Что сервис ДЕЛАЕТ: getUnixTime/isTimeValid, проверка интервалов
// (для расписаний — например, запрет кнопки выхода 22:00–06:00), события
// TIME_EVENT_SYNCED/RTC_LOST/RTC_RESTORED.
// Чего сервис НЕ ДЕЛАЕТ: будильники, ночной режим, рабочие часы — это
// Scheduler (доменный сервис, Phase 3), подписчик TIME_EVENT_*.
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "TimeInterval.h"   // чистая логика интервалов (D2: host-тесты)
#include <ctime>
#include <IPAddress.h>

// Периоды (из монолита v2.5.0)
constexpr uint32_t TIME_SYNC_INTERVAL_MS = 3600000; // RTC -> системное, 1 час
constexpr uint32_t TIME_TICK_MS          = 1000;    // контроль раз в секунду

// NTP (5.5.7+, свой SNTP-клиент): тайминги конечного автомата
constexpr uint32_t NTP_DISPATCH_TIMEOUT_MS = 5000;   // исполнение задания tcpip_thread (5.5.8)
constexpr uint32_t NTP_REPLY_TIMEOUT_MS   = 5000;    // ответ сервера
constexpr uint32_t NTP_DNS_TIMEOUT_MS     = 15000;   // резолв hostname (5.5.10: холодный DNS MikroTik отвечает 5-15 с — урок бенча 5.5.9)
constexpr uint32_t NTP_RETRY_INTERVAL_MS  = 60000;   // переарм после неудачи
constexpr uint32_t NTP_RESYNC_INTERVAL_MS = 86400000; // ресинк раз в сутки

/// Состояния NTP-автомата — видны в API/панели (самодиагностика 5.5.7+)
enum class NtpState : uint8_t {
    IDLE = 0,    // не запускался (нет сети / выключен в конфиге)
    DISPATCHING, // задание отправлено в tcpip_thread (5.5.8)
    RESOLVING,   // hostname в резолве (async DNS)
    REQUESTED,   // запрос на проводе, ждём ответ
    SYNCED,      // синхронизирован в этой сессии
    WAIT_RETRY,  // неудача, ждём переарма (NTP_RETRY_INTERVAL_MS)
};

class TimeService : public ModuleBase {
public:
    static TimeService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "TimeService"; }
    const char* getVersion() const override { return "5.5.10"; }  // 5.5.10: DNS-таймаут 15 с под холодный upstream (урок бенча 5.5.9)
    ModuleId getModuleId() const override { return 0x0006; }

    void registerExtensions() override;   // схема sys.ntp_* / sys.tz_offset
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return TIME_TICK_MS; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- API СЕРВИСА (вызывают модули и приложения) ---------------------------
    /// Unix-время (сек). 0 — время недостоверно (RTC мёртв, NTP не было).
    time_t getUnixTime() const;

    /// Достоверно ли время СЕЙЧАС. Важно для Fail-Safe: монолит при мёртвом
    /// RTC всегда разрешал кнопку выхода — правило живёт на этом флаге.
    bool isTimeValid() const { return _timeValid; }

    // Источник достоверности — для панелей/API (5.5.6: у home_master нет
    // RTC, и время было нигде не видно; замок часы показывает, мастер — нет)
    bool ntpSynced() const { return _ntpSynced; }
    bool rtcAlive()  const { return _rtcAlive; }

    // Самодиагностика NTP (5.5.7): состояние автомата и счётчики — в API
    NtpState   ntpState()    const { return _ntpState; }
    uint32_t   ntpAttempts() const { return _ntpAttempts; }
    uint32_t   ntpLastRttMs() const { return _ntpLastRttMs; }
    const char* ntpStateStr() const;

    /// Локальное время в struct tm. false — время недостоверно.
    bool getLocalTime(struct tm& out) const;

    /// Проверка "сейчас внутри интервала HH:MM–HH:MM" с переходом через
    /// полночь (22:00–06:00 — внутри с 22 вечера до 6 утра).
    /// Недостоверное время -> false (решение Fail-Safe — у вызывающего).
    bool isTimeInInterval(const char* startHHMM, const char* endHHMM) const;

    /// Делегат к чистой TimeInterval.h (D2: логика покрыта host-тестами).
    static bool minutesInInterval(int nowMin, int startMin, int endMin) {
        return sh_time::minutesInInterval(nowMin, startMin, endMin);
    }

    /// Записать время из внешнего источника (NTP, Phase 2; ручная
    /// синхронизация из веб): в системное время И в RTC.
    bool setSystemTime(time_t unixUtc);

    /// Принудительный NTP-запрос из веб (кнопка «Синхр. время» в панели).
    /// Асинхронно: ответ SNTP ловит tick(). false — NTP выключен в конфиге.
    bool forceNtpSync();

private:
    TimeService() = default;

    /// Прочитать RTC и обновить системное время + флаг достоверности.
    void syncFromRtc(const char* reason);

    /// Запустить NTP-цикл (из tick: _ntpPending / переарм / ресинк).
    /// ТОЛЬКО loopTask: конфиг + пост задания в tcpip_thread (5.5.8).
    void requestNtp();

    bool     _timeValid = false;       // время достоверно
    bool     _rtcAlive = false;        // DS3231 отвечает (из событий шины)
    uint32_t _lastSyncMs = 0;          // последняя синхронизация RTC->system
    uint32_t _uptimeOffsetSec = 0;     // резерв: millis-подобное время без RTC

    // NTP (5.5.7+): конечный автомат со своим SNTP-клиентом (SntpCore.h).
    // Каждый переход — строка в журнале: стек больше не молчит.
    // 5.5.8: ни одного вызова lwIP вне tcpip_thread — сетевые события и
    // HTTP-API ставят только флаг _ntpPending, исполняет tick (loopTask).
    NtpState _ntpState    = NtpState::IDLE;
    bool     _ntpSynced   = false;     // был РЕАЛЬНЫЙ ответ сервера (не порог!)
    uint32_t _ntpAttempts = 0;         // отправлено запросов за сессию
    uint32_t _ntpLastRttMs = 0;        // RTT последнего ответа
    uint32_t _ntpStateSinceMs = 0;     // когда вошли в текущее состояние
    uint32_t _ntpResyncAtMs = 0;       // суточный ресинк после успеха
    volatile bool _ntpPending = false; // заявка на запуск (onEvent/forceNtpSync)
    static constexpr size_t NTP_SERVER_LEN = 64;
    char     _ntpServer[NTP_SERVER_LEN] = {0}; // сервер из конфига (для лога)
};
