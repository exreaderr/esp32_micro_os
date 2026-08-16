// ============================================================================
// Kernel.cpp — реализация ядра
// ============================================================================
#include "Kernel.h"
#include "Events.h"
#include "Version.h"
#include "../platform/BaseProfile.h"
#include "BusManager.h"
#include "DriverRegistry.h"
#include "../drivers/EspTempDriver.h"
#include "../drivers/Ds3231Driver.h"
#include "../services/TimeService.h"
#include "../services/HealthMonitor.h"
#include "../services/UpdateService.h"
#include "../services/StorageService.h"
#include "../services/ConfigService.h"
#include "../services/NetworkManager.h"
#include "../services/AuthService.h"
#include "../services/LogService.h"
#include "../services/AuditService.h"
#include "../services/TelemetryService.h"
#include "../services/DataLogService.h"
#include "../services/MqttTransport.h"
#include "../services/HttpService.h"
#include "../services/AudioService.h"
#include "../services/ScheduleService.h"
#include "../services/CounterService.h"
#include <esp_system.h>

// ============================================================================
// RTC RAM ХРАНИЛИЩЕ (секция .noinit)
// ============================================================================
// Данные в этой секции НЕ инициализируются загрузчиком при soft-reset —
// поэтому bootloop-счётчик переживает перезагрузки, но сбрасывается при
// полном выключении питания (что и требуется: включение "с нуля" — это
// осознанное действие, а не звено цикла ребутов).
// ============================================================================
constexpr uint32_t RTC_MAGIC = 0x5AFE2026;

__attribute__((section(".noinit"))) static Kernel::RtcPersist s_rtc;

Kernel::RtcPersist& Kernel::rtc() { return s_rtc; }

Kernel& Kernel::getInstance() {
    static Kernel instance;
    return instance;
}

// ============================================================================
// ШАГ 1: BOOT-ДИАГНОСТИКА
// ============================================================================
void Kernel::captureBootDiagnostics() {
    esp_reset_reason_t reason = esp_reset_reason();

    // Структура мертва (power-on или впервые прошито) — инициализируем нулями
    if (s_rtc.magic != RTC_MAGIC) {
        s_rtc.magic = RTC_MAGIC;
        s_rtc.bootloopCount = 0;
        s_rtc.lastResetReason = 0;
        s_rtc.safeRequested = 0;
    }

    // Аварийный предыдущий старт (паника, WDT, brownout) = нестабильная
    // загрузка -> наращиваем счётчик. Штатные причины — сбрасываем.
    bool unstable = (reason == ESP_RST_PANIC) ||
                    (reason == ESP_RST_TASK_WDT) ||
                    (reason == ESP_RST_WDT) ||
                    (reason == ESP_RST_BROWNOUT);
    if (unstable) {
        s_rtc.bootloopCount++;
    }
    _bootloopCount = s_rtc.bootloopCount;

    Serial.printf("[KERNEL] Reset reason: %d, bootloop count: %u\n",
                  (int)reason, _bootloopCount);

    s_rtc.lastResetReason = (uint8_t)reason;
}

// ============================================================================
// ШАГ 2: ДЕТЕКТ SAFE MODE
// ============================================================================
Kernel::SafeModeReason Kernel::detectSafeMode(int8_t safeModePin) {
    // --- Триггер 1: bootloop-счётчик достиг порога (A1) -------------------
    if (_bootloopCount >= KERNEL_BOOTLOOP_LIMIT) {
        Serial.println(F("[KERNEL] SAFE MODE: bootloop limit reached"));
        return SafeModeReason::BOOTLOOP;
    }

    // --- Триггер 2: команда из serial/recovery на предыдущем старте --------
    if (s_rtc.safeRequested) {
        s_rtc.safeRequested = 0;   // одноразовый флаг
        Serial.println(F("[KERNEL] SAFE MODE: requested by command"));
        return SafeModeReason::COMMAND;
    }

    // --- Триггер 3: удержание кнопки safe_mode_pin при старте --------------
    // Кнопка объявляется профилем (safe_mode_pin в манифесте). Ядро читает
    // пин ДО инициализации любых модулей — профиль в этом не участвует.
    if (safeModePin >= 0) {
        pinMode((uint8_t)safeModePin, INPUT_PULLUP);
        delay(25);   // стабилизация переходных процессов подтяжки
                     // (из монолита: 25 мс достаточно для внешней подтяжки)
        uint32_t holdMs = 0;
        while (digitalRead((uint8_t)safeModePin) == LOW &&
               holdMs < KERNEL_SAFE_BTN_MS) {
            delay(100);
            holdMs += 100;
        }
        if (holdMs >= KERNEL_SAFE_BTN_MS) {
            Serial.printf("[KERNEL] SAFE MODE: button on GPIO%d held %lu ms\n",
                          safeModePin, (unsigned long)holdMs);
            return SafeModeReason::BUTTON;
        }
    }

    return SafeModeReason::NONE;
}

// ============================================================================
// ЗАГРУЗКА
// ============================================================================
void Kernel::boot(IDeviceProfile* profile) {
    Serial.begin(115200);
    Serial.printf("\n[KERNEL] MicroOS %s boot, profile: %s\n",
                  MICROOS_VERSION, profile->profileId());
    // ODR-use метки версии: гарантия, что строка доживёт до .bin
    // (constexpr + used без использования --gc-sections выбросил, 5.0.12),
    // а заодно тег виден в мониторе порта — как Build Master его и ищет.
    Serial.printf("[KERNEL] bin tag: %s\n", MICROOS_BIN_TAG);

    // Шаг 1: диагностика предыдущего старта
    captureBootDiagnostics();

    // Шаг 2: Safe Mode (кнопку объявит манифест; до него проверяем
    // bootloop/команду — для этого сначала запросим манифест "на сухую")
    HardwareManifest manifest;
    profile->describeHardware(manifest);   // манифест нужен и для safe_mode_pin
    // Выбор платы платформы (5.3.0): ДО claim'а ресурсов, BusManager и
    // NetworkService — все они читают пины из platform::board().
    platform::selectBoard(manifest.boardId);
    Serial.printf("[KERNEL] board: %s\n", platform::board().name);
    _safeReason = detectSafeMode(manifest.safeModePin);

    if (isSafeMode()) {
        // Событие ещё не может быть опубликовано (шина не поднята) —
        // опубликуем после EventBus.begin().
        Serial.printf("[KERNEL] *** SAFE MODE, reason=%d ***\n",
                      (int)_safeReason);
    }

    // Шаг 3: ресурсы платформы — защищаем базовые пины выбранной платы
    // (5.3.0: набор зависит от BoardDesc: WT32-ETH01 vs ESP32-S3-POE-ETH)
    ResourceManager& rm = ResourceManager::getInstance();
    const platform::BoardDesc& bd = platform::board();
    if (bd.ethPowerPin >= 0) {   // RMII: питание PHY LAN8720
        rm.claimGpio((uint8_t)bd.ethPowerPin, "platform.eth_phy_power");
    }
    if (bd.ethKind == platform::EthKind::SpiW5500) {
        rm.claimGpio((uint8_t)bd.ethCs,   "platform.eth_cs");
        rm.claimGpio((uint8_t)bd.ethIrq,  "platform.eth_irq");
        rm.claimGpio((uint8_t)bd.ethRst,  "platform.eth_rst");
        rm.claimGpio((uint8_t)bd.ethSck,  "platform.eth_sck");
        rm.claimGpio((uint8_t)bd.ethMiso, "platform.eth_miso");
        rm.claimGpio((uint8_t)bd.ethMosi, "platform.eth_mosi");
    }
    rm.claimGpio(bd.i2cSda, "platform.i2c_sda");
    rm.claimGpio(bd.i2cScl, "platform.i2c_scl");
    rm.claimI2cAddress(0x68, "platform.ds3231");
    rm.claimUart(0, "platform.console");          // UART0 — Serial-консоль
    if (bd.sdPresent) {          // слот microSD — отдельная SPI-шина платы
        rm.claimGpio((uint8_t)bd.sdCs,   "platform.sd_cs");
        rm.claimGpio((uint8_t)bd.sdSck,  "platform.sd_sck");
        rm.claimGpio((uint8_t)bd.sdMiso, "platform.sd_miso");
        rm.claimGpio((uint8_t)bd.sdMosi, "platform.sd_mosi");
    }

    // Шаг 4: валидация манифеста периферии профиля через реестр (A2).
    // Конфликт -> профиль не стартует, но система продолжает в Safe Mode-
    // подобном урезанном виде (Phase 1: явный перевод в SafeModeReason::PANIC).
    if (!isSafeMode()) {
        bool ok = manifest.validateResources(rm);
        if (!ok) {
            Serial.println(F("[KERNEL] CRITICAL: profile resource conflict!"));
            _safeReason = SafeModeReason::PANIC;   // урезанный режим
        }
    }

    // Шаг 5: шина событий — до любых модулей
    EventBus::getInstance().begin();
    if (isSafeMode()) {
        ShEventData d; d.clear();
        d.code = (int32_t)_safeReason;
        EventBus::getInstance().post(SH_EVENT_SAFE_MODE_ENTERED, &d);
    }

    // Шаг 6: регистрация драйверов и модулей.
    // 6a. Модули ядра — всегда (и в Safe Mode: шины и реестр нужны для
    //     диагностики/recovery). Приоритеты: BusManager раньше реестра,
    //     чтобы шина была жива до init() драйверов.
    registerModule(&StorageService::getInstance(), /*prio*/ 0, /*tickMs*/ 0);
    registerModule(&BusManager::getInstance(),     /*prio*/ 0, /*tickMs*/ 0);
    // LogService: prio 0 — его describe() подключает приёмник логов самой
    // ранней фазой, до registerExtensions/init остальных модулей.
    registerModule(&LogService::getInstance(),     /*prio*/ 0, /*tickMs*/ 0);
    registerModule(&DriverRegistry::getInstance(), /*prio*/ 1, /*tickMs*/ 0);
    registerModule(&ConfigService::getInstance(),  /*prio*/ 1, /*tickMs*/ 0);
    // AuthService: prio 1 ПОСЛЕ ConfigService (читает auth.* при init).
    // Ядерный модуль — нужен и в Safe Mode (recovery-UI защищён ПИНом).
    registerModule(&AuthService::getInstance(),    /*prio*/ 1, /*tickMs*/ 0);
    registerModule(&TimeService::getInstance(),    /*prio*/ 2, /*tickMs*/ 0);
    // ScheduleService: prio 2, сразу после времени — доменные временные
    // правила (ночной режим и т.п.) опираются только на TimeService,
    // а их события могут понадобиться уже ранним подписчикам.
    registerModule(&ScheduleService::getInstance(),/*prio*/ 2, /*tickMs*/ 0);
    // CounterService: prio 3 — счётчики опираются на Config/Storage,
    // а их потребители (профильные исполнители) стартуют позже.
    registerModule(&CounterService::getInstance(), /*prio*/ 3, /*tickMs*/ 0);
    // NetworkService: prio 2 (конфиг уже загружен). Ядерный — в Safe Mode
    // сеть обязательна: веб-интерфейс восстановления и OTA.
    registerModule(&NetworkService::getInstance(), /*prio*/ 2, /*tickMs*/ 0);
    // Наблюдаемость: телеметрия и аудит после сети (читают её состояние).
    registerModule(&TelemetryService::getInstance(), /*prio*/ 3, /*tickMs*/ 0);
    registerModule(&AuditService::getInstance(),   /*prio*/ 3, /*tickMs*/ 0);
    registerModule(&DataLogService::getInstance(), /*prio*/ 3, /*tickMs*/ 0);
    // MQTT-мост: транспорт, после сети и телеметрии (публикует их данные).
    registerModule(&MqttTransport::getInstance(),  /*prio*/ 3, /*tickMs*/ 0);
    // Веб-сервер: ядерный — в Safe Mode это recovery-интерфейс (OTA/конфиг).
    registerModule(&HttpService::getInstance(),    /*prio*/ 3, /*tickMs*/ 0);
    // Звуковой диспетчер: после драйверов (prio 1), единственный «рот».
    registerModule(&AudioService::getInstance(),   /*prio*/ 3, /*tickMs*/ 0);
    registerModule(&UpdateService::getInstance(),  /*prio*/ 3, /*tickMs*/ 0);
    registerModule(&HealthMonitor::getInstance(),  /*prio*/ 4, /*tickMs*/ 0);

    // 6b. Базовые драйверы платформы (есть у любого устройства: температура
    //     кристалла — единственный источник, HealthMonitor его подписчик).
    DriverRegistry::getInstance().add(&EspTempDriver::getInstance());
    // DS3231 — часы платформы (WT32-ETH01+DS3231). Урок 5.0.x: драйвер
    // был написан и протестирован, но НИГДЕ не регистрировался — TimeService
    // не находил его (findAs == nullptr) и стартовал с «RTC dead», время
    // жило только от NTP/браузера, а Fail-Safe кнопки выхода (мёртвый RTC
    // = разрешена всегда) законно отменял расписание. BusManager к этому
    // моменту уже поднял I2C (фаза init реестра — после BusManager).
    DriverRegistry::getInstance().add(&Ds3231Driver::getInstance());

    // 6c. Профильные драйверы и модули — только в штатном режиме.
    if (!isSafeMode()) {
        profile->registerDrivers(manifest);      // драйверы периферии
        profile->registerModules(*this);         // доменные сервисы + приложение
    } else {
        Serial.println(F("[KERNEL] Safe Mode: profile modules skipped"));
    }

    // Шаги 7–10: фазы жизненного цикла
    runPhaseDescribe();
    runPhaseRegisterExtensions();
    runPhaseInit();
    runPhaseStart();

    _stableSinceMs = millis();
    _ready = true;

    // SH_EVENT_READY — точка отсчёта "система работает"
    ShEventData d; d.clear();
    EventBus::getInstance().post(SH_EVENT_READY, &d);
    Serial.printf("[KERNEL] Boot complete, %u modules, safe=%d\n",
                  _moduleCount, (int)isSafeMode());
}

// ============================================================================
// РЕГИСТРАЦИЯ МОДУЛЕЙ
// ============================================================================
bool Kernel::registerModule(IModule* module, uint8_t priority, uint32_t tickMs) {
    if (module == nullptr) return false;
    if (_moduleCount >= KERNEL_MAX_MODULES) {
        Serial.printf("[KERNEL] ERROR: module table full, '%s' rejected\n",
                      module->getName());
        return false;
    }
    _slots[_moduleCount] = { module, priority, tickMs, 0 };
    _moduleCount++;
    return true;
}

// ============================================================================
// ФАЗЫ ЖИЗНЕННОГО ЦИКЛА
// ============================================================================
// Сортировка по приоритету — вставками (таблица <= 24, сортируем один раз).
// Все четыре фазы идут в ОДНОМ порядке — приоритет определяет и порядок
// registerExtensions: системные сервисы регистрируют точки расширения
// раньше, чем профильные модули начнут в них инжектить.
// ============================================================================
static void sortByPriority(Kernel::ModuleSlot* slots, uint8_t count) {
    for (uint8_t i = 1; i < count; ++i) {
        Kernel::ModuleSlot key = slots[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && slots[j].priority > key.priority) {
            slots[j + 1] = slots[j];
            --j;
        }
        slots[j + 1] = key;
    }
}

uint8_t Kernel::runPhaseDescribe() {
    sortByPriority(_slots, _moduleCount);
    uint8_t ok = 0;
    for (uint8_t i = 0; i < _moduleCount; ++i) {
        _slots[i].module->describe();
        ++ok;
    }
    Serial.printf("[KERNEL] Phase describe: %u modules\n", ok);
    return ok;
}

uint8_t Kernel::runPhaseRegisterExtensions() {
    uint8_t ok = 0;
    for (uint8_t i = 0; i < _moduleCount; ++i) {
        _slots[i].module->registerExtensions();
        ++ok;
    }
    Serial.printf("[KERNEL] Phase registerExtensions: %u modules\n", ok);
    return ok;
}

uint8_t Kernel::runPhaseInit() {
    uint8_t ok = 0;
    for (uint8_t i = 0; i < _moduleCount; ++i) {
        IModule* m = _slots[i].module;
        Serial.printf("[KERNEL] init: %s\n", m->getName());
        m->init();
        if (m->isReady() || true) ++ok;   // isReady взлетит после start;
    }
    Serial.printf("[KERNEL] Phase init: %u modules\n", ok);
    return ok;
}

uint8_t Kernel::runPhaseStart() {
    uint8_t ok = 0;
    for (uint8_t i = 0; i < _moduleCount; ++i) {
        IModule* m = _slots[i].module;
        m->start();
        ++ok;

        ShEventData d; d.clear();
        d.code = 2;   // 2 = start
        d.sourceModule = m->getModuleId();
        EventBus::getInstance().post(SH_EVENT_MODULE_STATUS, &d);
    }
    Serial.printf("[KERNEL] Phase start: %u modules\n", ok);
    return ok;
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void Kernel::loop() {
    if (!_ready) return;
    uint32_t now = millis();

    // --- Сброс bootloop-счётчика после 60 с стабильной работы (A1) -------
    // "Стабильная" = прошло KERNEL_STABLE_MS с завершения start-фазы.
    if (_bootloopCount > 0 && now - _stableSinceMs > KERNEL_STABLE_MS) {
        s_rtc.bootloopCount = 0;
        _bootloopCount = 0;
        Serial.println(F("[KERNEL] Stable 60s: bootloop counter reset"));
    }

    // --- Тик модулей по индивидуальным периодам + бюджет (B2) -------------
    for (uint8_t i = 0; i < _moduleCount; ++i) {
        ModuleSlot& s = _slots[i];
        uint32_t interval = s.tickMs ? s.tickMs : s.module->getTickIntervalMs();
        if (now - s.lastTickMs < interval) continue;
        s.lastTickMs = now;

        uint32_t t0 = micros();
        s.module->tick();
        uint32_t elapsedMs = (micros() - t0) / 1000;

        // Превышение бюджета — событие для HealthMonitor/телеметрии.
        // Сам Kernel не наказывает: решение — у ПАЗ (Phase 1+).
        if (elapsedMs > KERNEL_TICK_BUDGET_MS) {
            ShEventData d; d.clear();
            d.code = (int32_t)elapsedMs;
            d.sourceModule = s.module->getModuleId();
            EventBus::getInstance().post(SH_EVENT_TICK_OVERRUN, &d);
        }
    }

    // Phase 1: esp_task_wdt_reset() + HealthMonitor.tick здесь же.
}
