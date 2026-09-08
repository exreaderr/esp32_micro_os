// ============================================================================
// SmartCounterApp.h — ПРИКЛАДНОЙ МОДУЛЬ SMART_COUNTER (C1 → панель)
// ============================================================================
// ЕДИНСТВЕННЫЙ источник версии профиля — макрос ниже (5.8.8: двухполевая
// версия OTA; шкала профиля отвязана от ядра, образец smart_lock 0.5.15).
// Определяется ДО включения ядра — Version.h добавит в .bin полную метку
// MICROOS|<ядро>|<профиль>|END рядом с ядерной.
// ============================================================================
#pragma once

#define MICROOS_PROFILE_VER 0.1.2
// 0.1.2: ConfigField.min/max int32_t — дробные границы FLOAT-полей
//        заменены целыми (сборка Build Master, [-Wnarrowing]);
//        sc.gas.imp_per_m3 перевернут на размерность «импульсов на м3»
//        (решение владельца: симметрия с водой, точность по шильдику),
//        умолчание 100 имп/м3, gasPulsesToM3 с защитой делителя.
// 0.1.1: ядро 5.8.8 (двухполевая версия OTA, setProfileVersion), панель
//        /web/sc.html (наследие дизайна монолита v4.4.2), /api/dev/state,
//        dlog-эндпоинты, АКБ/AC-события, биллинг с last_ymd, протечка,
//        фильтр Big Blue, AC-loss снимок (ярус 3 концепт-ноты §5);
// 0.1.0: C1 — скелет + импульсный ввод (PCNT через CounterService),
//        схема sc.* (33 поля), host-тесты 92 PASS.

#include <core/Version.h>   // сразу после макроса: метка MICROOS|ядро|профиль|END
#include <core/ModuleBase.h>
#include <services/IUiProvider.h>
#include "ScPulseCore.h"
#include "ScBillingCore.h"

// ============================================================================
// UI-ПРОВАЙДЕР ПРОФИЛЯ
// ============================================================================
class SmartCounterUi : public IUiProvider {
public:
    static SmartCounterUi& getInstance() {
        static SmartCounterUi instance;
        return instance;
    }

    const char* uiTitle() const override { return "smart_counter"; }

    /// Публичная карточка на "/" (бюджет ~2 КБ): показания + ссылка на панель.
    size_t renderPublicHtml(char* buf, size_t bufSize) override;

    /// Профильный API: counters (публичный зонд), state (панель),
    /// dlog/channels + dlog (история — только админ, паттерн smart_lock).
    bool handleApi(const char* pathTail, const ShUiRequest& req,
                   char* responseBuf, size_t bufSize,
                   int& statusCode) override;

private:
    SmartCounterUi() = default;
};

// ============================================================================
// МОДУЛЬ
// ============================================================================
class SmartCounterApp : public ModuleBase {
public:
    static SmartCounterApp& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "SmartCounterApp"; }
    const char* getVersion() const override { return MICROOS_STR(MICROOS_PROFILE_VER); }
    ModuleId getModuleId() const override { return 0x1000; }     // приложения

    void registerExtensions() override;   // конфиг sc.* (33 поля), UI, hg-группы
    void init() override;                 // PCNT-каналы, версия профиля, кросс-чек
    void start() override;                // DataLog-каналы, conformance
    void stop() override;
    void tick() override;                 // 500 мс: АКБ/AC, протечка, биллинг
    uint32_t getTickIntervalMs() const override { return 500; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- ПОКАЗАНИЯ (импульсы из CounterService + база из конфига) ----------
    // Учёт в ИМПУЛЬСАХ (ScPulseCore.h, урок 1); м³ — только для показа.
    uint32_t waterPulses() const;
    uint32_t gasPulses() const;
    float    waterTotalM3() const;
    float    gasTotalM3() const;

    // --- СНИМОК СОСТОЯНИЯ (для /api/dev/state и панели) ---------------------
    float    batteryV() const { return _battV; }
    bool     acPresent() const { return _acPresent; }
    bool     leakActive() const { return _leak.leakActive; }
    bool     filterWarning() const { return _filterWarn; }
    float    waterFlowLh() const { return _waterFlowLh; }

private:
    SmartCounterApp() = default;

    void readBattery();                   // ADC + EMA (sc.bat.ema_alpha)
    void checkAc();                       // фронт сети: события + снимок
    void saveSnapshot();                  // ярус 3: atomicWrite показаний
    void loadSnapshotCrosscheck();        // бут: max(NVS, снимок) — без отката
    void checkBillingReport();            // отчётный день + catch-up
    void checkFilter();                   // ресурс Big Blue + событие
    void tickLeak();                      // почасовой детектор протечки

    uint32_t _lastSummaryMs = 0;
    uint32_t _lastBattMs    = 0;
    uint32_t _lastBillMs    = 0;
    uint32_t _hourStartMs   = 0;
    uint32_t _hourStartPulses = 0;

    float    _battV      = 0.0f;
    bool     _acPresent  = true;
    bool     _filterWarn = false;
    float    _waterFlowLh = 0.0f;
    scp::LeakDetector _leak;

    int8_t   _chWaterFlow = -1;           // DataLog: расход воды, л/ч
    int8_t   _chBatt      = -1;           // DataLog: АКБ, В
};
