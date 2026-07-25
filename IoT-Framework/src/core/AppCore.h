// ============================================================================
// AppCore.h — МикроОС v5.0
// Оркестратор системы с расширяемой шиной событий
// ============================================================================
#pragma once

#include "IModule.h"
#include <vector>
#include <algorithm>
#include <atomic>

// -----------------------------------------------------------------------------
// ИНФОРМАЦИЯ О МОДУЛЕ
// -----------------------------------------------------------------------------
struct ModuleInfo {
    IModule* module = nullptr;
    uint32_t moduleId = 0;
    char name[MODULE_NAME_MAX_LEN] = {0};
    uint8_t priority = 128;
    uint32_t tickIntervalMs = 1000;
    uint32_t lastTickMs = 0;
    bool initialized = false;
    bool started = false;
    bool healthy = true;
    uint32_t errorCount = 0;
    uint32_t lastErrorMs = 0;
};

// -----------------------------------------------------------------------------
// КОНФИГУРАЦИЯ СИСТЕМЫ
// -----------------------------------------------------------------------------
struct SystemConfig {
    uint32_t wdtTimeoutMs = 30000;
    uint32_t healthCheckIntervalMs = 60000;
    uint32_t maxEventQueueSize = 64;
    uint32_t maxModules = 32;
    bool enableHealthMonitor = true;
    bool enableAutoRestart = true;
};

// -----------------------------------------------------------------------------
// ИНТЕРФЕЙС ПРИЛОЖЕНИЯ УСТРОЙСТВА (бизнес-логика)
// -----------------------------------------------------------------------------
class IDeviceApp {
public:
    virtual void init(const ShEventData* config) = 0;
    virtual void tick() = 0;
    virtual void onEvent(int32_t eventId, const ShEventData* data) = 0;
    virtual ~IDeviceApp() = default;
};

// -----------------------------------------------------------------------------
// ORCHESTRATOR
// -----------------------------------------------------------------------------
class AppCore {
public:
    // Синглтон
    static AppCore& getInstance();

    // Запрет копирования
    AppCore(const AppCore&) = delete;
    AppCore& operator=(const AppCore&) = delete;

    // Инициализация системы
    bool init(const SystemConfig& config = SystemConfig{});

    // Регистрация модулей ядра
    bool registerModule(IModule* module, uint32_t moduleId,
                        const char* name, uint8_t priority = 128);

    // Регистрация бизнес-логики устройства
    bool registerApp(IDeviceApp* app);

    // Жизненный цикл
    bool start();
    void stop();
    void tick();  // Вызывать в loop()

    // Событийная шина
    bool postEvent(int32_t eventId, const ShEventData* data);
    bool postEventFromISR(int32_t eventId, const ShEventData* data,
                          BaseType_t* higherPriorityTaskWoken);

    // Подписка на события (для модулей)
    bool subscribe(int32_t eventId, IModule* subscriber);
    bool subscribe(int32_t eventId, IDeviceApp* subscriber);
    bool unsubscribe(int32_t eventId, IModule* subscriber);

    // Кастомные payload'ы
    void registerPayloadType(int32_t typeId,
                             std::function<ICustomEventPayload*()> factory);
    ICustomEventPayload* createPayload(int32_t typeId);

    // Статус системы
    void getSystemStatus(char* buffer, size_t bufferSize) const;
    size_t getModuleCount() const;
    const ModuleInfo* getModuleInfo(size_t index) const;

    // Health check
    bool isSystemHealthy() const;
    void restartModule(uint32_t moduleId);

    // Готовность
    bool isReady() const { return _ready; }

private:
    AppCore() = default;
    ~AppCore();

    // Инициализация шины событий
    bool initEventBus();
    void deinitEventBus();

    // Обработчики событий
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    static void eventHandlerISR(void* handlerArgs, esp_event_base_t base,
                                int32_t id, void* eventData);

    // Диспетчеризация событий
    void dispatchEvent(int32_t eventId, const ShEventData* data);

    // WDT
    bool initWDT();
    void feedWDT();
    static void hwWatchdogISR();

    // Health monitor
    void checkModuleHealth();

    // Сортировка модулей по приоритету
    void sortModulesByPriority();

    // Потокобезопасность
    bool takeMutex(TickType_t timeout = portMAX_DELAY);
    void giveMutex();

    // Поля
    std::vector<ModuleInfo> _modules;
    IDeviceApp* _app = nullptr;

    esp_event_loop_handle_t _eventLoop = nullptr;
    esp_event_loop_args_t _eventLoopArgs{};

    hw_timer_t* _hwWdtTimer = nullptr;
    uint32_t _wdtTimeoutMs = 30000;
    uint32_t _lastWdtFeedMs = 0;

    SemaphoreHandle_t _modulesMutex = nullptr;
    SemaphoreHandle_t _eventMutex = nullptr;

    SystemConfig _config;
    bool _initialized = false;
    bool _started = false;
    bool _ready = false;
    bool _panicMode = false;

    // Кастомные payload'ы
    struct PayloadFactory {
        int32_t typeId;
        std::function<ICustomEventPayload*()> factory;
    };
    std::vector<PayloadFactory> _payloadFactories;
    SemaphoreHandle_t _payloadMutex = nullptr;

    // Подписчики (eventId -> список moduleId)
    struct Subscription {
        int32_t eventId;
        uint32_t moduleId;
        bool isApp;  // true = IDeviceApp, false = IModule
    };
    std::vector<Subscription> _subscriptions;
};