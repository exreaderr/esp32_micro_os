// ============================================================================
// SmartLockApp.h — ПОЛИТИКА ДОСТУПА СКУД (профильный модуль, «голова»)
// ============================================================================
// Сценарии перенесены из монолита smart_lock v2.5.0 (части 3–6) и переложены
// на сервисы 5.0. Модуль НЕ трогает железо напрямую:
//   карта   <- WiegandDriver (DRV_EVENT_WIEGAND_CARD)
//   кто     -> CardStore (база)
//   открыть -> LockControl (openPulse/setTriggerHold)
//   сказать -> AudioService (say/sayRaw, тихий режим — политика профиля)
//   факты   -> ACCESS_EVENT_GRANTED/DENIED (ядро: аудит B3 + MQTT E2)
//              sl_ev::* (доменные: режимы, дверь, база)
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include "SmartLockEvents.h"
#include "CardDbFormat.h"   // SlUser

// Причины отказа (code события ACCESS_EVENT_DENIED)
enum class SlDenyReason : int32_t {
    UNKNOWN_CARD = 1, EXPIRED = 2, SCHEDULE_BLOCK = 3, NOT_PROVISIONED = 4,
    BLOCKED_CARD = 5   // карта в базе, но заблокирована (потеряна)
};

class SmartLockApp : public ModuleBase {
public:
    static SmartLockApp& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "SmartLockApp"; }
    const char* getVersion() const override { return "5.5.14"; }     // 5.5.14: правило 23 — порядок вкладок (Сеть, ПАЗ, Система, Админ → профильные), «Служебные» не отображаются (web); правило зеркала — веб-доступ чтит blocked/expiry как карта (репорт 03.09)
    ModuleId getModuleId() const override { return 0x1002; }   // профиль

    void registerExtensions() override;   // конфиг lock.*, SoundPack
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 100; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- СОСТОЯНИЕ (для API / health-checks) ---------------------------------
    LockMode getMode() const { return _mode; }
    bool isDoorAlarm() const { return _doorAlarm; }
    bool isForcedAlarm() const { return _forcedAlarm; }
    const char* getLastCard() const { return _lastCard; }
    const char* getLastUser() const { return _lastUser; }

    // --- КОМАНДЫ (API, MQTT, мастер-ключ) --------------------------------------
    /// Дистанционное открытие (веб/MQTT). userName — жилец, чей ПИН принят
    /// (per-user ПИН: открытие тоже персонифицировано — «последний доступ»).
    void remoteOpen(SlOpenSource source, const char* userName = nullptr);

    /// Журнал отказа веб-доступа по blocked/expiry (правило зеркала,
    /// 03.09.2026). Вызывает SmartLockUi — сам он не ModuleBase, а log()
    /// ядра protected. reason: "blocked" | "expired" | "expired_notime".
    void logWebDeny(const char* userName, const char* reason) const;
    /// Смена режима извне (API). Возвращает новый режим.
    void setMode(LockMode mode);
    /// Режим чтения карты для веб-UI (10 с, монолит: web_read_mode).
    void startWebRead();
    bool isWebReadActive() const { return _webReadActive; }
    /// Карта, пойманная в web-read ("" — ещё нет; читается после события).
    const char* webReadCard() const { return _webReadCard; }

    /// Разрешена ли кнопка выхода СЕЙЧАС (политика расписания + Fail-Safe
    /// при мёртвом RTC). Используют и кнопка, и веб-статус («РАЗРЕШЕНА»).
    bool isExitAllowedNow() const;

    /// Сброс флага ПАЗ оператором (монолит: /api/paz/reset): снять тревоги
    /// и заглушить сирену, не дожидаясь закрытия двери.
    void resetAlarms();

    // --- ПОГОДА ОТ УД (внешняя MQTT-подписка) ---------------------------------
    /// HA (сегодня) или своя метеостанция (завтра) публикует в топик
    /// lock.weather_topic JSON: {"temp":20.6,"feels_like":21.0,"state":"rainy"}
    /// или короткие алиасы монолита: {"temp":20.6,"feel":21.0,"text":"rainy"}.
    /// state/text — код weather-элемента HA (rainy, sunny, cloudy...);
    /// перевод в русский — карта weatherMap в панели (дословно из монолита).
    bool weatherValid() const { return _wxValid; }
    float weatherTemp() const { return _wxTemp; }
    float weatherFeelsLike() const { return _wxFeel; }
    const char* weatherState() const { return _wxState; }

private:
    SmartLockApp() = default;

    // --- Сценарии (монолит: loop-обработка карты, часть 6) --------------------
    void handleCard(const char* cardHex);
    void handleMasterCard();
    void handleExitButton();
    void handleDoorOpen();
    void handleDoorClosed();
    void grantAccess(const char* card, const SlUser* user);
    void denyAccess(const char* card, SlDenyReason reason, const char* phrase);

    // --- Home Assistant discovery (E2): устройство объявляет себя само ------
    /// Discovery-конфиги (retained) + первичные состояния. Вызывается на
    /// каждый CONNECT: брокер без нашего конфига (сброс/новый) восполняется.
    void publishHaDiscovery();
    /// Retained-состояния сущностей: замок/дверь/тревога.
    void publishHaStates();
    /// Проброс ядерной cmd/<verb>: «open» -> remoteOpen(MQTT).
    static bool onMqttCmd(const char* verb, const char* body);

    // --- Озвучка с политикой тихого режима (lock.quiet_mode) ------------------
    /// Тихий режим монолита: звучат ТОЛЬКО критические тревоги
    /// (sl_track::allowedInQuietMode). actionTrack — номер фразы в папке 03
    /// для сверки с белым списком.
    bool sayFiltered(const char* phrase, uint8_t actionTrack);
    /// Цепочка приветствия: время суток -> имя -> «доступ разрешён».
    void playGreeting(const SlUser* user);

    // --- Данные -----------------------------------------------------------------
    LockMode _mode = LockMode::NORMAL;

    // Дверь/тревоги
    bool _doorAlarm = false;        // «дверь открыта долго»
    bool _forcedAlarm = false;      // «взлом»: открыта без разблокировки
    uint32_t _lastUnlockMs = 0;     // для детекта взлома

    // Кнопка выхода
    // (сырой факт — от LockControl; расписание проверяем здесь)

    // Веб-чтение карты
    bool _webReadActive = false;
    uint32_t _webReadStart = 0;
    char _webReadCard[9] = "";
    static constexpr uint32_t WEB_READ_TIMEOUT_MS = 10000;

    // Последнее событие (для API)
    char _lastCard[9] = "None";
    char _lastUser[65] = "";

    // Погода от УД (кэш последнего сообщения брокера). Кэш персистентный
    // (/wx_cache.json): урок 5.0.x — после ребута карточка висела в
    // «Синхронизация…» до ближайшей публикации HA (до 15 мин), хотя данные
    // на устройстве уже были. Пишем на каждое валидное сообщение (редко,
    // обычно 1/15 мин — износ флеша ничтожен), читаем в start().
    static void onWeatherMqtt(const char* topic, const char* payload);
    void parseWeather(const char* payload);
    void saveWeatherCache();
    void loadWeatherCache();
    bool  _wxValid = false;
    float _wxTemp = 0.0f;
    float _wxFeel = 0.0f;
    char  _wxState[16] = "";

    // --- Даталоггер (п.5): каналы профиля в ядерном DataLogService --------
    // cpu_t/heap — сэмпл из TelemetryService.snapshot() раз в 60 с (tick);
    // wx_t — точка на каждое сообщение погоды (обычно 1/15 мин).
    int8_t   _chCpu  = -1;
    int8_t   _chHeap = -1;
    int8_t   _chWx   = -1;
    uint32_t _lastDlogMs = 0;
    static constexpr uint32_t DLOG_SAMPLE_MS = 60000;
    // Авто-чистка просроченных временных ключей (решение владельца
    // 03.09.2026): проверка раз в 60 с, чистка при смене штампа суток
    // (покрывает и полночь, и «устройство было выключено ночью»).
    uint32_t _lastPurgeCheckMs = 0;
    static constexpr uint32_t PURGE_CHECK_MS = 60000;
    int _purgeDay = -1;                     // tm_year*400+tm_yday; -1 = ещё не было
};
