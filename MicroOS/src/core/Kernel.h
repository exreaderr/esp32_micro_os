// ============================================================================
// Kernel.h — ЯДРО МикроОС 5.0 (оркестратор жизненного цикла)
// ============================================================================
// Фаза 0. Сокращённый наследник AppCore v4.2.2: только то, что делает
// систему системой — порядок загрузки, тикание модулей, bootloop-защита,
// Safe Mode. Никакой прикладной логики.
//
// ПОСЛЕДОВАТЕЛЬНОСТЬ ЗАГРУЗКИ (Kernel::run<Profile>):
//
//   1. boot-диагностика: reset reason, bootloop-счётчик (RTC RAM)
//   2. детект Safe Mode (кнопка safe_mode_pin / bootloop / паника)
//   3. ResourceManager + ресурсы BaseProfile (WT32-ETH01 + DS3231)
//   4. profile.describeHardware() -> валидация пинов через ResourceManager
//   5. EventBus.begin()
//   6. profile.registerDrivers(), profile.registerModules()
//   7. фаза describe() всех модулей
//   8. фаза registerExtensions() всех модулей   <- ВСЕ инжекции ДО init
//   9. фаза init() по приоритетам
//  10. фаза start() по приоритетам  -> SH_EVENT_READY
//
// В Safe Mode шаги 4–8 для профиля ПРОПУСКАЮТСЯ: поднимаются только
// системные сервисы (Phase 2 подключит recovery-UI).
//
// MAIN LOOP (Kernel::loop из Arduino loop):
//   · тикает модули по их getTickIntervalMs() с замером бюджета (B2);
//   · раз в 60 с стабильной работы сбрасывает bootloop-счётчик;
//   · сторожит WDT (Phase 1: esp_task_wdt).
// ============================================================================
#pragma once

#include <Arduino.h>
#include "ShTypes.h"
#include "IModule.h"
#include "EventBus.h"
#include "ResourceManager.h"
#include "IDeviceProfile.h"

// ============================================================================
// БЮДЖЕТЫ ЯДРА
// ============================================================================
constexpr uint8_t  KERNEL_MAX_MODULES    = 24;   // модулей в системе
constexpr uint32_t KERNEL_TICK_BUDGET_MS = 50;   // бюджет tick() модуля (B2)
constexpr uint32_t KERNEL_EVENT_BUDGET_MS= 50;   // бюджет onEvent() модуля (B2)
constexpr uint32_t KERNEL_STABLE_MS      = 60000;// "стабильная работа" для
                                                 // сброса bootloop-счётчика
constexpr uint8_t  KERNEL_BOOTLOOP_LIMIT = 3;    // порог входа в Safe Mode
constexpr uint32_t KERNEL_SAFE_BTN_MS    = 3000; // удержание кнопки Safe Mode

class Kernel {
public:
    static Kernel& getInstance();

    // --- ПРИЧИНЫ ВХОДА В SAFE MODE ---------------------------------------
    enum class SafeModeReason : uint8_t {
        NONE     = 0,   // штатный режим (не Safe Mode)
        BUTTON   = 1,   // удержание кнопки safe_mode_pin при старте
        BOOTLOOP = 2,   // счётчик циклических перезагрузок >= порога
        PANIC    = 3,   // паника/WDT на предыдущем старте + сбой init
        COMMAND  = 4    // команда из serial/recovery-эндпоинта
    };

    // --- ЗАПУСК ------------------------------------------------------------
    /// Точка входа устройства: Kernel::run<SmartLockProfile>() из setup().
    /// Шаблон — чтобы main.cpp не знал класс профиля заранее.
    template <typename ProfileT>
    void run() {
        static ProfileT profile;
        boot(&profile);
    }

    /// Тик из Arduino loop(). Неблокирующий.
    void loop();

    // --- СОСТОЯНИЕ ----------------------------------------------------------
    bool isSafeMode() const { return _safeReason != SafeModeReason::NONE; }
    SafeModeReason safeModeReason() const { return _safeReason; }
    uint8_t bootloopCount() const { return _bootloopCount; }

    /// 5.8.1 (урок №20): след последнего тика для посмертной диагностики.
    /// true — RTC-структура живая; buf получает имя модуля, в тике которого
    /// застрял loopTask (пустая строка = авария случилась между тиками).
    bool lastTickTrace(char* buf, size_t n) const;

    // --- РЕГИСТРАЦИЯ МОДУЛЕЙ -------------------------------------------------
    /// Вызывается из profile.registerModules() и системными сервисами.
    /// priority — порядок init/start (меньше = раньше);
    /// tickMs   — период вызова tick().
    bool registerModule(IModule* module, uint8_t priority, uint32_t tickMs);

    // --- ДОСТУП К РЕЕСТРАМ -----------------------------------------------------
    ResourceManager& resources() { return ResourceManager::getInstance(); }
    EventBus&        bus()       { return EventBus::getInstance(); }

    // --- RTC RAM ХРАНИЛИЩЕ ------------------------------------------------------
    /// Структура в .noinit — сохраняет данные через soft-reset ESP32.
    /// magic защищает от мусора после power-on.
    /// (public: инстанцируется как файловый static в Kernel.cpp)
    struct RtcPersist {
        uint32_t magic;          // 0x5AFE2027 — "живая" ли структура
        uint8_t  bootloopCount;  // подряд идущие нестабильные загрузки
        uint8_t  lastResetReason;// esp_reset_reason_t предыдущего старта
        uint8_t  safeRequested;  // флаг входа в Safe Mode (из команды)
        // 5.8.1, урок №20 (TWDT 17.08): сторож печатает бэктрейс ТЕКУЩЕЙ
        // задачи (обычно IDLE), а не зависшей. Ведём свой след: имя модуля
        // пишется СЮДА перед его tick() и стирается в конце прохода loop.
        // Пустая строка на аварийном буте = "между тиками" (код ядра
        // Arduino/lwIP/колбэки, не тик модуля). Живёт в .noinit — переживает
        // SW-ребут (тот же механизм, что доказал bootloopCount в поле).
        char     lastTick[24];   // модуль, в тике которого застрял loopTask
        uint32_t tickSeq;        // счётчик тиков (свежесть следа)
    };

    // --- ЗАПИСЬ МОДУЛЯ В ТАБЛИЦЕ -------------------------------------------------
    // (public: сортировка по приоритету реализована свободной функцией в .cpp)
    struct ModuleSlot {
        IModule* module;
        uint8_t  priority;
        uint32_t tickMs;
        uint32_t lastTickMs;     // millis() последнего вызова tick
    };

    // --- ДОСТУП К RTC RAM (read-only для системных сервисов) -------------------
    /// Указатель на RTC RAM структуру (размещается в .noinit в .cpp).
    /// public: TelemetryService (B1) читает bootloopCount для снимка;
    /// UpdateService проверяет lastResetReason. Запись — только ядру.
    static RtcPersist& rtc();

    // --- ДИАГНОСТИКА (D1: conformance-тест, отчёты о составе) -----------------
    uint8_t moduleCount() const { return _moduleCount; }
    const ModuleSlot* moduleAt(uint8_t i) const {
        return i < _moduleCount ? &_slots[i] : nullptr;
    }

private:
    Kernel() = default;

    // --- ЭТАПЫ ЗАГРУЗКИ --------------------------------------------------------
    void boot(IDeviceProfile* profile);

    /// Шаг 1: reset reason + bootloop-счётчик из RTC RAM (переживает
    /// soft-reset, но не power-on — именно это и нужно).
    void captureBootDiagnostics();

    /// Шаг 2: определить, входим ли в Safe Mode, и по какой причине.
    SafeModeReason detectSafeMode(int8_t safeModePin);

    /// Шаги 7–10: прогнать фазу жизненного цикла по всем модулям
    /// в порядке приоритета. Возвращает число успешно обработанных.
    uint8_t runPhaseDescribe();
    uint8_t runPhaseRegisterExtensions();
    uint8_t runPhaseInit();
    uint8_t runPhaseStart();

    // --- ДАННЫЕ ------------------------------------------------------------------
    ModuleSlot _slots[KERNEL_MAX_MODULES];
    uint8_t    _moduleCount = 0;

    SafeModeReason _safeReason = SafeModeReason::NONE;
    uint8_t  _bootloopCount = 0;
    uint32_t _stableSinceMs = 0;      // когда началась стабильная работа
    bool     _ready = false;          // runPhaseStart завершён
};
