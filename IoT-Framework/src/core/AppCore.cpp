// ============================================================================
// AppCore.cpp - ULTIMATE MICRO-OS CORE V4.2.2 (AUDITED)
// ============================================================================
// Описание:
// Реализация ядра системы - оркестратор, диспетчер событий,
// управление модулями, watchdog, статистика.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - Исправлена ошибка в dispatchEvent (перебор ModuleInfo вместо IModule*)
// - Добавлен метод postEventFromISR для безопасной отправки из прерываний
// - Добавлены методы логирования (logError, logInfo, logDebug)
// - Исправлена проблема с thread_local для отслеживания рекурсии
// - Добавлено разделение системных и прикладных событий по шинам
// - Увеличен стек задач до 6144 байт
// - Добавлен сброс WDT в фоновых задачах
// - Добавлен метод getModuleById
// - Добавлен процесс обработки tick() модулей
// ============================================================================
#include <cstring>
#include <cstdarg>
#include "core/AppCore.h"
#include "core/IModule.h"
#include "IoTFramework.h"
#include <esp_task_wdt.h>
#include <esp_timer.h>

// ============================================================================
// ОПРЕДЕЛЕНИЕ ШИН СОБЫТИЙ
// ============================================================================
ESP_EVENT_DEFINE_BASE(SH_SYS_EVENTS);
ESP_EVENT_DEFINE_BASE(SH_APP_EVENTS);

// ============================================================================
// ГЛОБАЛЬНЫЙ ЭКЗЕМПЛЯР
// ============================================================================
AppCore& AppCore::getInstance() {
    static AppCore instance;
    return instance;
}

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
AppCore::AppCore() {
    _startTime = millis();
    _uptime = 0;
    _ready = false;
    _shuttingDown = false;
    _wdtEnabled = true;
    _wdtTimeoutMs = 8000;
    _eventCounter = 0;
    _restartCount = 0;
    _lastHealthCheck = 0;
    _lastStatsUpdate = 0;
    _healthTaskHandle = nullptr;
    _eventProcessorHandle = nullptr;
    _hwWdtTimer = nullptr;
    _verboseLogging = false;
    _onError = nullptr;
    _onHealthChange = nullptr;

    // Создаем РЕКУРСИВНЫЕ МЬЮТЕКСЫ
    _modulesMutex = xSemaphoreCreateRecursiveMutex();
    _statsMutex = xSemaphoreCreateRecursiveMutex();

    if (_modulesMutex == nullptr || _statsMutex == nullptr) {
        Serial.println("[CORE] FATAL: Failed to create mutexes!");
        while (1) { delay(100); }
    }

    // Инициализируем статистику
    memset(&_stats, 0, sizeof(_stats));
    _stats.cpuFreq = ESP.getCpuFreqMHz();
    _stats.heapMinFree = ESP.getFreeHeap();
    _stats.healthStatus = 0;

    // Создаем очередь событий
    _eventQueue = xQueueCreate(_eventQueueSize, sizeof(QueuedEvent));
    if (_eventQueue == nullptr) {
        Serial.println("[CORE] FATAL: Failed to create event queue!");
        while (1) { delay(100); }
    }

    Serial.println("[CORE] Instance created (v4.2.2)");
}

AppCore::~AppCore() {
    shutdown();
    if (_modulesMutex) vSemaphoreDelete(_modulesMutex);
    if (_statsMutex) vSemaphoreDelete(_statsMutex);
    if (_eventQueue) vQueueDelete(_eventQueue);
    if (_hwWdtTimer) timerEnd(_hwWdtTimer);
    Serial.println("[CORE] Instance destroyed");
}

// ============================================================================
// 2. ЗАПУСК СИСТЕМЫ
// ============================================================================
void AppCore::begin() {
    if (_ready) return;

    Serial.println("\n===========================================");
    Serial.println(" MICRO-OS v4.2.2 STARTING (AUDITED)");
    Serial.println(" Event-Driven Architecture (ESP32 Core v3.x)");
    Serial.println("===========================================\n");

    // 1. Инициализируем событийную шину
    initEventLoop();

    // 2. Инициализируем аппаратный Watchdog
    if (_wdtEnabled) {
        initHardwareWatchdog();
    }

    // 3. Отправляем событие начала загрузки
    ShEventData bootData;
    memset(&bootData, 0, sizeof(ShEventData));
    bootData.sourceModule = 0;
    bootData.targetModule = 0;
    bootData.value = 0;
    strncpy(bootData.payload, _version, sizeof(bootData.payload) - 1);
    bootData.payloadLen = strlen(_version);
    postEvent(SH_EVENT_SYS_BOOT, &bootData);

    // 4. Инициализируем все зарегистрированные модули
    initModules();

    // 5. Запускаем все модули
    startModules();

    // 6. Создаем фоновые задачи (с увеличенным стеком)
    xTaskCreatePinnedToCore(
        healthTask,
        "CoreHealth",
        6144,  // Увеличено с 4096
        this,
        1,     // Низкий приоритет
        &_healthTaskHandle,
        0      // Core 0
    );

    xTaskCreatePinnedToCore(
        eventProcessorTask,
        "CoreEvents",
        6144,  // Увеличено с 4096
        this,
        3,     // Средний приоритет
        &_eventProcessorHandle,
        0      // Core 0
    );

    // 7. Отправляем событие готовности
    _ready = true;
    postEvent(SH_EVENT_SYS_READY);

    Serial.println("\n===========================================");
    Serial.printf(" MICRO-OS v%s READY\n", _version);
    Serial.printf(" Modules: %zu\n", _modules.size());
    Serial.printf(" Free Heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf(" Event Queue: %zu slots\n", _eventQueueSize);
    Serial.println("===========================================\n");
}

// ============================================================================
// 3. ГЛАВНЫЙ ЦИКЛ
// ============================================================================
void AppCore::loop() {
    if (!_ready) return;

    // Сбрасываем аппаратный Watchdog
    resetHardwareWatchdog();

    // Обрабатываем периодические вызовы tick() модулей
    processModuleTick();

    // Обновляем статистику
    updateStats();

    // Проверяем здоровье
    uint32_t now = millis();
    if (now - _lastHealthCheck > _healthCheckIntervalMs) {
        _lastHealthCheck = now;
        checkHealth();
    }

    // Небольшая задержка для стабильности
    vTaskDelay(pdMS_TO_TICKS(10));
}

// ============================================================================
// 4. ИНИЦИАЛИЗАЦИЯ СОБЫТИЙНОЙ ШИНЫ
// ============================================================================
void AppCore::initEventLoop() {
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[CORE] Failed to create event loop: %d\n", err);
        emergencyStop();
        return;
    }

    // Регистрируем глобальный обработчик для системных событий
    err = esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &AppCore::eventHandler,
        this,
        NULL
    );
    if (err != ESP_OK) {
        Serial.printf("[CORE] Failed to register sys handler: %d\n", err);
    }

    // Регистрируем глобальный обработчик для прикладных событий
    err = esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &AppCore::eventHandler,
        this,
        NULL
    );
    if (err != ESP_OK) {
        Serial.printf("[CORE] Failed to register app handler: %d\n", err);
    } else {
        Serial.println("[CORE] Event loop initialized");
    }
}

// ============================================================================
// 5. ОБРАБОТЧИК СОБЫТИЙ (СТАВИТ В ОЧЕРЕДЬ)
// ============================================================================
void AppCore::eventHandler(void* handlerArgs, esp_event_base_t base,
                          int32_t id, void* eventData) {
    AppCore* instance = static_cast<AppCore*>(handlerArgs);
    if (!instance || instance->_shuttingDown) return;

    // Увеличиваем счетчик
    instance->_eventCounter++;

    // Формируем событие для очереди
    QueuedEvent qEvent;
    qEvent.eventId = id;
    qEvent.hasData = (eventData != nullptr);
    if (qEvent.hasData) {
        memcpy(&qEvent.data, eventData, sizeof(ShEventData));
    } else {
        memset(&qEvent.data, 0, sizeof(ShEventData));
    }

    // Отправляем в очередь (неблокирующая, из ISR)
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(instance->_eventQueue, &qEvent, &higherPriorityTaskWoken) != pdPASS) {
        // Очередь переполнена
        instance->_stats.queueOverflows++;
        if (instance->_verboseLogging) {
            // В ISR нельзя использовать Serial.printf
            // Используем ESP_LOG или сохраняем в буфер
        }
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

// ============================================================================
// 6. ОБРАБОТЧИК ОЧЕРЕДИ СОБЫТИЙ (ЗАДАЧА FREERTOS)
// ============================================================================
void AppCore::eventProcessorTask(void* parameters) {
    AppCore* instance = static_cast<AppCore*>(parameters);
    QueuedEvent qEvent;

    while (1) {
        // Сбрасываем WDT в каждой итерации
        instance->resetHardwareWatchdog();

        if (xQueueReceive(instance->_eventQueue, &qEvent, pdMS_TO_TICKS(100)) == pdPASS) {
            // === ОБРАБОТКА СИСТЕМНЫХ СОБЫТИЙ ===

            // Ошибка модуля
            if (qEvent.eventId == SH_EVENT_MODULE_ERROR) {
                instance->_stats.errors++;

                // Отправляем в LogManager, если он доступен
                IModule* logModule = instance->getModuleByName("LogManager");
                if (logModule) {
                    ShEventData logData;
                    memset(&logData, 0, sizeof(ShEventData));
                    logData.sourceModule = 0;
                    logData.targetModule = MODULE_ID_LOG;
                    logData.value = qEvent.data.value;
                    snprintf(logData.payload, sizeof(logData.payload),
                            "MODULE_ERROR: %s (code: %d)",
                            qEvent.data.payload, qEvent.data.value);
                    logData.payloadLen = strlen(logData.payload);
                    logModule->postEvent(SH_EVENT_CMD_EXECUTE, &logData);
                }

                Serial.printf("[CORE] Module error: %s (code: %d)\n",
                             qEvent.data.payload, qEvent.data.value);
                continue;
            }

            // Перезагрузка системы
            if (qEvent.eventId == SH_EVENT_SYS_RESTART) {
                instance->restart();
                continue;
            }

            // Критическое состояние здоровья
            if (qEvent.eventId == SH_EVENT_HEALTH_CRITICAL) {
                Serial.println("[CORE] CRITICAL HEALTH ISSUE DETECTED!");
                if (instance->_stats.healthStatus < 2) {
                    instance->_stats.healthStatus = 2;
                    if (instance->_onHealthChange) {
                        instance->_onHealthChange(2);
                    }
                }
                continue;
            }

            // Восстановление здоровья
            if (qEvent.eventId == SH_EVENT_HEALTH_RESTORED) {
                if (instance->_stats.healthStatus > 0) {
                    instance->_stats.healthStatus = 0;
                    if (instance->_onHealthChange) {
                        instance->_onHealthChange(0);
                    }
                }
                continue;
            }

            // Передаем событие модулям
            instance->dispatchEvent(qEvent.eventId,
                                   qEvent.hasData ? &qEvent.data : nullptr);
        }

        // Обновляем статистику
        instance->updateStats();
    }
}

// ============================================================================
// 7. ДИСПЕТЧЕРИЗАЦИЯ СОБЫТИЙ
// ============================================================================
void AppCore::dispatchEvent(int32_t eventId, const ShEventData* data) {
    if (!_modulesMutex) return;

    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        _stats.errors++;
        return;
    }

    // ИСПРАВЛЕНО: перебираем ModuleInfo, а не IModule*
    for (const auto& info : _modules) {
        if (info.module && info.module->canHandleEvent(eventId)) {
            // Проверяем, что событие адресовано этому модулю или всем
            if (data == nullptr || data->targetModule == 0 ||
                data->targetModule == info.module->getModuleId()) {
                dispatchToModule(info.module, eventId, data);
            }
        }
    }

    xSemaphoreGiveRecursive(_modulesMutex);
}

// ============================================================================
// 8. ОТПРАВКА В МОДУЛЬ (С ЗАЩИТОЙ ОТ РЕКУРСИИ)
// ============================================================================
void AppCore::dispatchToModule(IModule* module, int32_t eventId, const ShEventData* data) {
    if (!module || !module->isReady()) return;

    // Используем thread_local для защиты от гонок
    // Каждая задача имеет свой счетчик рекурсии
    static thread_local int recursionDepth = 0;

    if (recursionDepth > MAX_RECURSION_DEPTH) {
        Serial.printf("[CORE] Recursion depth exceeded! Module: %s\n", module->getName());
        ShEventData errorData;
        memset(&errorData, 0, sizeof(ShEventData));
        errorData.sourceModule = 0;
        errorData.targetModule = module->getModuleId();
        errorData.value = recursionDepth;
        strncpy(errorData.payload, "RECURSION_DEPTH_EXCEEDED", sizeof(errorData.payload) - 1);
        errorData.payloadLen = strlen(errorData.payload);
        postEvent(SH_EVENT_MODULE_ERROR, &errorData);
        return;
    }

    recursionDepth++;
    module->onEvent(eventId, data);
    recursionDepth--;
}

// ============================================================================
// 9. ОТПРАВКА СОБЫТИЙ (С РАЗДЕЛЕНИЕМ ШИН)
// ============================================================================
void AppCore::postEvent(int32_t eventId, const ShEventData* data) {
    if (_shuttingDown) return;
    _eventCounter++;

    // Разделяем системные и прикладные события
    esp_event_base_t base;
    if (eventId >= SH_EVENT_USER_BASE) {
        base = SH_APP_EVENTS;   // Прикладные события (>= 4096)
    } else {
        base = SH_SYS_EVENTS;   // Системные события (0-4095)
    }

    esp_err_t err = esp_event_post(
        base,
        eventId,
        const_cast<ShEventData*>(data),
        data ? sizeof(ShEventData) : 0,
        pdMS_TO_TICKS(50)
    );

    if (err != ESP_OK) {
        _stats.errors++;
        if (_verboseLogging) {
            Serial.printf("[CORE] Failed to post event %d to %s: %d\n",
                         eventId, base == SH_SYS_EVENTS ? "SYS" : "APP", err);
        }
    }
}

// ============================================================================
// 10. ОТПРАВКА ИЗ ISR (НОВЫЙ МЕТОД)
// ============================================================================
void AppCore::postEventFromISR(int32_t eventId, const ShEventData* data,
                               BaseType_t* higherPriorityTaskWoken) {
    if (_shuttingDown || !_eventQueue) return;

    QueuedEvent qEvent;
    qEvent.eventId = eventId;
    qEvent.hasData = (data != nullptr);
    if (qEvent.hasData) {
        memcpy(&qEvent.data, data, sizeof(ShEventData));
    } else {
        memset(&qEvent.data, 0, sizeof(ShEventData));
    }

    if (xQueueSendFromISR(_eventQueue, &qEvent, higherPriorityTaskWoken) != pdPASS) {
        _stats.queueOverflows++;
    }
}

// ============================================================================
// 11. ОТПРАВКА КОМАНДЫ
// ============================================================================
void AppCore::postCommand(uint32_t targetModule, uint32_t command,
                         int32_t value, const char* payload) {
    ShEventData cmd;
    memset(&cmd, 0, sizeof(ShEventData));
    cmd.sourceModule = 0; // AppCore
    cmd.targetModule = targetModule;
    cmd.command = command;
    cmd.value = value;

    if (payload) {
        size_t len = strnlen(payload, sizeof(cmd.payload) - 1);
        memcpy(cmd.payload, payload, len);
        cmd.payload[len] = '\0';
        cmd.payloadLen = len;
    }

    postEvent(SH_EVENT_CMD_EXECUTE, &cmd);
}

// ============================================================================
// 12. ОТПРАВКА СОБЫТИЯ МОДУЛЮ
// ============================================================================
void AppCore::postEventToModule(uint32_t moduleId, int32_t eventId, const ShEventData* data) {
    if (_shuttingDown) return;

    ShEventData eventData;
    if (data) {
        memcpy(&eventData, data, sizeof(ShEventData));
    } else {
        memset(&eventData, 0, sizeof(ShEventData));
    }
    eventData.targetModule = moduleId;
    postEvent(eventId, &eventData);
}

// ============================================================================
// 13. РЕГИСТРАЦИЯ МОДУЛЕЙ
// ============================================================================
void AppCore::registerModule(IModule* module, uint8_t priority, uint32_t tickInterval) {
    if (!module || !_modulesMutex) return;

    // Проверяем, что модуль еще не зарегистрирован
    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (const auto& info : _modules) {
            if (info.module == module) {
                xSemaphoreGiveRecursive(_modulesMutex);
                Serial.printf("[CORE] Module %s already registered\n", module->getName());
                return;
            }
        }

        // Добавляем модуль с приоритетом
        ModuleInfo info;
        info.module = module;
        info.priority = priority;
        info.tickInterval = tickInterval;
        info.lastTickMs = millis();
        info.enabled = true;
        _modules.push_back(info);

        // Сортируем по приоритету
        std::sort(_modules.begin(), _modules.end(),
                  [](const ModuleInfo& a, const ModuleInfo& b) {
                      return a.priority < b.priority;
                  });

        xSemaphoreGiveRecursive(_modulesMutex);
    }

    // Отправляем событие о регистрации
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = 0;
    data.targetModule = 0;
    data.value = module->getModuleId();
    strncpy(data.payload, module->getName(), sizeof(data.payload) - 1);
    data.payloadLen = strlen(module->getName());
    postEvent(SH_EVENT_MODULE_REGISTERED, &data);

    Serial.printf("[CORE] Module registered: %s (ID: %u, v%s, prio: %d)\n",
                 module->getName(), module->getModuleId(),
                 module->getVersion(), priority);
}

void AppCore::registerModules(const std::vector<IModule*>& modules) {
    for (auto* module : modules) {
        registerModule(module);
    }
}

// ============================================================================
// 14. ПОИСК МОДУЛЕЙ
// ============================================================================
IModule* AppCore::getModuleByName(const char* name) const {
    if (!name || !_modulesMutex) return nullptr;

    IModule* found = nullptr;
    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& info : _modules) {
            if (info.module && strcmp(info.module->getName(), name) == 0) {
                found = info.module;
                break;
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }
    return found;
}

IModule* AppCore::getModuleById(uint32_t moduleId) const {
    if (!_modulesMutex) return nullptr;

    IModule* found = nullptr;
    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& info : _modules) {
            if (info.module && info.module->getModuleId() == moduleId) {
                found = info.module;
                break;
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }
    return found;
}

// ============================================================================
// 15. ИНИЦИАЛИЗАЦИЯ И ЗАПУСК МОДУЛЕЙ
// ============================================================================
void AppCore::initModules() {
    if (!_modulesMutex) return;

    Serial.println("[CORE] Initializing modules...");
    postEvent(SH_EVENT_MODULE_INIT_START);

    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        for (auto& info : _modules) {
            if (info.module) {
                Serial.printf("[CORE] Initializing %s...\n", info.module->getName());
                info.module->init();
                info.module->setInitialized(true);

                // Отправляем событие
                ShEventData data;
                memset(&data, 0, sizeof(ShEventData));
                data.sourceModule = 0;
                data.targetModule = info.module->getModuleId();
                data.value = 1;
                strncpy(data.payload, info.module->getName(), sizeof(data.payload) - 1);
                data.payloadLen = strlen(info.module->getName());
                postEvent(SH_EVENT_MODULE_INIT_DONE, &data);
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }

    postEvent(SH_EVENT_MODULE_INIT_DONE);
    Serial.printf("[CORE] All %zu modules initialized\n", _modules.size());
}

void AppCore::startModules() {
    if (!_modulesMutex) return;

    Serial.println("[CORE] Starting modules...");

    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (auto& info : _modules) {
            if (info.module && info.module->isReady()) {
                info.module->start();
                info.module->setReadyFlag(true);
                Serial.printf("[CORE] Module started: %s\n", info.module->getName());
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }
}

// ============================================================================
// 16. ОБРАБОТКА TICK МОДУЛЕЙ
// ============================================================================
void AppCore::processModuleTick() {
    if (!_modulesMutex) return;

    uint32_t now = millis();

    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (auto& info : _modules) {
            if (info.module && info.enabled && info.tickInterval > 0) {
                if (now - info.lastTickMs >= info.tickInterval) {
                    info.lastTickMs = now;
                    info.module->tick();
                }
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }
}

// ============================================================================
// 17. СТАТИСТИКА
// ============================================================================
void AppCore::updateStats() {
    uint32_t now = millis();
    if (now - _lastStatsUpdate < 1000) return;
    _lastStatsUpdate = now;

    if (_statsMutex && xSemaphoreTakeRecursive(_statsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        _stats.uptime = (now - _startTime) / 1000;
        _stats.freeHeap = ESP.getFreeHeap();
        _stats.freePsram = ESP.getFreePsram();
        _stats.modulesCount = _modules.size();
        _stats.eventsProcessed = _eventCounter;
        _stats.totalRestarts = _restartCount;
        _stats.cpuFreq = ESP.getCpuFreqMHz();

        // Обновляем минимальную память
        if (_stats.freeHeap < _stats.heapMinFree || _stats.heapMinFree == 0) {
            _stats.heapMinFree = _stats.freeHeap;
        }

        // Температура чипа (если доступна)
        #ifdef ESP_TEMPERATURE_SENSOR_SUPPORTED
        _stats.temperature = temperatureRead();
        #endif

        xSemaphoreGiveRecursive(_statsMutex);
    }
}

// ============================================================================
// 18. ПРОВЕРКА ЗДОРОВЬЯ
// ============================================================================
void AppCore::checkHealth() {
    bool hasWarning = false;
    bool hasCritical = false;

    // 1. Проверка памяти
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 10000) {
        hasCritical = true;
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = 0;
        data.targetModule = 0;
        data.value = freeHeap;
        strncpy(data.payload, "LOW_MEMORY_CRITICAL", sizeof(data.payload) - 1);
        data.payloadLen = strlen(data.payload);
        postEvent(SH_EVENT_HEALTH_CRITICAL, &data);
    } else if (freeHeap < 20000) {
        hasWarning = true;
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = 0;
        data.targetModule = 0;
        data.value = freeHeap;
        strncpy(data.payload, "LOW_MEMORY_WARNING", sizeof(data.payload) - 1);
        data.payloadLen = strlen(data.payload);
        postEvent(SH_EVENT_HEALTH_WARNING, &data);
    }

    // 2. Проверка модулей
    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (const auto& info : _modules) {
            if (info.module && !info.module->isReady()) {
                hasWarning = true;
                if (_verboseLogging) {
                    Serial.printf("[CORE] Module %s not ready\n", info.module->getName());
                }
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }

    // 3. Обновляем статус здоровья
    if (_statsMutex && xSemaphoreTakeRecursive(_statsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        uint8_t oldStatus = _stats.healthStatus;
        if (hasCritical) {
            _stats.healthStatus = 2;
        } else if (hasWarning) {
            _stats.healthStatus = 1;
        } else {
            _stats.healthStatus = 0;
        }

        // Вызываем колбэк при изменении статуса
        if (oldStatus != _stats.healthStatus && _onHealthChange) {
            _onHealthChange(_stats.healthStatus);
        }

        xSemaphoreGiveRecursive(_statsMutex);
    }
}

// ============================================================================
// 19. ЗАДАЧА ЗДОРОВЬЯ
// ============================================================================
void AppCore::healthTask(void* parameters) {
    AppCore* instance = static_cast<AppCore*>(parameters);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        // Сбрасываем WDT
        instance->resetHardwareWatchdog();

        // Проверка температуры чипа
        #ifdef ESP_TEMPERATURE_SENSOR_SUPPORTED
        float temp = temperatureRead();
        if (temp > 80.0f) {
            Serial.printf("[CORE] WARNING: CPU temperature %.1f°C\n", temp);
            instance->postEvent(SH_EVENT_HEALTH_WARNING);
        }
        #endif

        // Проверка свободной памяти
        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 8000) {
            Serial.printf("[CORE] CRITICAL: Low heap %u bytes\n", freeHeap);
            instance->postEvent(SH_EVENT_HEALTH_CRITICAL);
        }
    }
}

// ============================================================================
// 20. АППАРАТНЫЙ WATCHDOG
// ============================================================================
void AppCore::initHardwareWatchdog() {
    // Используем таймер 0 с делителем 80 (1 МГц)
    _hwWdtTimer = timerBegin(0, 80, true);
    if (_hwWdtTimer) {
        timerAttachInterrupt(_hwWdtTimer, &AppCore::hwWatchdogISR, true);
        timerAlarmWrite(_hwWdtTimer, _wdtTimeoutMs * 1000, false);
        timerAlarmEnable(_hwWdtTimer);
        Serial.printf("[CORE] HW Watchdog armed: %lu ms\n", _wdtTimeoutMs);
    } else {
        Serial.println("[CORE] WARNING: Failed to init HW Watchdog");
        // Используем Task Watchdog как fallback
        esp_task_wdt_init(_wdtTimeoutMs / 1000, true);
        esp_task_wdt_add(NULL);
    }
}

void IRAM_ATTR AppCore::hwWatchdogISR() {
    // Жесткий сброс при срабатывании
    esp_restart();
}

void AppCore::resetHardwareWatchdog() {
    if (_hwWdtTimer) {
        timerWrite(_hwWdtTimer, 0); // Сброс счетчика
    } else {
        // Fallback: сбрасываем Task Watchdog
        esp_task_wdt_reset();
    }
}

void AppCore::feedWatchdog() {
    resetHardwareWatchdog();
}

// ============================================================================
// 21. СИСТЕМНЫЕ ВЫЗОВЫ
// ============================================================================
void AppCore::emergencyStop() {
    Serial.println("[CORE] !!! EMERGENCY STOP EXECUTION !!!");

    if (_healthTaskHandle) {
        vTaskDelete(_healthTaskHandle);
        _healthTaskHandle = nullptr;
    }
    if (_eventProcessorHandle) {
        vTaskDelete(_eventProcessorHandle);
        _eventProcessorHandle = nullptr;
    }
    if (_hwWdtTimer) {
        timerEnd(_hwWdtTimer);
        _hwWdtTimer = nullptr;
    }

    _shuttingDown = true;

    // Бесконечный цикл с миганием встроенным LED
    pinMode(LED_BUILTIN, OUTPUT);
    while (1) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(100);
        digitalWrite(LED_BUILTIN, LOW);
        delay(900);
    }
}

void AppCore::restart() {
    Serial.println("[CORE] System restart initiated...");
    _restartCount++;
    _stats.totalRestarts = _restartCount;

    // Отправляем событие перезагрузки
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = 0;
    data.targetModule = 0;
    data.value = _restartCount;
    strncpy(data.payload, "RESTART", sizeof(data.payload) - 1);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_SYS_RESTART, &data);

    // Даем время на отправку события
    vTaskDelay(pdMS_TO_TICKS(100));

    // Перезагружаемся
    esp_restart();
}

void AppCore::shutdown() {
    Serial.println("[CORE] System shutdown...");

    if (_modulesMutex && xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (auto& info : _modules) {
            if (info.module) {
                info.module->stop();
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }

    _shuttingDown = true;
    _ready = false;
}

// ============================================================================
// 22. ЛОГИРОВАНИЕ
// ============================================================================
void AppCore::logError(const char* module, const char* format, ...) {
    va_list args;
    va_start(args, format);
    logError(module, format, args);
    va_end(args);
}

void AppCore::logError(const char* module, const char* format, va_list args) {
    char buffer[128];
    vsnprintf(buffer, sizeof(buffer), format, args);

    // Вывод в Serial
    Serial.printf("[%s] ERROR: %s\n", module, buffer);

    // Отправка в LogManager
    IModule* logModule = getModuleByName("LogManager");
    if (logModule) {
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = 0;
        data.targetModule = MODULE_ID_LOG;
        data.value = 1; // ERROR level
        strncpy(data.payload, buffer, sizeof(data.payload) - 1);
        data.payloadLen = strlen(buffer);
        logModule->postEvent(SH_EVENT_CMD_EXECUTE, &data);
    }

    // Вызов колбэка
    if (_onError) {
        _onError(buffer);
    }
}

void AppCore::logInfo(const char* module, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[128];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    Serial.printf("[%s] INFO: %s\n", module, buffer);

    // Отправка в LogManager (опционально)
    IModule* logModule = getModuleByName("LogManager");
    if (logModule) {
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = 0;
        data.targetModule = MODULE_ID_LOG;
        data.value = 2; // INFO level
        strncpy(data.payload, buffer, sizeof(data.payload) - 1);
        data.payloadLen = strlen(buffer);
        logModule->postEvent(SH_EVENT_CMD_EXECUTE, &data);
    }
}

void AppCore::logDebug(const char* module, const char* format, ...) {
    if (!_verboseLogging) return;

    va_list args;
    va_start(args, format);
    char buffer[128];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    Serial.printf("[%s] DEBUG: %s\n", module, buffer);
}