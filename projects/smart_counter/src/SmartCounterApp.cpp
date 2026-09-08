// ============================================================================
// SmartCounterApp.cpp — реализация прикладного модуля smart_counter
// ============================================================================
// Ядро 5.8.8. Конфиг sc.* (33 поля, порядок ЗАМОРОЖЕН — правило 5),
// PCNT через ядерный CounterService, биллинг/протечка/фильтр/АКБ —
// чистая логика Sc*Core.h (host-тесты 92 PASS), AC-loss снимок через
// StorageService.atomicWrite (ярус 3 персистентности, концепт-нота §5).
// ============================================================================
#include "SmartCounterApp.h"
#include "SmartCounterEvents.h"
#include "SmartCounterProfile.h"
#include <core/Kernel.h>
#include <core/ResourceManager.h>
#include <core/ConformanceTest.h>
#include <services/ConfigService.h>
#include <services/CounterService.h>
#include <services/DataLogService.h>
#include <services/HttpService.h>
#include <services/StorageService.h>
#include <services/TimeService.h>
#include <services/UpdateService.h>

// Имена автономных счётчиков CounterService (NVS namespace "cnt",
// лимит ключа 15 символов — CNT_NAME_LEN). Импульсы, НЕ м³ (урок 1
// ScPulseCore.h): показания в м³ = база конфига + импульсы/коэффициент.
static const char* CNT_WATER = "sc.water.imp";   // 12 символов
static const char* CNT_GAS   = "sc.gas.imp";     // 10 символов

// AC-loss снимок (ярус 3): АКБ даёт минуты — успеваем записать всё.
// Буфер маленький (правило 12: >4 КБ — не на стек; нам хватает 128).
static const char* SNAP_PATH = "/sc_snap.json";

SmartCounterApp& SmartCounterApp::getInstance() {
    static SmartCounterApp instance;
    return instance;
}

// ============================================================================
// РАСШИРЕНИЯ ЯДРА: схема конфига sc.* + UI
// ============================================================================
// ПОРЯДОК ПОЛЕЙ ЗАМОРОЖЕН с первого коммита C1 (правило 5: новые поля
// СТРОГО В КОНЕЦ). Состав — концепт-нота C0 (рецензия ядра 01.09.2026):
// 30 полей + поправки рецензии §3.1/§3.2 (позиции 31–32) + поле 33
// sc.bill.last_ymd (закрытие дефекта монолита «lastResetDay в RAM»).
void SmartCounterApp::registerExtensions() {
    ConfigService& cfg = ConfigService::getInstance();

    // Правило 23 (hg-флаг, прецедент weather_gate 0.5.1): ядерные группы,
    // не влияющие на профиль, в профильных панелях не показываем. Группу
    // «Счётчики» НЕ скрываем — CounterService у нас рабочий (cnt.flush_*).
    cfg.setHiddenGroups("Планировщик,Звук");

    bool ok = cfg.addFields("Счётчик электро (RS-485)", {
        { "sc.meter.enabled",  ConfigType::BOOL,   "true", 0, 0, CFG_NONE,
          "Счётчик электро (RS-485)", "Опрос Меркурия-206 разрешён" },
        { "sc.meter.poll_s",   ConfigType::UINT,   "5", 1, 60, CFG_NONE,
          "Счётчик электро (RS-485)", "Период U/I/P, с" },
        { "sc.meter.energy_s", ConfigType::UINT,   "30", 5, 300, CFG_NONE,
          "Счётчик электро (RS-485)", "Период энергии, с" },
        { "sc.meter.sn",       ConfigType::STRING, "", 0, 0, CFG_READONLY,
          "Счётчик электро (RS-485)", "Серийный номер (автопоиск)" },
        { "sc.meter.addr_hex", ConfigType::STRING, "00000000", 0, 0, CFG_CRITICAL,
          "Счётчик электро (RS-485)", "BCD-адрес на шине (hex)" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'Счётчик электро' failed");

    ok = cfg.addFields("Вода (ХВС)", {
        { "sc.water.enabled",     ConfigType::BOOL,  "true", 0, 0, CFG_CRITICAL,
          "Вода (ХВС)", "Канал ХВС разрешён" },
        { "sc.water.imp_per_l",   ConfigType::FLOAT, "1", 1, 100, CFG_NONE,
          "Вода (ХВС)", "Импульсов на литр" },
        { "sc.water.debounce_ms", ConfigType::UINT,  "200", 10, 2000, CFG_NONE,
          "Вода (ХВС)", "Антидребезг, мс" },
        { "sc.water.total_m3",    ConfigType::FLOAT, "0", 0, 0, CFG_READONLY,
          "Вода (ХВС)", "База показаний, м3" },
        { "sc.water.price",       ConfigType::FLOAT, "0", 0, 10000, CFG_NONE,
          "Вода (ХВС)", "Тариф, руб/м3" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'Вода' failed");

    ok = cfg.addFields("Газ", {
        { "sc.gas.enabled",     ConfigType::BOOL,  "false", 0, 0, CFG_CRITICAL,
          "Газ", "Канал газа разрешён" },
        { "sc.gas.imp_per_m3",  ConfigType::FLOAT, "100", 1, 100000, CFG_NONE,
          "Газ", "Импульсов на м3" },
        { "sc.gas.debounce_ms", ConfigType::UINT,  "200", 10, 2000, CFG_NONE,
          "Газ", "Антидребезг, мс" },
        { "sc.gas.total_m3",    ConfigType::FLOAT, "0", 0, 0, CFG_READONLY,
          "Газ", "База показаний, м3" },
        { "sc.gas.price",       ConfigType::FLOAT, "0", 0, 10000, CFG_NONE,
          "Газ", "Тариф, руб/м3" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'Газ' failed");

    ok = cfg.addFields("Электроэнергия", {
        { "sc.energy.total_kwh", ConfigType::FLOAT, "0", 0, 0, CFG_READONLY,
          "Электроэнергия", "Показания на момент ввода, кВт*ч" },
        { "sc.energy.price",     ConfigType::FLOAT, "0", 0, 10000, CFG_NONE,
          "Электроэнергия", "Тариф, руб/кВт*ч" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'Электроэнергия' failed");

    ok = cfg.addFields("Биллинг", {
        { "sc.bill.report_day",  ConfigType::UINT,  "24", 1, 28, CFG_NONE,
          "Биллинг", "Отчётный день месяца" },
        { "sc.bill.water_base",  ConfigType::FLOAT, "0", 0, 0, CFG_READONLY,
          "Биллинг", "База воды на отчётную дату, м3" },
        { "sc.bill.gas_base",    ConfigType::FLOAT, "0", 0, 0, CFG_READONLY,
          "Биллинг", "База газа на отчётную дату, м3" },
        { "sc.bill.energy_base", ConfigType::FLOAT, "0", 0, 0, CFG_READONLY,
          "Биллинг", "База электро на отчётную дату, кВт*ч" },
        { "sc.bill.last_ymd",    ConfigType::UINT,  "0", 0, 0, CFG_READONLY,
          "Биллинг", "Дата последнего сброса YYYYMMDD (0=никогда)" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'Биллинг' failed");

    ok = cfg.addFields("Протечка", {
        { "sc.leak.enabled",       ConfigType::BOOL,  "true", 0, 0, CFG_NONE,
          "Протечка", "Детектор «нет сухого часа» разрешён" },
        { "sc.leak.threshold_l_h", ConfigType::FLOAT, "200", 0, 10000, CFG_NONE,
          "Протечка", "Средний расход мокрой серии, л/ч (отсечка)" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'Протечка' failed");

    ok = cfg.addFields("Фильтр Big Blue", {
        { "sc.filter.limit_l", ConfigType::UINT,  "50000", 1000, 500000, CFG_NONE,
          "Фильтр Big Blue", "Ресурс картриджа, л" },
        { "sc.filter.base_m3", ConfigType::FLOAT, "0", 0, 0, CFG_READONLY,
          "Фильтр Big Blue", "Якорь замены, м3" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'Фильтр' failed");

    ok = cfg.addFields("АКБ ИБП", {
        { "sc.bat.coeff",     ConfigType::FLOAT, "2.1", 1, 10, CFG_NONE,
          "АКБ ИБП", "Коэффициент делителя АЦП" },
        { "sc.bat.low_v",     ConfigType::FLOAT, "3.4", 2, 4, CFG_NONE,
          "АКБ ИБП", "Порог разряда, В" },
        // Поле 32 — поправка рецензии ядра §3.2 (концепт-нота C0):
        { "sc.bat.ema_alpha", ConfigType::FLOAT, "0.15", 0, 1, CFG_NONE,
          "АКБ ИБП", "Alpha EMA-фильтра напряжения" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'АКБ' failed");

    ok = cfg.addFields("Температуры ZONT", {
        { "sc.temp.hot_topic",  ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Температуры ZONT", "MQTT-топик ГВС" },
        { "sc.temp.cold_topic", ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Температуры ZONT", "MQTT-топик ХВС" },
        { "sc.temp.stale_s",    ConfigType::UINT,   "300", 30, 3600, CFG_NONE,
          "Температуры ZONT", "Протухание данных, с" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'ZONT' failed");

    // Поле 31 — поправка рецензии ядра §3.1 (шумовой вентиль ПАЗ).
    // Отдельной группой: поправки §3.1/§3.2 внесены ПОСЛЕ утверждения
    // 30 полей — занимают позиции 31–32 строго в порядке рецензии.
    ok = cfg.addFields("Импульсные входы", {
        { "sc.pulse.max_hz", ConfigType::UINT, "50", 1, 1000, CFG_NONE,
          "Импульсные входы", "Потолок частоты (выше = наводка, не расход)" },
    });
    if (!ok) log(LogLevel::Error, "addFields 'Импульсные входы' failed");

    // --- UI профиля ------------------------------------------------------
    HttpService::getInstance().setUiProvider(&SmartCounterUi::getInstance());
}

// ============================================================================
// INIT: PCNT-каналы, версия профиля (5.8.8), кросс-чек со снимком
// ============================================================================
void SmartCounterApp::init() {
    // 5.8.8: двухполевая версия OTA — профиль сообщает свою шкалу.
    UpdateService::getInstance().setProfileVersion(getVersion());
#ifdef MICROOS_PROFILE_VER
    // used не спасает от --gc-sections: живая ссылка нужна на внешний
    // вызов (printf), чтобы адрес метки «утёк» (урок 5.8.8, smart_lock).
    log(LogLevel::Info, "bin tag: %s", MICROOS_BIN_TAG_FULL);
#endif

    // PCNT-бэкенд: импульсы считает АППАРАТНЫЙ блок ESP32 (без CPU и
    // прерываний), дельта wrap-safe снимается сервисом раз в секунду,
    // сброс в NVS — батчами (cnt.flush_every / cnt.flush_interval_s) +
    // на SH_EVENT_SHUTDOWN. Глитч-фильтр 10 мкс — аппаратный; программное
    // подавление дребезга геркона (200 мс) — ScPulseCore, включается при
    // подтверждении на стенде C1 (CounterService.h: «для геркона
    // дополнительно программное подавление в профиле»).
    CounterService& cnt = CounterService::getInstance();
    const SmartCounterPins& p = SmartCounterProfile::pins();

    if (cfgGetBool("sc.water.enabled", true)) {
        bool okw = cnt.attachPcnt(CNT_WATER, (int8_t)p.waterPulse, 10000);
        log(okw ? LogLevel::Info : LogLevel::Error,
            "PCNT water GPIO%u: %s", p.waterPulse, okw ? "attached" : "FAILED");
    }
    if (cfgGetBool("sc.gas.enabled", false)) {
        bool okg = cnt.attachPcnt(CNT_GAS, (int8_t)p.gasPulse, 10000);
        log(okg ? LogLevel::Info : LogLevel::Error,
            "PCNT gas GPIO%u: %s", p.gasPulse, okg ? "attached" : "FAILED");
    }

    // Пины локальной периферии (input-only, ADC — как в монолите, analogRead)
    pinMode(p.acSense, INPUT);      // GPIO39: оптрон сети 220В
    pinMode(p.battAdc, INPUT);      // GPIO36: делитель АКБ (12 бит, 3.3 В —
                                    // дефолт Arduino-ядра, как монолит)

    _leak.init();
    _hourStartMs = millis();
    _hourStartPulses = waterPulses();

    // Ярус 3→1 кросс-чек: снимок AC-loss против NVS CounterService.
    // Показания монотонны — отката быть не должно: берём max().
    loadSnapshotCrosscheck();

    _initialized = true;
}

// ============================================================================
// START: DataLog-каналы + conformance (боевой профиль проходит ЧИСТО)
// ============================================================================
void SmartCounterApp::start() {
    _started = true;

    // DataLog: RAW 6 ч / HOUR 31 сутки / DAY год — своё кольцо писать
    // ЗАПРЕЩЕНО (брифинг §3). 8 каналов максимум — планируем.
    DataLogService& dl = DataLogService::getInstance();
    _chWaterFlow = dl.registerChannel("sc_wf", "Расход ХВС", "л/ч");
    _chBatt      = dl.registerChannel("sc_bat", "АКБ ИБП", "В");

    SmartCounterProfile self;
    HardwareManifest m;
    self.describeHardware(m);
    conformance::runAll(self.profileId(), m);
}

void SmartCounterApp::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    _started = false;
}

// ============================================================================
// TICK (500 мс): АКБ, сеть 220В, протечка (почасовая), биллинг, сводка
// ============================================================================
void SmartCounterApp::tick() {
    uint32_t now = millis();

    checkAc();                                   // фронт сети — без задержки

    if (now - _lastBattMs >= 1000) {             // АКБ: 1 Гц + EMA
        _lastBattMs = now;
        readBattery();
    }

    tickLeak();                                  // закрытие часа по расписанию

    if (now - _lastBillMs >= 60000) {            // биллинг: раз в минуту
        _lastBillMs = now;
        checkBillingReport();
        checkFilter();
    }

    if (now - _lastSummaryMs >= 10000) {         // сводка + DataLog: 10 с
        _lastSummaryMs = now;
        CounterService& cnt = CounterService::getInstance();
        log(LogLevel::Info,
            "CNT: water=%lu imp (%.3f m3) gas=%lu imp (%.3f m3) "
            "bat=%.2fV ac=%d flush=%lu drop=%lu",
            (unsigned long)cnt.value(CNT_WATER), waterTotalM3(),
            (unsigned long)cnt.value(CNT_GAS),   gasTotalM3(),
            _battV, (int)_acPresent,
            (unsigned long)cnt.flushCount(),
            (unsigned long)cnt.droppedIncrements());
        DataLogService& dl = DataLogService::getInstance();
        if (_chWaterFlow >= 0) dl.logPoint(_chWaterFlow, _waterFlowLh);
        if (_chBatt >= 0)      dl.logPoint(_chBatt, _battV);
    }
}

// ============================================================================
// АКБ ИБП: ADC + EMA (первый отсчёт без сглаживания — ПАЗ не слепнет)
// ============================================================================
void SmartCounterApp::readBattery() {
    int raw = analogRead(SmartCounterProfile::pins().battAdc);   // GPIO36
    float sample = (raw * (3.3f / 4095.0f)) * cfgGetFloat("sc.bat.coeff", 2.1f);
    _battV = scp::emaUpdate(_battV, sample, cfgGetFloat("sc.bat.ema_alpha", 0.15f));
}

// ============================================================================
// СЕТЬ 220В: фронт → события; при пропадании — снимок (АКБ даёт минуты)
// ============================================================================
void SmartCounterApp::checkAc() {
    const SmartCounterPins& p = SmartCounterProfile::pins();
    bool ac = (digitalRead(p.acSense) != 0);
    if (ac == _acPresent) return;
    _acPresent = ac;
    if (!ac) {
        log(LogLevel::Warning, "AC LOST — снимок показаний (ярус 3)");
        saveSnapshot();
        EventBus::getInstance().post(sc_ev::acLoss());
    } else {
        log(LogLevel::Info, "AC restored");
        EventBus::getInstance().post(sc_ev::acRestored());
    }
}

// ============================================================================
// СНИМОК ПОКАЗАНИЙ (ярус 3 персистентности, концепт-нота §5)
// ============================================================================
void SmartCounterApp::saveSnapshot() {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "{\"w\":%lu,\"g\":%lu,\"t\":%lu}",
        (unsigned long)waterPulses(), (unsigned long)gasPulses(),
        (unsigned long)TimeService::getInstance().getUnixTime());
    if (n <= 0) return;
    bool ok = StorageService::getInstance().atomicWrite(SNAP_PATH, buf);
    log(ok ? LogLevel::Info : LogLevel::Error, "snapshot %s: %s",
        ok ? "saved" : "FAILED", buf);
}

/// Бут: если снимок НОВЕЕ NVS-тени CounterService (питание умерло до
/// батч-сброса), догоняем счётчик дельтой. Показания не откатываются.
void SmartCounterApp::loadSnapshotCrosscheck() {
    uint8_t buf[128];
    size_t n = StorageService::getInstance().readFile(SNAP_PATH, buf, sizeof(buf) - 1);
    if (n == 0) return;                          // снимка нет — первый запуск
    buf[n] = '\0';
    // Минимальный разбор без JSON-библиотеки: формат наш, фиксированный.
    uint32_t w = 0, g = 0;
    if (sscanf((const char*)buf, "{\"w\":%lu,\"g\":%lu", &w, &g) != 2) {
        log(LogLevel::Warning, "snapshot corrupt, ignored: %s", buf);
        return;
    }
    CounterService& cnt = CounterService::getInstance();
    uint32_t cw = cnt.value(CNT_WATER);
    uint32_t cg = cnt.value(CNT_GAS);
    if (w > cw) { cnt.increment(CNT_WATER, w - cw);
                  log(LogLevel::Warning, "snapshot catch-up water: +%lu imp", (unsigned long)(w - cw)); }
    if (g > cg) { cnt.increment(CNT_GAS, g - cg);
                  log(LogLevel::Warning, "snapshot catch-up gas: +%lu imp", (unsigned long)(g - cg)); }
}

// ============================================================================
// БИЛЛИНГ: отчётный день + догон (ScBillingCore; last_ymd в NVS — урок
// монолита «lastResetDay в RAM» здесь не повторяется)
// ============================================================================
void SmartCounterApp::checkBillingReport() {
    TimeService& ts = TimeService::getInstance();
    time_t unix = ts.getUnixTime();
    if (unix < 1700000000) return;               // время не валидно — не сбрасываем

    // Детерминированный пояс (правило 1): gmtime_r(unix + tz*3600)
    time_t local = unix + (time_t)cfgGetInt("sys.tz_offset", 3) * 3600;
    struct tm t;
    gmtime_r(&local, &t);

    int reportDay = (int)cfgGetUInt("sc.bill.report_day", 24);
    uint32_t lastYmd = cfgGetUInt("sc.bill.last_ymd", 0);
    if (!scb::isReportDue(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                          reportDay, lastYmd)) return;

    // Фиксация баз: штатно — текущие показания; при catch-up — не новее
    // текущих (снимок как «база на момент пропадания» — задел C2, когда
    // появится дата снимка; пока база = текущие, см. ScBillingCore.h).
    char v[24];
    ConfigService& cfg = ConfigService::getInstance();
    snprintf(v, sizeof(v), "%.3f", waterTotalM3());
    cfg.setInternal("sc.bill.water_base", v);
    snprintf(v, sizeof(v), "%.3f", gasTotalM3());
    cfg.setInternal("sc.bill.gas_base", v);
    snprintf(v, sizeof(v), "%.2f", cfgGetFloat("sc.energy.total_kwh", 0.0f));
    cfg.setInternal("sc.bill.energy_base", v);
    snprintf(v, sizeof(v), "%lu",
             (unsigned long)scb::ymd(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday));
    cfg.setInternal("sc.bill.last_ymd", v);

    log(LogLevel::Info, "BILL: база отчётного периода зафиксирована (%s)", v);
    EventBus::getInstance().post(sc_ev::billReset());
}

// ============================================================================
// ФИЛЬТР BIG BLUE: переходы тревоги -> событие (ScBillingCore)
// ============================================================================
void SmartCounterApp::checkFilter() {
    float remaining = scb::filterRemainingL(waterTotalM3(),
        cfgGetFloat("sc.filter.base_m3", 0.0f),
        (float)cfgGetUInt("sc.filter.limit_l", 50000));
    int ev = scb::filterWarningStep(_filterWarn, remaining);
    if (ev == 1) {
        _filterWarn = true;
        log(LogLevel::Warning, "FILTER: ресурс Big Blue исчерпан — заменить!");
        EventBus::getInstance().post(sc_ev::filterReplace());
    } else if (ev == 2) {
        _filterWarn = false;
    }
}

// ============================================================================
// ПРОТЕЧКА: почасовой детектор «нет сухого часа» (ScPulseCore)
// ============================================================================
void SmartCounterApp::tickLeak() {
    if (!cfgGetBool("sc.leak.enabled", true)) return;
    uint32_t now = millis();

    // Мгновенный расход окном 10 с — для панели и DataLog
    static uint32_t s_flowMs = 0, s_flowPulses = 0;
    if (now - s_flowMs >= 10000) {
        float impPerL = cfgGetFloat("sc.water.imp_per_l", 1.0f);
        _waterFlowLh = scp::flowLitresPerHour(waterPulses() - s_flowPulses,
                                              (now - s_flowMs) / 1000, impPerL);
        s_flowMs = now; s_flowPulses = waterPulses();
    }

    if (now - _hourStartMs < 3600000UL) return;
    // Закрываем час: расход за час в литрах
    float impPerL = cfgGetFloat("sc.water.imp_per_l", 1.0f);
    uint32_t pulses = waterPulses();
    float litres = (impPerL > 0.0f)
        ? (float)(pulses - _hourStartPulses) / impPerL : 0.0f;
    _hourStartPulses = pulses;
    _hourStartMs = now;

    _leak.addLitres(litres);
    int ev = _leak.closeHour(cfgGetFloat("sc.leak.threshold_l_h", 200.0f));
    if (ev == 1) {
        log(LogLevel::Error, "LEAK: нет сухого часа — протечка!");
        EventBus::getInstance().post(sc_ev::leakDetected());
    } else if (ev == 2) {
        log(LogLevel::Info, "LEAK: сухой час — тревога снята");
        EventBus::getInstance().post(sc_ev::leakCleared());
    }
}

// ============================================================================
// ПОКАЗАНИЯ
// ============================================================================
uint32_t SmartCounterApp::waterPulses() const {
    return CounterService::getInstance().value(CNT_WATER);
}
uint32_t SmartCounterApp::gasPulses() const {
    return CounterService::getInstance().value(CNT_GAS);
}
float SmartCounterApp::waterTotalM3() const {
    return cfgGetFloat("sc.water.total_m3", 0.0f)
         + scp::pulsesToM3(waterPulses(), cfgGetFloat("sc.water.imp_per_l", 1.0f));
}
float SmartCounterApp::gasTotalM3() const {
    return cfgGetFloat("sc.gas.total_m3", 0.0f)
         + scp::gasPulsesToM3(gasPulses(), cfgGetFloat("sc.gas.imp_per_m3", 100.0f));
}

// ============================================================================
// СОБЫТИЯ (подписки — C3/C4: MQTT, HA discovery, ПАЗ-реакции)
// ============================================================================
bool SmartCounterApp::canHandleEvent(int32_t id) const {
    (void)id;
    return false;
}
void SmartCounterApp::onEvent(int32_t eventId, const ShEventData* data) {
    (void)eventId; (void)data;
}

// ============================================================================
// UI-ПРОВАЙДЕР
// ============================================================================
size_t SmartCounterUi::renderPublicHtml(char* buf, size_t bufSize) {
    SmartCounterApp& app = SmartCounterApp::getInstance();
    int n = snprintf(buf, bufSize,
        "<b>Умный счётчик</b>"
        "<p>ХВС: %.3f м&sup3; &middot; Газ: %.3f м&sup3;</p>"
        "<p><a href=\"/web/sc.html\">Панель счётчика</a></p>",
        app.waterTotalM3(), app.gasTotalM3());
    return n > 0 ? (size_t)n : 0;
}

bool SmartCounterUi::handleApi(const char* pathTail, const ShUiRequest& req,
                               char* responseBuf, size_t bufSize,
                               int& statusCode) {
    // --- Публичные зонды --------------------------------------------------
    if (strcmp(pathTail, "counters") == 0) {
        // Readback-зонд: сырые импульсы + пересчёт + здоровье CounterService.
        SmartCounterApp& app = SmartCounterApp::getInstance();
        CounterService& cnt = CounterService::getInstance();
        int n = snprintf(responseBuf, bufSize,
            "{\"water_imp\":%lu,\"gas_imp\":%lu,"
            "\"water_m3\":%.3f,\"gas_m3\":%.3f,"
            "\"flush\":%lu,\"dropped\":%lu,\"pending\":%lu}",
            (unsigned long)app.waterPulses(),
            (unsigned long)app.gasPulses(),
            app.waterTotalM3(), app.gasTotalM3(),
            (unsigned long)cnt.flushCount(),
            (unsigned long)cnt.droppedIncrements(),
            (unsigned long)cnt.pendingTotal());
        (void)n;
        statusCode = 200;
        return true;
    }

    if (strcmp(pathTail, "state") == 0) {
        // Полный снимок для панели /web/sc.html. Показания — не секрет
        // (монолит: публичный дашборд); операции и настройки — ядерный
        // /admin с авторизацией.
        SmartCounterApp& app = SmartCounterApp::getInstance();
        ConfigService& cfg = ConfigService::getInstance();

        scb::Bill wb = scb::computeBill(app.waterTotalM3(),
            cfgGetFloat("sc.bill.water_base", 0.0f), cfgGetFloat("sc.water.price", 0.0f));
        scb::Bill gb = scb::computeBill(app.gasTotalM3(),
            cfgGetFloat("sc.bill.gas_base", 0.0f), cfgGetFloat("sc.gas.price", 0.0f));
        float eTotal = cfgGetFloat("sc.energy.total_kwh", 0.0f);
        scb::Bill eb = scb::computeBill(eTotal,
            cfgGetFloat("sc.bill.energy_base", 0.0f), cfgGetFloat("sc.energy.price", 0.0f));

        float fUsed = scb::filterUsageL(app.waterTotalM3(),
            cfgGetFloat("sc.filter.base_m3", 0.0f));
        float fLimit = (float)cfgGetUInt("sc.filter.limit_l", 50000);

        int batState = scp::batteryState(app.batteryV(), app.acPresent(),
            cfgGetFloat("sc.bat.low_v", 3.4f));

        int n = snprintf(responseBuf, bufSize,
            "{\"water\":{\"total\":%.3f,\"monthly\":%.3f,\"bill\":%.2f,"
             "\"flow_lh\":%.1f,\"price\":%.2f},"
            "\"gas\":{\"enabled\":%d,\"total\":%.3f,\"monthly\":%.3f,"
             "\"bill\":%.2f,\"price\":%.2f},"
            "\"energy\":{\"total\":%.2f,\"monthly\":%.2f,\"bill\":%.2f,"
             "\"price\":%.2f,\"meter_on\":%d},"
            "\"filter\":{\"used_l\":%.0f,\"limit_l\":%.0f,\"warning\":%d},"
            "\"pwr\":{\"ac\":%d,\"batt_v\":%.2f,\"batt_state\":%d},"
            "\"leak\":{\"active\":%d,\"threshold\":%.0f},"
            "\"bill\":{\"report_day\":%lu,\"last_ymd\":%lu},"
            "\"ver\":\"%s\"}",
            app.waterTotalM3(), wb.monthly, wb.amount,
            app.waterFlowLh(), cfgGetFloat("sc.water.price", 0.0f),
            cfgGetBool("sc.gas.enabled", false) ? 1 : 0,
            app.gasTotalM3(), gb.monthly, gb.amount, cfgGetFloat("sc.gas.price", 0.0f),
            eTotal, eb.monthly, eb.amount, cfgGetFloat("sc.energy.price", 0.0f),
            cfgGetBool("sc.meter.enabled", true) ? 1 : 0,
            fUsed, fLimit, app.filterWarning() ? 1 : 0,
            app.acPresent() ? 1 : 0, app.batteryV(), batState,
            app.leakActive() ? 1 : 0, cfgGetFloat("sc.leak.threshold_l_h", 200.0f),
            (unsigned long)cfgGetUInt("sc.bill.report_day", 24),
            (unsigned long)cfgGetUInt("sc.bill.last_ymd", 0),
            app.getVersion());
        (void)n;
        statusCode = 200;
        (void)cfg;
        return true;
    }

    // --- История (графики) — только админ (паттерн smart_lock/шлюз) -------
    if (strncmp(pathTail, "dlog", 4) == 0) {
        if (!HttpService::getInstance().isAdminToken(req.token)) {
            return false;   // 404 ядра: не раскрываем существование путей
        }
        DataLogService& dl = DataLogService::getInstance();
        if (strcmp(pathTail, "dlog/channels") == 0) {
            size_t pos = 0;
            int n = snprintf(responseBuf, bufSize, "{\"channels\":[");
            if (n > 0) pos = (size_t)n;
            for (uint8_t i = 0; i < dl.channelCount(); ++i) {
                char id[12], name[28], unit[8];
                if (!dl.channelInfo(i, id, sizeof(id), name, sizeof(name),
                                    unit, sizeof(unit))) continue;
                n = snprintf(responseBuf + pos, bufSize - pos,
                    "%s{\"i\":%u,\"id\":\"%s\",\"name\":\"%s\",\"unit\":\"%s\"}",
                    pos > 12 ? "," : "", i, id, name, unit);
                if (n < 0 || (size_t)n >= bufSize - pos) break;
                pos += (size_t)n;
            }
            snprintf(responseBuf + pos, bufSize - pos, "]}");
            statusCode = 200;
            return true;
        }
        if (strcmp(pathTail, "dlog") == 0) {
            const char* chArg = req.getArg("ch");   // getArg транзиентен (правило 17)
            uint8_t ch = chArg ? (uint8_t)atoi(chArg) : 0;
            if (ch >= dl.channelCount()) {
                statusCode = 404;
                snprintf(responseBuf, bufSize, "{\"err\":\"no_channel\"}");
                return true;
            }
            const char* range = req.getArg("range");
            uint32_t nowTs = (uint32_t)TimeService::getInstance().getUnixTime();
            uint32_t fromTs = 0;
            bool raw = true;
            if (range == nullptr || strcmp(range, "6h") == 0) {
                fromTs = nowTs > 6UL * 3600 ? nowTs - 6UL * 3600 : 0;
            } else if (strcmp(range, "24h") == 0) {
                raw = false; fromTs = nowTs > 24UL * 3600 ? nowTs - 24UL * 3600 : 0;
            } else if (strcmp(range, "7d") == 0) {
                raw = false; fromTs = nowTs > 7UL * 86400 ? nowTs - 7UL * 86400 : 0;
            } else if (strcmp(range, "30d") == 0) {
                raw = false; fromTs = 0;
            } else {
                statusCode = 400;
                snprintf(responseBuf, bufSize, "{\"err\":\"range: 6h|24h|7d|30d\"}");
                return true;
            }
            // RAW-кольцо в RAM; для ярусов HOUR/DAY — файлы (C3, по мере
            // наполнения панели). Пока отдаём RAW (6 ч) и raw=false как
            // тот же канал (решение по ярусам — со стендом C3).
            static DlogPoint pts[DLOG_RAW_CAP];   // >4 КБ — статик (правило 12)
            uint16_t cnt = dl.getRaw(ch, pts, DLOG_RAW_CAP, fromTs);
            size_t pos = 0;
            int n = snprintf(responseBuf, bufSize, "{\"ch\":%u,\"raw\":%d,\"pts\":[",
                             ch, raw ? 1 : 0);
            if (n > 0) pos = (size_t)n;
            for (uint16_t i = 0; i < cnt; ++i) {
                n = snprintf(responseBuf + pos, bufSize - pos, "%s[%lu,%.2f]",
                             i ? "," : "", (unsigned long)pts[i].ts, pts[i].v);
                if (n < 0 || (size_t)n >= bufSize - pos) { pos = bufSize - 8; break; }
                pos += (size_t)n;
            }
            snprintf(responseBuf + pos, bufSize - pos, "]}");
            statusCode = 200;
            return true;
        }
    }

    return false;   // неизвестный профильный путь -> 404 ядра
}
