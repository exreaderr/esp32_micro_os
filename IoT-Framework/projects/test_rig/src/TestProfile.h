// ============================================================================
// TestProfile.h — ТЕСТОВЫЙ ПРОФИЛЬ ДЛЯ ПРОВЕРКИ КАРКАСА (Фаза 0–1)
// ============================================================================
// НЕ боевой профиль. Назначение: скомпилировать и слинковать всё ядро и
// драйверы, прогнать цепочки на живом железе WT32-ETH01:
//   1. ResourceManager: умышленный конфликт пина -> лог CONFLICT;
//   2. EventBus: подписка TestApp на события драйверов -> доставка;
//   3. Drivers: EspTemp (температура), Ds3231 (время по I2C),
//      DFPlayer (UART/клон MP3-TF-16P);
//      (Wiegand вынесен в каталог профильной периферии — его проверяет
//      профиль СКУД, не тестовый стенд ядра);
//   4. Kernel: bootloop-счётчик, детект Safe Mode (кнопка GPIO14);
//   5. BusManager: скан I2C при старте, health-трекинг.
// ============================================================================
#pragma once

#include <core/IDeviceProfile.h>
#include <core/Kernel.h>
#include <core/ModuleBase.h>
#include <core/DriverRegistry.h>
#include <core/BusManager.h>
#include <core/Events.h>
#include <services/ConfigService.h>
#include <services/NetworkManager.h>
#include <services/AuthService.h>
#include <services/TelemetryService.h>
#include <services/HttpService.h>
#include <services/IUiProvider.h>
#include <services/AudioService.h>
#include <services/ISoundPack.h>
#include <drivers/EspTempDriver.h>
#include <core/ConformanceTest.h>
#include <drivers/Ds3231Driver.h>
#include <drivers/DfPlayerDriver.h>

// ============================================================================
// ТЕСТОВЫЙ UI-ПРОВАЙДЕР: проверка инжекции веб-интерфейса (Фаза 3)
// ============================================================================
// Минимальная реализация IUiProvider: секция на публичной странице +
// профильный API /api/dev/ping. Доказывает, что профильный UI встраивается
// в ядерный веб-сервер без правок ядра.
// ============================================================================
class TestUiProvider : public IUiProvider {
public:
    const char* uiTitle() const override { return "test_rig"; }

    size_t renderPublicHtml(char* buf, size_t bufSize) override {
        int n = snprintf(buf, bufSize,
            "<b>Тестовый стенд МикроОС 5.0</b>"
            "<p>Инжекция UI профиля работает. "
            "API: <a href=\"/api/dev/ping\">/api/dev/ping</a></p>");
        return n > 0 ? (size_t)n : 0;
    }

    bool handleApi(const char* pathTail, const ShUiRequest& req,
                   char* responseBuf, size_t bufSize,
                   int& statusCode) override {
        (void)req;
        if (strcmp(pathTail, "ping") == 0) {
            int n = snprintf(responseBuf, bufSize,
                             "{\"pong\":1,\"ms\":%lu}",
                             (unsigned long)millis());
            (void)n;
            statusCode = 200;
            return true;
        }
        return false;   // неизвестный профильный путь -> 404 ядра
    }
};

// Прогон D1-стенда (определён в конце файла — после класса TestProfile)
void runConformanceForTestProfile();

// ============================================================================
// ТЕСТОВЫЙ SOUNDPACK: звуковое наполнение как данные (AudioService)
// ============================================================================
// Проверяет: слот ядра "sys.boot" (озвучка без участия профиля), приоритеты,
// ADVERT-флаг, зацикленную сирену. Файлы на SD стенда: /01/001..003, /04/099.
// ============================================================================
class TestSoundPack : public ISoundPack {
public:
    const char* packName() const override { return "test_rig_ru_v1"; }
    const SoundPhrase* phrases(uint8_t& count) const override {
        static const SoundPhrase TABLE[] = {
            // имя            папка трек приоритет                       флаги
            { "sys.boot",       1, 1, (uint8_t)SndPriority::Normal,    0 },
            { "test.beep",      1, 2, (uint8_t)SndPriority::Ambient,   0 },
            { "test.notice",    0, 1, (uint8_t)SndPriority::Important,
                                                                SND_FLAG_ADVERT },
            { "test.alarm",     4, 99, (uint8_t)SndPriority::Alarm,
                                                                SND_FLAG_LOOP },
        };
        count = sizeof(TABLE) / sizeof(TABLE[0]);
        return TABLE;
    }
};

// ============================================================================
// ТЕСТОВЫЙ МОДУЛЬ: подписчик событий драйверов + проверка реестра
// ============================================================================
class TestApp : public ModuleBase {
public:
    static TestApp& getInstance() {
        static TestApp instance;
        return instance;
    }

    const char* getName() const override { return "TestApp"; }
    const char* getVersion() const override { return "0.1.0"; }
    ModuleId getModuleId() const override { return 0x1000; }

    void registerExtensions() override {
        // --- Проверка 4: инжекция конфиг-схемы (IConfigProvider) ---------
        ConfigService::getInstance().addFields("Тест", {
            { "test.pulse_ms", ConfigType::UINT, "500", 100, 5000, CFG_NONE,
              "Тест", "Тестовый импульс, мс" },
            { "test.enabled",  ConfigType::BOOL, "true", 0, 0, CFG_NONE,
              "Тест", "Тестовый флаг" },
        });

        // --- Проверка 7: инжекция веб-UI профиля (IUiProvider) -----------
        static TestUiProvider ui;
        HttpService::getInstance().setUiProvider(&ui);

        // --- Проверка 9: инжекция звукового наполнения (ISoundPack) ------
        static TestSoundPack pack;
        AudioService::getInstance().setSoundPack(&pack);
    }

    void init() override {
        // --- Проверка 5: чтение инжектированных полей ----------------------
        log(LogLevel::Info, "cfg test.pulse_ms=%lu test.enabled=%d",
            (unsigned long)cfgGetUInt("test.pulse_ms", 0),
            (int)cfgGetBool("test.enabled", false));
        // Запись с валидацией: 800 в диапазоне 100–5000 -> true + событие
        bool setOk = ConfigService::getInstance().set("test.pulse_ms", "800");
        log(LogLevel::Info, "cfg set test (expect 1): %d", setOk);
        // Вне диапазона: 99999 -> false
        bool setBad = ConfigService::getInstance().set("test.pulse_ms", "99999");
        log(LogLevel::Info, "cfg set invalid (expect 0): %d", setBad);

        // --- Проверка 1: ResourceManager ловит умышленный конфликт --------
        // GPIO16 занят платформой (platform.eth_phy_power) — попытка занять
        // его за тестом ДОЛЖНА дать CONFLICT в лог и false.
        bool stolen = ResourceManager::getInstance().claimGpio(16, "test.steal");
        log(LogLevel::Info, "resource conflict test (expect false): %d", stolen);

        // --- Проверка 6: rate-limiter AuthService (C3) ----------------------
        // max_fails=5 (умолчание): 4 неудачи -> ещё не заблокирован,
        // 5-я -> блокировка. Ключ "test" не пересекается с "admin".
        AuthService& auth = AuthService::getInstance();
        uint8_t left = 255;
        for (uint8_t i = 0; i < 5; ++i) left = auth.noteFailure("test");
        log(LogLevel::Info,
            "rate-limit: after 5 fails left=%u (expect 0), limited=%d (expect 1)",
            left, (int)auth.isRateLimited("test"));
        auth.noteSuccess("test");
        log(LogLevel::Info, "rate-limit: after success limited=%d (expect 0)",
            (int)auth.isRateLimited("test"));
        log(LogLevel::Info, "provisioning state: %s",
            auth.isProvisioned() ? "provisioned" : "SETUP REQUIRED");

        // --- Проверка 10: звуковой диспетчер (AudioService) ---------------
        // "sys.boot" произнесёт сам AudioService в start() (слот ядра).
        // Здесь — очередь и приоритеты: beep (Ambient) + alarmOn + alarmOff.
        bool beep = AudioService::getInstance().say("test.beep");
        bool unknown = AudioService::getInstance().say("test.nonexistent");
        log(LogLevel::Info, "audio: say(beep)=%d, say(unknown)=%d (expect 0)",
            (int)beep, (int)unknown);

        // --- Проверка 2: карта ресурсов в лог -------------------------------
        char report[1024];
        ResourceManager::getInstance().report(report, sizeof(report));
        Serial.println(report);

        // --- Проверка 3: скан I2C-шины ---------------------------------------
        BusManager::getInstance().scan();

        _initialized = true;
    }

    void start() override {
        // Подписка на события драйверов — проверка доставки EventBus
        EventBus::getInstance().subscribe(DRV_EVENT_TEMP_UPDATE, this);
        EventBus::getInstance().subscribe(DRV_EVENT_AUDIO_STARTED, this);
        EventBus::getInstance().subscribe(DRV_EVENT_AUDIO_FINISHED, this);
        EventBus::getInstance().subscribe(DRV_EVENT_AUDIO_OFFLINE, this);
        EventBus::getInstance().subscribe(DRV_EVENT_BUS_RECOVERED, this);
        EventBus::getInstance().subscribe(DRV_EVENT_BUS_DEAD, this);
        EventBus::getInstance().subscribe(SH_EVENT_TICK_OVERRUN, this);
        // Сеть и аутентификация (Фаза 2, порция 2)
        EventBus::getInstance().subscribe(NET_EVENT_CONNECTED, this);
        EventBus::getInstance().subscribe(NET_EVENT_DISCONNECTED, this);
        EventBus::getInstance().subscribe(NET_EVENT_IP_CHANGED, this);
        EventBus::getInstance().subscribe(NET_EVENT_GATEWAY_LOST, this);
        EventBus::getInstance().subscribe(NET_EVENT_GATEWAY_RESTORED, this);
        EventBus::getInstance().subscribe(NET_EVENT_DISABLED, this);
        EventBus::getInstance().subscribe(SH_EVENT_DEGRADED_LEVEL, this);
        EventBus::getInstance().subscribe(AUTH_EVENT_LOGIN, this);
        EventBus::getInstance().subscribe(AUTH_EVENT_LOGIN_FAILED, this);
        EventBus::getInstance().subscribe(AUTH_EVENT_LOCKED_OUT, this);
        EventBus::getInstance().subscribe(AUTH_EVENT_SETUP_REQUIRED, this);
        EventBus::getInstance().subscribe(TEL_EVENT_SNAPSHOT, this);
        EventBus::getInstance().subscribe(SH_EVENT_MQTT_CONNECTED, this);
        EventBus::getInstance().subscribe(SH_EVENT_MQTT_DISCONNECTED, this);
        EventBus::getInstance().subscribe(SH_EVENT_MQTT_MESSAGE, this);
        _started = true;
        log(LogLevel::Info, "TestApp started, subscribed to driver events");

        // --- Проверка 8 (D1): стенд соответствия профиля -------------------
        // Реализация — runConformanceForTestProfile() в конце файла
        // (TestProfile объявлен ниже TestApp в этом заголовке).
        runConformanceForTestProfile();
    }

    void stop() override {
        EventBus::getInstance().unsubscribeAll(this);
        _started = false;
    }

    void tick() override {
        // Раз в 10 с: сводка — температура, время RTC, метрики шины
        uint32_t now = millis();
        if (now - _lastSummaryMs < 10000) return;
        _lastSummaryMs = now;

        auto* temp = DriverRegistry::getInstance().findAs<EspTempDriver>("esp_temp");
        if (temp) {
            log(LogLevel::Info, "CPU temp: %.1f C, bus dropped=%lu, hw=%u",
                temp->getTemperature(),
                (unsigned long)EventBus::getInstance().getDroppedCount(),
                EventBus::getInstance().getHighWatermark());
        }
        auto* rtc = DriverRegistry::getInstance().findAs<Ds3231Driver>("ds3231");
        if (rtc) {
            struct tm t;
            if (rtc->getDateTime(t)) {
                log(LogLevel::Info, "RTC: %04d-%02d-%02d %02d:%02d:%02d",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec);
            } else {
                log(LogLevel::Warning, "RTC read failed");
            }
        }

        // Сеть: идентичность (E1), адрес, уровень деградации (A3)
        NetworkService& net = NetworkService::getInstance();
        char ip[16];
        net.ipString(ip, sizeof(ip));
        log(LogLevel::Info, "NET: host=%s id=%s ip=%s level=%d gw_rtt=%lu",
            net.hostname(), net.deviceId(), ip,
            (int)net.degradationLevel(),
            (unsigned long)net.gatewayRttMs());

        // Телеметрия (B1): полный JSON-снимок — то, что увидит УД
        char tel[256];
        TelemetryService::getInstance().toJson(tel, sizeof(tel));
        log(LogLevel::Info, "TEL: %s", tel);
    }
    uint32_t getTickIntervalMs() const override { return 500; }

    bool canHandleEvent(int32_t id) const override {
        return id == DRV_EVENT_TEMP_UPDATE || id == DRV_EVENT_AUDIO_STARTED ||
               id == DRV_EVENT_AUDIO_FINISHED || id == DRV_EVENT_AUDIO_OFFLINE ||
               id == DRV_EVENT_BUS_RECOVERED || id == DRV_EVENT_BUS_DEAD ||
               id == SH_EVENT_TICK_OVERRUN ||
               id == NET_EVENT_CONNECTED || id == NET_EVENT_DISCONNECTED ||
               id == NET_EVENT_IP_CHANGED || id == NET_EVENT_GATEWAY_LOST ||
               id == NET_EVENT_GATEWAY_RESTORED || id == NET_EVENT_DISABLED ||
               id == SH_EVENT_DEGRADED_LEVEL ||
               id == AUTH_EVENT_LOGIN || id == AUTH_EVENT_LOGIN_FAILED ||
               id == AUTH_EVENT_LOCKED_OUT || id == AUTH_EVENT_SETUP_REQUIRED ||
               id == TEL_EVENT_SNAPSHOT ||
               id == SH_EVENT_MQTT_CONNECTED || id == SH_EVENT_MQTT_DISCONNECTED ||
               id == SH_EVENT_MQTT_MESSAGE;
    }

    void onEvent(int32_t eventId, const ShEventData* data) override {
        switch (eventId) {
            case DRV_EVENT_AUDIO_FINISHED:
                log(LogLevel::Info, "audio finished, track=%ld",
                    data ? (long)data->code : 0L);
                break;
            case DRV_EVENT_AUDIO_OFFLINE:
                log(LogLevel::Error, "DFPlayer OFFLINE");
                break;
            case SH_EVENT_TICK_OVERRUN:
                log(LogLevel::Warning, "tick overrun %ld ms at module 0x%lX",
                    data ? (long)data->code : 0L,
                    data ? (unsigned long)data->sourceModule : 0UL);
                break;
            case NET_EVENT_CONNECTED:
                log(LogLevel::Info, "NET: link up"); break;
            case NET_EVENT_DISCONNECTED:
                log(LogLevel::Warning, "NET: down (code=%ld)",
                    data ? (long)data->code : 0L); break;
            case NET_EVENT_IP_CHANGED:
                log(LogLevel::Info, "NET: IP %s",
                    data ? data->payload : "?"); break;
            case NET_EVENT_GATEWAY_LOST:
                log(LogLevel::Warning, "NET: gateway LOST"); break;
            case NET_EVENT_GATEWAY_RESTORED:
                log(LogLevel::Info, "NET: gateway restored, rtt=%ld ms",
                    data ? (long)data->code : 0L); break;
            case NET_EVENT_DISABLED:
                log(LogLevel::Info, "NET: disabled (local mode)"); break;
            case SH_EVENT_DEGRADED_LEVEL:
                log(LogLevel::Info, "DEGRADATION -> %s",
                    data ? data->payload : "?"); break;
            case AUTH_EVENT_SETUP_REQUIRED:
                log(LogLevel::Warning, "AUTH: SETUP REQUIRED (C1)"); break;
            case AUTH_EVENT_LOGIN:
                log(LogLevel::Info, "AUTH: login role=%ld from %s",
                    data ? (long)data->code : 0L,
                    data ? data->payload : "?"); break;
            case AUTH_EVENT_LOGIN_FAILED:
                log(LogLevel::Warning, "AUTH: failed, %ld attempts left",
                    data ? (long)data->code : 0L); break;
            case AUTH_EVENT_LOCKED_OUT:
                log(LogLevel::Warning, "AUTH: locked out for %ld s",
                    data ? (long)data->code : 0L); break;
            case SH_EVENT_MQTT_CONNECTED:
                log(LogLevel::Info, "MQTT: broker connected"); break;
            case SH_EVENT_MQTT_DISCONNECTED:
                log(LogLevel::Warning, "MQTT: broker lost"); break;
            case SH_EVENT_MQTT_MESSAGE:
                // E2: событие другого устройства экосистемы
                log(LogLevel::Info, "MQTT peer event: %s",
                    data ? data->payload : "?"); break;
            default:
                break;   // TEMP_UPDATE/BUS_* — не спамим, сводка в tick()
        }
    }

private:
    TestApp() = default;
    uint32_t _lastSummaryMs = 0;
};

// ============================================================================
// ТЕСТОВЫЙ ПРОФИЛЬ
// ============================================================================
class TestProfile : public IDeviceProfile {
public:
    const char* profileId() const override { return "test_rig"; }

    void describeHardware(HardwareManifest& m) override {
        // Safe Mode — GPIO14, DFPlayer 17/5/36 (UART2)
        m.safeModePin  = 14;
        m.dfPlayerRx   = 17;
        m.dfPlayerTx   = 5;
        m.dfPlayerBusy = 36;
        m.dfPlayerUart = 2;
    }

    void registerDrivers(const HardwareManifest& m) override {
        DriverRegistry& dr = DriverRegistry::getInstance();
        dr.add(&Ds3231Driver::getInstance());

        // Клон MP3-TF-16P: компат-пресет
        auto dfCfg = DfPlayerDriver::cloneMP3TF16P();
        dfCfg.stuckTimeoutMs = 12000;
        DfPlayerDriver::getInstance().configure(
            m.dfPlayerUart, (uint8_t)m.dfPlayerRx, (uint8_t)m.dfPlayerTx,
            (uint8_t)m.dfPlayerBusy, dfCfg);
        dr.add(&DfPlayerDriver::getInstance());
        // EspTempDriver регистрирует сам Kernel как базовый драйвер платформы
    }

    void registerModules(Kernel& k) override {
        k.registerModule(&TestApp::getInstance(), /*prio*/ 10, /*tickMs*/ 0);
    }
};

// ============================================================================
// D1: ПРОГОН СТЕНДА СООТВЕТСТВИЯ (после класса TestProfile — тип полный)
// ============================================================================
// Вся регистрация завершена к моменту start() TestApp — самое время
// прогнать правила платформы.
// NB: умышленный конфликт из init() (проверка 1, claimGpio(16)) зафиксирован
// реестром => "ноль конфликтов" ожидаемо даст FAIL. Это ДЕМОНСТРАЦИЯ работы
// стенда, а не дефект профиля: боевой профиль обязан проходить чисто.
// ============================================================================
inline void runConformanceForTestProfile() {
    TestProfile self;
    HardwareManifest m;
    self.describeHardware(m);
    conformance::runAll(self.profileId(), m);
}
