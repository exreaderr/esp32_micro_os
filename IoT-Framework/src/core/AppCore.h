// ============================================================================
// AppCore.h - ЯДРО СИСТЕМЫ МикроОС v4.2.2
// ============================================================================
// Описание:
// Оркестратор системы. Управляет жизненным циклом всех модулей,
// обрабатывает события, контролирует здоровье системы.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - Добавлен метод postEventFromISR для безопасной отправки из прерываний
// - Добавлен метод logError для унифицированного логирования ошибок
// - Добавлена поддержка thread_local для отслеживания рекурсии
// - Увеличен стек задач до 6144 байт
// - Добавлен метод getModuleById для быстрого доступа
// - Улучшена документация всех методов
// ============================================================================
#pragma once

#include <Arduino.h>
#include <vector>
#include <map>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_event.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include "core/IModule.h"

// ============================================================================
// 1. ОБЪЯВЛЕНИЕ ШИН СОБЫТИЙ
// ============================================================================
ESP_EVENT_DECLARE_BASE(SH_SYS_EVENTS);
ESP_EVENT_DECLARE_BASE(SH_APP_EVENTS);

// ============================================================================
// 2. СТРУКТУРА СТАТИСТИКИ СИСТЕМЫ
// ============================================================================
/**
 * @brief Статистика работы системы
 *
 * Содержит информацию о состоянии системы,
 * используемой памяти, количестве событий и ошибок.
 */
struct ShSystemStats {
    uint32_t uptime;            // Время работы в секундах
    uint32_t freeHeap;          // Свободная память кучи
    uint32_t freePsram;         // Свободная PSRAM (если есть)
    uint32_t totalRestarts;     // Количество перезагрузок
    uint32_t errors;            // Количество ошибок
    uint32_t modulesCount;      // Количество зарегистрированных модулей
    uint32_t eventsProcessed;   // Всего обработанных событий
    uint32_t heapMinFree;       // Минимальный уровень свободной памяти
    uint8_t healthStatus;       // Статус здоровья (0=OK, 1=WARNING, 2=CRITICAL)
    float cpuFreq;              // Частота CPU в МГц
    float temperature;          // Температура чипа
    uint32_t queueOverflows;    // Потерянные события из-за переполнения очереди
    uint32_t wdtResets;         // Сбросы по WDT
};

// ============================================================================
// 3. СТРУКТУРА ДЛЯ ОЧЕРЕДИ СОБЫТИЙ
// ============================================================================
/**
 * @brief Структура события в очереди
 *
 * Используется для буферизации событий между
 * обработчиком и диспетчером.
 */
struct QueuedEvent {
    int32_t eventId;            // ID события
    ShEventData data;           // Данные события
    bool hasData;               // Флаг наличия данных
};

// ============================================================================
// 4. ИНФОРМАЦИЯ О МОДУЛЕ С ПРИОРИТЕТОМ
// ============================================================================
/**
 * @brief Информация о зарегистрированном модуле
 *
 * Содержит указатель на модуль, его приоритет,
 * интервал вызова tick() и состояние.
 */
struct ModuleInfo {
    IModule* module;            // Указатель на модуль
    uint8_t priority;           // 0 = высший, 10 = низший
    uint32_t lastTickMs;        // Время последнего вызова tick()
    uint32_t tickInterval;      // Интервал вызова tick() в мс
    bool enabled;               // Включен ли модуль
};

// ============================================================================
// 5. КЛАСС ЯДРА СИСТЕМЫ
// ============================================================================
/**
 * @brief Главный оркестратор системы
 *
 * Синглтон. Управляет всеми модулями, событиями,
 * здоровьем системы и watchdog.
 */
class AppCore {
public:
    // === СИНГЛТОН ===
    /**
     * @brief Получить единственный экземпляр ядра
     * @return Ссылка на экземпляр AppCore
     */
    static AppCore& getInstance();

    // Запрещаем копирование
    AppCore(const AppCore&) = delete;
    AppCore& operator=(const AppCore&) = delete;

    // === ЖИЗНЕННЫЙ ЦИКЛ ===
    /**
     * @brief Запуск системы
     *
     * Инициализирует событийную шину, watchdog,
     * регистрирует и запускает все модули.
     * Должна быть вызвана один раз в setup().
     */
    void begin();

    /**
     * @brief Главный цикл системы
     *
     * Должна быть вызвана в loop().
     * Обрабатывает фоновые задачи, обновляет статистику,
     * сбрасывает watchdog.
     */
    void loop();

    /**
     * @brief Перезагрузка системы
     *
     * Корректно останавливает модули и перезагружает ESP32.
     */
    void restart();

    /**
     * @brief Выключение системы
     *
     * Корректно останавливает все модули и переходит в спящий режим.
     */
    void shutdown();

    // === РЕГИСТРАЦИЯ МОДУЛЕЙ ===
    /**
     * @brief Зарегистрировать модуль в системе
     * @param module Указатель на модуль
     * @param priority Приоритет (0 = высший, 10 = низший)
     * @param tickInterval Интервал вызова tick() в мс
     *
     * Модули должны быть зарегистрированы до вызова begin().
     */
    void registerModule(IModule* module, uint8_t priority = 5, uint32_t tickInterval = 50);

    /**
     * @brief Зарегистрировать список модулей
     * @param modules Вектор указателей на модули
     */
    void registerModules(const std::vector<IModule*>& modules);

    /**
     * @brief Получить модуль по имени
     * @param name Имя модуля
     * @return Указатель на модуль или nullptr
     */
    IModule* getModuleByName(const char* name) const;

    /**
     * @brief Получить модуль по ID
     * @param moduleId ID модуля
     * @return Указатель на модуль или nullptr
     */
    IModule* getModuleById(uint32_t moduleId) const;

    /**
     * @brief Получить модуль по типу (шаблон)
     * @return Указатель на модуль или nullptr
     */
    template<typename T>
    T* getModule() const;

    // === ОТПРАВКА СОБЫТИЙ ===
    /**
     * @brief Отправить событие в систему
     * @param eventId ID события
     * @param data Данные события (может быть nullptr)
     *
     * Безопасная отправка. Автоматически определяет шину
     * (системную или прикладную) по ID события.
     */
    void postEvent(int32_t eventId, const ShEventData* data = nullptr);

    /**
     * @brief Отправить событие из ISR
     * @param eventId ID события
     * @param data Данные события (может быть nullptr)
     * @param higherPriorityTaskWoken Флаг для yield
     *
     * Специальный метод для отправки из прерываний.
     * Использует xQueueSendFromISR.
     */
    void postEventFromISR(int32_t eventId, const ShEventData* data,
                          BaseType_t* higherPriorityTaskWoken);

    /**
     * @brief Отправить команду модулю
     * @param targetModule ID целевого модуля
     * @param command Код команды
     * @param value Числовое значение
     * @param payload Строковые данные (может быть nullptr)
     */
    void postCommand(uint32_t targetModule, uint32_t command,
                    int32_t value = 0, const char* payload = nullptr);

    /**
     * @brief Отправить событие конкретному модулю
     * @param moduleId ID модуля
     * @param eventId ID события
     * @param data Данные события (может быть nullptr)
     */
    void postEventToModule(uint32_t moduleId, int32_t eventId, const ShEventData* data = nullptr);

    // === СТАТУС ===
    bool isReady() const { return _ready; }
    bool isShuttingDown() const { return _shuttingDown; }
    const ShSystemStats& getStats() const { return _stats; }
    uint32_t getUptime() const { return _uptime; }
    uint32_t getRestartCount() const { return _restartCount; }
    const char* getVersion() const { return _version; }

    // === УПРАВЛЕНИЕ ===
    void emergencyStop();
    void feedWatchdog();
    void setWdtTimeout(uint32_t ms) { _wdtTimeoutMs = ms; }
    void setEventQueueSize(size_t size) { _eventQueueSize = size; }
    void setHealthCheckInterval(uint32_t ms) { _healthCheckIntervalMs = ms; }
    void enableVerboseLogging(bool enable) { _verboseLogging = enable; }

    // === ЛОГИРОВАНИЕ ===
    void logError(const char* module, const char* format, ...);
    void logError(const char* module, const char* format, va_list args);
    void logInfo(const char* module, const char* format, ...);
    void logDebug(const char* module, const char* format, ...);

    // === КОЛБЭКИ ДЛЯ ВНЕШНЕГО МОНИТОРИНГА ===
    void setOnError(std::function<void(const char* error)> callback) { _onError = callback; }
    void setOnHealthChange(std::function<void(uint8_t newStatus)> callback) { _onHealthChange = callback; }

private:
    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    AppCore();
    ~AppCore();

    // === ВНУТРЕННИЕ МЕТОДЫ ===
    void initEventLoop();
    void initModules();
    void startModules();
    void processEventQueue();
    void updateStats();
    void checkHealth();
    void dispatchEvent(int32_t eventId, const ShEventData* data);
    void dispatchToModule(IModule* module, int32_t eventId, const ShEventData* data);
    void initHardwareWatchdog();
    void resetHardwareWatchdog();
    void processModuleTick();

    // === ОБРАБОТЧИКИ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    static void healthTask(void* parameters);
    static void eventProcessorTask(void* parameters);
    static void IRAM_ATTR hwWatchdogISR();

    // === ДАННЫЕ ===
    bool _ready = false;
    bool _shuttingDown = false;
    bool _verboseLogging = false;

    // Модули с приоритетами
    std::vector<ModuleInfo> _modules;
    SemaphoreHandle_t _modulesMutex = nullptr;  // Рекурсивный!

    // Очередь событий
    QueueHandle_t _eventQueue = nullptr;
    size_t _eventQueueSize = 32;

    // Статистика
    ShSystemStats _stats;
    SemaphoreHandle_t _statsMutex = nullptr;  // Рекурсивный!

    // Таймеры
    uint32_t _startTime = 0;
    uint32_t _uptime = 0;
    uint32_t _eventCounter = 0;
    uint32_t _restartCount = 0;
    uint32_t _lastHealthCheck = 0;
    uint32_t _lastStatsUpdate = 0;
    uint32_t _healthCheckIntervalMs = 10000;

    // Задачи FreeRTOS
    TaskHandle_t _healthTaskHandle = nullptr;
    TaskHandle_t _eventProcessorHandle = nullptr;

    // Watchdog
    bool _wdtEnabled = true;
    uint32_t _wdtTimeoutMs = 8000;
    hw_timer_t* _hwWdtTimer = nullptr;  // Аппаратный WDT

    // Версия
    const char* _version = "4.2.2";

    // Колбэки для внешнего мониторинга
    std::function<void(const char* error)> _onError = nullptr;
    std::function<void(uint8_t newStatus)> _onHealthChange = nullptr;

    // Константы
    static constexpr uint32_t MAX_EVENTS_PER_TICK = 20;  // Увеличено с 10
    static constexpr uint32_t WDT_FEED_INTERVAL_MS = 5000;
    static constexpr uint32_t MAX_RECURSION_DEPTH = 3;
};

// ============================================================================
// 6. ШАБЛОННЫЙ МЕТОД ДЛЯ ПОЛУЧЕНИЯ МОДУЛЯ ПО ТИПУ
// ============================================================================
template<typename T>
T* AppCore::getModule() const {
    if (_modulesMutex == nullptr) return nullptr;

    T* found = nullptr;
    if (xSemaphoreTakeRecursive(_modulesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (const auto& info : _modules) {
            T* casted = dynamic_cast<T*>(info.module);
            if (casted != nullptr) {
                found = casted;
                break;
            }
        }
        xSemaphoreGiveRecursive(_modulesMutex);
    }
    return found;
}
