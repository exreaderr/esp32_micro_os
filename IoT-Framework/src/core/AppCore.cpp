// ============================================================================
// AppCore.cpp — МикроОС v5.0
// ============================================================================
#include "AppCore.h"
#include <esp_timer.h>
#include <esp_task_wdt.h>

// -----------------------------------------------------------------------------
// СИНГЛТОН
// -----------------------------------------------------------------------------
AppCore& AppCore::getInstance() {
    static AppCore instance;
    return instance;
}

AppCore::~AppCore() {
    stop();
    deinitEventBus();
    if (_modulesMutex) vSemaphoreDelete(_modulesMutex);
    if (_eventMutex) vSemaphoreDelete(_eventMutex);
    if (_payloadMutex) vSemaphoreDelete(_payloadMutex);
}

// -----------------------------------------------------------------------------
// ИНИЦИАЛИЗАЦИЯ
// -----------------------------------------------------------------------------
bool AppCore::init(const SystemConfig& config) {
    if (_initialized) {
        return true;
    }

    _config = config;

    // Создание мьютексов
    _modulesMutex = xSemaphoreCreateRecursiveMutex();
    _eventMutex = xSemaphoreCreateRecursiveMutex();
    _payloadMutex = xSemaphoreCreateRecursiveMutex();

    if (!_modulesMutex || !_eventMutex || !_payloadMutex) {
        return false;
    }

    // Инициализация шины событий
    if (!initEventBus()) {
        return false;
    }

    // Установка callback'ов для IModule
    IModule::_eventPoster = [this](int32_t eventId, const ShEventData* data, bool isISR) {
        return this->postEvent(eventId, data);
    };
    IModule::_eventPosterISR = [this](int32_t eventId, const ShEventData* data, BaseType_t* hpw) {
        return this->postEventFromISR(eventId, data, hpw);
    };

    // WDT
    if (!initWDT()) {
        return false;
    }

    _initialized = true;
    return true;
}

// -----------------------------------------------------------------------------
// ШИНА СОБЫТИЙ (только esp_event, без двойной буферизации)
// -----------------------------------------------------------------------------
bool AppCore::initEventBus() {
    _eventLoopArgs.queue_size = _config.maxEventQueueSize;
    _eventLoopArgs.task_name = "event_task";
    _eventLoopArgs.task_priority = 5;
    _eventLoopArgs.task_stack_size = 4096;
    _eventLoopArgs.task_core_id = 1;  // На Core 1 (App Core)

    esp_err_t err = esp_event_loop_create(&_eventLoopArgs, &_eventLoop);
    if (err != ESP_OK) {
        return false;
    }

    // Регистрация базовых событий
    err = esp_event_handler_instance_register_with(
        _eventLoop,
        SH_SYS_EVENTS, ESP_EVENT_ANY_ID,
        eventHandler, this, nullptr
    );
    if (err != ESP_OK) {
        return false;
    }

    err = esp_event_handler_instance_register_with(
        _eventLoop,
        SH_APP_EVENTS, ESP_EVENT_ANY_ID,
        eventHandler, this, nullptr
    );
    if (err != ESP_OK) {
        return false;
    }

    return true;
}

void AppCore::deinitEventBus() {
    if (_eventLoop) {
        esp_event_loop_delete(_eventLoop);
        _eventLoop = nullptr;
    }
}

// -----------------------------------------------------------------------------
// РЕГИСТРАЦИЯ МОДУЛЕЙ
// -----------------------------------------------------------------------------
bool AppCore::registerModule(IModule* module, uint32_t moduleId,
                              const char* name, uint8_t priority) {
    if (!module || !name || !_modulesMutex) {
        return false;
    }

    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    // Проверка на дубликат
    for (const auto& info : _modules) {
        if (info.moduleId == moduleId) {
            xSemaphoreGiveRecursive(_modulesMutex);
            return false;
        }
    }

    ModuleInfo info;
    info.module = module;
    info.moduleId = moduleId;
    info.priority = priority;
    info.tickIntervalMs = module->getTickIntervalMs();
    IModule::safeStrCopy(info.name, sizeof(info.name), name);

    _modules.push_back(info);
    sortModulesByPriority();

    xSemaphoreGiveRecursive(_modulesMutex);
    return true;
}

bool AppCore::registerApp(IDeviceApp* app) {
    if (!app || _app) {
        return false;
    }
    _app = app;
    return true;
}

// -----------------------------------------------------------------------------
// ЖИЗНЕННЫЙ ЦИКЛ
// -----------------------------------------------------------------------------
bool AppCore::start() {
    if (!_initialized || _started) {
        return false;
    }

    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return false;
    }

    // Инициализация модулей
    for (auto& info : _modules) {
        if (!info.initialized) {
            info.module->init();
            info.initialized = true;
        }
    }

    // Инициализация приложения
    if (_app) {
        ShEventData config;
        _app->init(&config);
    }

    // Старт модулей
    for (auto& info : _modules) {
        if (!info.started) {
            info.module->start();
            info.started = true;
        }
    }

    _started = true;
    _ready = true;

    // Событие готовности
    ShEventData readyEvent;
    readyEvent.sourceModule = 0;
    readyEvent.command = SH_EVENT_SYS_READY;
    postEvent(SH_EVENT_SYS_READY, &readyEvent);

    xSemaphoreGiveRecursive(_modulesMutex);
    return true;
}

void AppCore::stop() {
    if (!_started) return;

    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        // Остановка в обратном порядке
        for (auto it = _modules.rbegin(); it != _modules.rend(); ++it) {
            if (it->started) {
                it->module->stop();
                it->started = false;
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }

    _started = false;
    _ready = false;
}

void AppCore::tick() {
    if (!_started) return;

    uint32_t now = millis();

    // WDT
    feedWDT();

    // Health check
    static uint32_t lastHealthCheck = 0;
    if (now - lastHealthCheck >= _config.healthCheckIntervalMs) {
        lastHealthCheck = now;
        checkModuleHealth();
    }

    // Tick модулей
    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (auto& info : _modules) {
            if (info.started && (now - info.lastTickMs >= info.tickIntervalMs)) {
                info.lastTickMs = now;
                xSemaphoreGiveRecursive(_modulesMutex);

                info.module->tick();

                if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                    break;
                }
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }

    // Tick приложения
    if (_app) {
        _app->tick();
    }
}

// -----------------------------------------------------------------------------
// ОТПРАВКА СОБЫТИЙ
// -----------------------------------------------------------------------------
bool AppCore::postEvent(int32_t eventId, const ShEventData* data) {
    if (!_eventLoop || !data) {
        return false;
    }

    // Копирование данных для безопасности
    ShEventData eventData = *data;
    eventData.timestamp = millis();

    esp_err_t err = esp_event_post_to(
        _eventLoop,
        (eventId < 0x0100) ? SH_SYS_EVENTS : SH_APP_EVENTS,
        eventId,
        &eventData,
        sizeof(eventData),
        pdMS_TO_TICKS(100)
    );

    return (err == ESP_OK);
}

bool AppCore::postEventFromISR(int32_t eventId, const ShEventData* data,
                                BaseType_t* higherPriorityTaskWoken) {
    if (!_eventLoop || !data) {
        return false;
    }

    ShEventData eventData = *data;
    eventData.timestamp = millis();

    esp_err_t err = esp_event_isr_post_to(
        _eventLoop,
        (eventId < 0x0100) ? SH_SYS_EVENTS : SH_APP_EVENTS,
        eventId,
        &eventData,
        sizeof(eventData),
        higherPriorityTaskWoken
    );

    return (err == ESP_OK);
}

// -----------------------------------------------------------------------------
// ОБРАБОТЧИКИ СОБЫТИЙ
// -----------------------------------------------------------------------------
void AppCore::eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData) {
    AppCore* instance = static_cast<AppCore*>(handlerArgs);
    if (!instance) return;

    ShEventData* data = static_cast<ShEventData*>(eventData);
    if (!data) return;

    instance->dispatchEvent(id, data);
}

void AppCore::dispatchEvent(int32_t eventId, const ShEventData* data) {
    // Отправка подписчикам
    if (xSemaphoreTakeRecursive(_eventMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& sub : _subscriptions) {
            if (sub.eventId == eventId || sub.eventId == ESP_EVENT_ANY_ID) {
                // Проверка targetModule
                if (data->targetModule != 0 && data->targetModule != sub.moduleId) {
                    continue;
                }

                if (sub.isApp && _app) {
                    xSemaphoreGiveRecursive(_eventMutex);
                    _app->onEvent(eventId, data);
                    if (xSemaphoreTakeRecursive(_eventMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                        break;
                    }
                } else {
                    for (auto& info : _modules) {
                        if (info.moduleId == sub.moduleId && info.started) {
                            xSemaphoreGiveRecursive(_eventMutex);
                            info.module->onEvent(eventId, data);
                            if (xSemaphoreTakeRecursive(_eventMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                                break;
                            }
                            break;
                        }
                    }
                }
            }
        }
        xSemaphoreGiveRecursive(_eventMutex);
    }
}

// -----------------------------------------------------------------------------
// ПОДПИСКА НА СОБЫТИЯ
// -----------------------------------------------------------------------------
bool AppCore::subscribe(int32_t eventId, IModule* subscriber) {
    if (!subscriber || !_eventMutex) return false;

    if (xSemaphoreTakeRecursive(_eventMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    Subscription sub;
    sub.eventId = eventId;
    sub.moduleId = subscriber->getModuleId();
    sub.isApp = false;
    _subscriptions.push_back(sub);

    xSemaphoreGiveRecursive(_eventMutex);
    return true;
}

bool AppCore::subscribe(int32_t eventId, IDeviceApp* subscriber) {
    if (!subscriber || !_eventMutex || !_app) return false;

    if (xSemaphoreTakeRecursive(_eventMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    Subscription sub;
    sub.eventId = eventId;
    sub.moduleId = MODULE_ID_DEVICE_APP;
    sub.isApp = true;
    _subscriptions.push_back(sub);

    xSemaphoreGiveRecursive(_eventMutex);
    return true;
}

bool AppCore::unsubscribe(int32_t eventId, IModule* subscriber) {
    if (!subscriber || !_eventMutex) return false;

    if (xSemaphoreTakeRecursive(_eventMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    uint32_t moduleId = subscriber->getModuleId();
    _subscriptions.erase(
        std::remove_if(_subscriptions.begin(), _subscriptions.end(),
            [eventId, moduleId](const Subscription& s) {
                return s.eventId == eventId && s.moduleId == moduleId && !s.isApp;
            }),
        _subscriptions.end()
    );

    xSemaphoreGiveRecursive(_eventMutex);
    return true;
}

// -----------------------------------------------------------------------------
// КАСТОМНЫЕ PAYLOAD'Ы
// -----------------------------------------------------------------------------
void AppCore::registerPayloadType(int32_t typeId,
                                   std::function<ICustomEventPayload*()> factory) {
    if (!factory || !_payloadMutex) return;

    if (xSemaphoreTakeRecursive(_payloadMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Проверка на дубликат
        for (const auto& f : _payloadFactories) {
            if (f.typeId == typeId) {
                xSemaphoreGiveRecursive(_payloadMutex);
                return;
            }
        }

        PayloadFactory pf;
        pf.typeId = typeId;
        pf.factory = factory;
        _payloadFactories.push_back(pf);

        xSemaphoreGiveRecursive(_payloadMutex);
    }
}

ICustomEventPayload* AppCore::createPayload(int32_t typeId) {
    if (!_payloadMutex) return nullptr;

    ICustomEventPayload* result = nullptr;

    if (xSemaphoreTakeRecursive(_payloadMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& f : _payloadFactories) {
            if (f.typeId == typeId) {
                result = f.factory();
                break;
            }
        }
        xSemaphoreGiveRecursive(_payloadMutex);
    }

    return result;
}

// -----------------------------------------------------------------------------
// WDT
// -----------------------------------------------------------------------------
bool AppCore::initWDT() {
    // ESP32 Core v3.x API
    _hwWdtTimer = timerBegin(1000000);  // 1 MHz
    if (!_hwWdtTimer) {
        return false;
    }

    timerAttachInterrupt(_hwWdtTimer, hwWatchdogISR);
    timerAlarm(_hwWdtTimer, _wdtTimeoutMs * 1000, false, 0);
    timerStart(_hwWdtTimer);

    return true;
}

void AppCore::feedWDT() {
    if (_hwWdtTimer) {
        timerWrite(_hwWdtTimer, 0);
    }
    _lastWdtFeedMs = millis();
}

void IRAM_ATTR AppCore::hwWatchdogISR() {
    // Перезагрузка системы
    esp_restart();
}

// -----------------------------------------------------------------------------
// HEALTH MONITOR
// -----------------------------------------------------------------------------
void AppCore::checkModuleHealth() {
    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    uint32_t now = millis();

    for (auto& info : _modules) {
        if (!info.started) continue;

        bool healthy = info.module->isHealthy();
        if (!healthy && info.healthy) {
            // Модуль стал нездоровым
            info.healthy = false;
            info.errorCount++;
            info.lastErrorMs = now;

            ShEventData alert;
            alert.sourceModule = info.moduleId;
            alert.command = SH_EVENT_HEALTH_CRITICAL;
            IModule::safeStrCopy(alert.payload, sizeof(alert.payload), info.name);
            postEvent(SH_EVENT_HEALTH_CRITICAL, &alert);

            // Автоперезапуск если включено
            if (_config.enableAutoRestart && info.errorCount < 5) {
                info.module->stop();
                delay(100);
                info.module->init();
                info.module->start();
                info.healthy = true;
            }
        } else if (healthy && !info.healthy) {
            // Модуль восстановился
            info.healthy = true;

            ShEventData alert;
            alert.sourceModule = info.moduleId;
            alert.command = SH_EVENT_HEALTH_OK;
            postEvent(SH_EVENT_HEALTH_OK, &alert);
        }
    }

    xSemaphoreGiveRecursive(_modulesMutex);
}

bool AppCore::isSystemHealthy() const {
    if (xSemaphoreTakeRecursive(const_cast<SemaphoreHandle_t&>(_modulesMutex),
                                pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    bool healthy = true;
    for (const auto& info : _modules) {
        if (info.started && !info.healthy) {
            healthy = false;
            break;
        }
    }

    xSemaphoreGiveRecursive(const_cast<SemaphoreHandle_t&>(_modulesMutex));
    return healthy;
}

void AppCore::restartModule(uint32_t moduleId) {
    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    for (auto& info : _modules) {
        if (info.moduleId == moduleId && info.started) {
            info.module->stop();
            delay(100);
            info.module->init();
            info.module->start();
            info.healthy = true;
            info.errorCount = 0;
            break;
        }
    }

    xSemaphoreGiveRecursive(_modulesMutex);
}

// -----------------------------------------------------------------------------
// СТАТУС СИСТЕМЫ
// -----------------------------------------------------------------------------
void AppCore::getSystemStatus(char* buffer, size_t bufferSize) const {
    if (!buffer || bufferSize == 0) return;

    buffer[0] = '\0';

    char temp[256];
    snprintf(temp, sizeof(temp),
        "=== MicroOS v5.0 System Status ===\n"
        "Modules: %zu\n"
        "Ready: %s\n"
        "Healthy: %s\n"
        "Uptime: %lu sec\n"
        "Free heap: %lu bytes\n",
        getModuleCount(),
        _ready ? "YES" : "NO",
        isSystemHealthy() ? "YES" : "NO",
        millis() / 1000,
        ESP.getFreeHeap()
    );
    IModule::safeStrCopy(buffer, bufferSize, temp);

    if (xSemaphoreTakeRecursive(const_cast<SemaphoreHandle_t&>(_modulesMutex),
                                pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& info : _modules) {
            char modStatus[128];
            info.module->getStatus(modStatus, sizeof(modStatus));

            snprintf(temp, sizeof(temp),
                "\n--- %s (ID=%lu, Prio=%u) ---\n"
                "  State: %s\n"
                "  Healthy: %s\n"
                "  Errors: %lu\n"
                "  %s\n",
                info.name, info.moduleId, info.priority,
                info.started ? "RUNNING" : "STOPPED",
                info.healthy ? "YES" : "NO",
                info.errorCount,
                modStatus
            );
            IModule::safeStrCat(buffer, bufferSize, temp);
        }
        xSemaphoreGiveRecursive(const_cast<SemaphoreHandle_t&>(_modulesMutex));
    }
}

size_t AppCore::getModuleCount() const {
    return _modules.size();
}

const ModuleInfo* AppCore::getModuleInfo(size_t index) const {
    if (index >= _modules.size()) return nullptr;
    return &_modules[index];
}

// -----------------------------------------------------------------------------
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// -----------------------------------------------------------------------------
void AppCore::sortModulesByPriority() {
    std::sort(_modules.begin(), _modules.end(),
        [](const ModuleInfo& a, const ModuleInfo& b) {
            return a.priority < b.priority;
        });
}

bool AppCore::takeMutex(TickType_t timeout) {
    return (_modulesMutex &&
            xSemaphoreTakeRecursive(_modulesMutex, timeout) == pdTRUE);
}

void AppCore::giveMutex() {
    if (_modulesMutex) {
        xSemaphoreGiveRecursive(_modulesMutex);
    }
}