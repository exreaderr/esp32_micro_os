// ============================================================================
// LogManager.h - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Централизованная система логирования с поддержкой событий.
// Все логи публикуются в шину событий для подписки другими модулями.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - Добавлена очередь событий для обработчика (защита от дедлоков)
// - Добавлен метод getLogsAsString с передачей по ссылке (без фрагментации)
// - Добавлена потокобезопасная запись в файл с использованием std::move
// - Улучшена защита от переполнения буфера
// - Добавлены новые методы для управления логами
// - Улучшена документация всех методов
// ============================================================================
#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include <cstdarg>

// Только IModule, без AppCore!
#include "core/IModule.h"

// ============================================================================
// 1. УРОВНИ ЛОГИРОВАНИЯ
// ============================================================================
/**
 * @brief Уровни логирования
 *
 * Используются для фильтрации и цветового выделения логов.
 * LOG_DEBUG - отладочная информация (самый низкий уровень)
 * LOG_INFO - общая информация
 * LOG_WARNING - предупреждения
 * LOG_ERROR - ошибки
 * LOG_CRITICAL - критические ошибки
 * LOG_PAZ - события системы противоаварийной защиты
 * LOG_SECURITY - события безопасности
 * LOG_AUDIT - аудит (доступ, изменения)
 */
enum LogLevel : uint8_t {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3,
    LOG_CRITICAL = 4,
    LOG_PAZ = 5,        // Система ПАЗ
    LOG_SECURITY = 6,   // События безопасности
    LOG_AUDIT = 7       // Аудит (доступ, изменения)
};

// ============================================================================
// 2. СОБЫТИЯ LOG MANAGER
// ============================================================================
/**
 * @brief События LogManager для публикации в шину
 */
enum LogEvents : int32_t {
    SH_EVENT_LOG_ENTRY = SH_EVENT_USER_BASE + 0x0200,       // Новая запись лога
    SH_EVENT_LOG_FLUSHED = SH_EVENT_USER_BASE + 0x0201,     // Логи сброшены на диск
    SH_EVENT_LOG_CLEARED = SH_EVENT_USER_BASE + 0x0202,     // Логи очищены
    SH_EVENT_LOG_ERROR = SH_EVENT_USER_BASE + 0x0203,       // Ошибка в LogManager
    SH_EVENT_LOG_BUFFER_FULL = SH_EVENT_USER_BASE + 0x0204, // Буфер переполнен
    SH_EVENT_LOG_ROTATED = SH_EVENT_USER_BASE + 0x0205      // Лог-файл ротирован
};

// ============================================================================
// 3. СТРУКТУРЫ ДАННЫХ
// ============================================================================
/**
 * @brief Структура записи лога для событий (отправляется в шину)
 */
struct LogEventData {
    uint32_t timestamp;     // Реальное время (Unix timestamp)
    uint32_t uptime;        // Время с момента запуска (секунды)
    uint8_t level;          // Уровень логирования
    char source[16];        // Источник (модуль или тег)
    char message[128];      // Сообщение
    uint16_t messageLen;    // Длина сообщения
};

/**
 * @brief Структура записи лога (внутренняя, хранится в RAM)
 *
 * Использует фиксированные буферы вместо String для
 * предотвращения фрагментации кучи.
 */
struct LogEntry {
    uint32_t uptime_sec;    // Время с момента запуска (секунды)
    uint32_t real_time;     // Реальное время (Unix timestamp)
    LogLevel level;         // Уровень логирования
    char source[16];        // Источник (фиксированный буфер)
    char message[128];      // Сообщение (фиксированный буфер)
};

/**
 * @brief Статус LogManager
 *
 * Содержит всю информацию о состоянии системы логирования.
 */
struct LogStatus {
    size_t ramBufferSize = 0;       // Текущий размер RAM буфера
    size_t maxRamEntries = 50;      // Максимальный размер RAM буфера
    bool isDirty = false;           // Есть ли несохраненные логи
    uint32_t lastFlushTime = 0;     // Время последнего сброса на диск
    size_t totalLogsCount = 0;      // Общее количество логов за сессию
    size_t fileSize = 0;            // Размер файла логов
    size_t backupSize = 0;          // Размер файла бэкапа
    bool isInitialized = false;     // Инициализирован ли LogManager
    LogLevel minLevel = LOG_DEBUG;  // Минимальный уровень для записи
    uint32_t errors = 0;            // Количество ошибок
    uint32_t droppedLogs = 0;       // Количество потерянных логов
    uint32_t flushCount = 0;        // Количество сбросов на диск
    uint32_t totalBytesWritten = 0; // Всего записано байт на диск
};

// ============================================================================
// 4. ВСПОМОГАТЕЛЬНЫЕ МАКРОСЫ ДЛЯ ФОРМАТИРОВАННОГО ЛОГА
// ============================================================================
/**
 * @brief Вспомогательные макросы для форматированного логирования
 *
 * Используются внутри модулей:
 *   LOG_DEBUG("TAG", "Value: %d", value);
 *   LOG_INFO("TAG", "Started");
 *   LOG_ERROR("TAG", "Error code: %d", code);
 *
 * НЕ ИСПОЛЬЗУЮТ глобальный LOGGER!
 */
#define LOG_DEBUG(tag, msg, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), msg, ##__VA_ARGS__); \
        LogManager::getInstance().addLog(LOG_DEBUG, tag, _buf); \
    } while(0)

#define LOG_INFO(tag, msg, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), msg, ##__VA_ARGS__); \
        LogManager::getInstance().addLog(LOG_INFO, tag, _buf); \
    } while(0)

#define LOG_WARN(tag, msg, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), msg, ##__VA_ARGS__); \
        LogManager::getInstance().addLog(LOG_WARNING, tag, _buf); \
    } while(0)

#define LOG_ERROR(tag, msg, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), msg, ##__VA_ARGS__); \
        LogManager::getInstance().addLog(LOG_ERROR, tag, _buf); \
    } while(0)

#define LOG_CRIT(tag, msg, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), msg, ##__VA_ARGS__); \
        LogManager::getInstance().addLog(LOG_CRITICAL, tag, _buf); \
    } while(0)

#define LOG_PAZ(tag, msg, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), msg, ##__VA_ARGS__); \
        LogManager::getInstance().addLog(LOG_PAZ, tag, _buf); \
    } while(0)

#define LOG_SECURITY(tag, msg, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), msg, ##__VA_ARGS__); \
        LogManager::getInstance().addLog(LOG_SECURITY, tag, _buf); \
    } while(0)

#define LOG_AUDIT(tag, msg, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), msg, ##__VA_ARGS__); \
        LogManager::getInstance().addLog(LOG_AUDIT, tag, _buf); \
    } while(0)

// ============================================================================
// 5. ВСПОМОГАТЕЛЬНЫЕ МАКРОСЫ ДЛЯ УСЛОВНОГО ЛОГИРОВАНИЯ
// ============================================================================
#define LOG_DEBUG_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_DEBUG(tag, msg, ##__VA_ARGS__); } } while(0)

#define LOG_INFO_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_INFO(tag, msg, ##__VA_ARGS__); } } while(0)

#define LOG_WARN_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_WARN(tag, msg, ##__VA_ARGS__); } } while(0)

#define LOG_ERROR_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_ERROR(tag, msg, ##__VA_ARGS__); } } while(0)

// ============================================================================
// 6. ОСНОВНОЙ КЛАСС
// ============================================================================
/**
 * @brief Централизованная система логирования
 *
 * Синглтон. Обеспечивает:
 * - Буферизацию логов в RAM
 * - Асинхронную запись на диск (LittleFS)
 * - Ротацию лог-файлов
 * - Публикацию событий в шину
 * - ANSI-цвета для Serial
 * - Форматированные сообщения
 * - Колбэки для внешнего мониторинга
 */
class LogManager : public IModule {
public:
    // === ТИПЫ КОЛБЭКОВ ===
    typedef std::function<void(const LogEntry& entry)> OnLogCallback;
    typedef std::function<void(size_t count)> OnBufferFullCallback;
    typedef std::function<void(const LogStatus& status)> OnStatusChangeCallback;
    typedef std::function<void(const char* error)> OnErrorCallback;

    // === СИНГЛТОН ===
    static LogManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    LogManager();
    ~LogManager();

    // Запрещаем копирование
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "LogManager"; }
    const char* getVersion() const override { return "4.2.2"; }
    uint32_t getModuleId() const override { return MODULE_ID_LOG; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ДОБАВЛЕНИЕ ЛОГОВ ===
    /**
     * @brief Добавить запись в лог
     * @param level Уровень логирования
     * @param source Источник (тег модуля)
     * @param message Сообщение
     */
    void addLog(LogLevel level, const char* source, const char* message);
    void addLog(LogLevel level, const char* source, const String& message);
    void addLog(uint8_t rawLevel, const char* source, const char* message);

    /**
     * @brief Добавить запись в лог с условием
     * @param condition Условие (если true, лог добавляется)
     * @param level Уровень логирования
     * @param source Источник
     * @param message Сообщение
     */
    void addLogIf(bool condition, LogLevel level, const char* source, const char* message);

    /**
     * @brief Добавить форматированную запись в лог
     * @param level Уровень логирования
     * @param source Источник
     * @param format Строка формата (как printf)
     * @param ... Аргументы для форматирования
     */
    void addLogF(LogLevel level, const char* source, const char* format, ...)
        __attribute__((format(printf, 4, 5)));

    // === УПРАВЛЕНИЕ ===
    void scheduleFlush();                  // Запланировать сброс на диск
    void flushToDisk();                    // Немедленный сброс на диск
    void clearAll();                       // Очистить все логи
    void setMaxEntries(size_t maxEntries); // Установить максимальный размер буфера
    void setMinLevel(LogLevel level);      // Установить минимальный уровень
    void setAutoFlush(bool enable) { _autoFlush = enable; }
    void setFlushInterval(uint32_t ms) { _flushIntervalMs = ms; }

    // === ПОЛУЧЕНИЕ ЛОГОВ ===
    /**
     * @brief Получить логи в виде строки
     * @param limit Максимальное количество записей
     * @return Строка с логами (может вызвать фрагментацию!)
     *
     * ВНИМАНИЕ: При большом количестве логов (более 100) используйте
     * версию с передачей по ссылке для предотвращения фрагментации.
     */
    String getLogsAsString(size_t limit = 100);

    /**
     * @brief Получить логи в виде строки (без фрагментации)
     * @param limit Максимальное количество записей
     * @param output Ссылка на строку для вывода
     * @return true если успешно
     *
     * Рекомендуемый метод для получения логов.
     */
    bool getLogsAsString(size_t limit, String& output) const;

    std::vector<LogEntry> getRecentLogs(size_t count = 50) const;

    // === ГЕТТЕРЫ ===
    size_t getRamBufferSize() const;
    size_t getMaxRamEntries() const { return _maxRamEntries; }
    bool isInitialized() const { return _initialized; }
    bool isDirty() const { return _isDirty; }
    uint32_t getLastFlushTime() const { return _lastFlushTime; }
    size_t getTotalLogsCount() const { return _totalLogsCount; }
    size_t getFileSize() const;
    LogLevel getMinLevel() const { return _minLevel; }
    uint32_t getDroppedLogs() const { return _droppedLogs; }
    uint32_t getErrors() const { return _errors; }

    // === СТАТУС ===
    LogStatus getLogStatus() const;
    String getLogStatusString() const;

    // === СТАТИЧЕСКИЕ МЕТОДЫ ===
    static const char* levelToString(LogLevel level);
    static LogLevel stringToLevel(const char* str);
    static const char* levelToColor(LogLevel level);
    static const char* levelToAnsiColor(LogLevel level);

    // === КОЛБЭКИ ===
    void setCallback(OnLogCallback cb) { _onLogCallback = cb; }
    void setBufferFullCallback(OnBufferFullCallback cb) { _onBufferFull = cb; }
    void setStatusCallback(OnStatusChangeCallback cb) { _onStatusChange = cb; }
    void setErrorCallback(OnErrorCallback cb) { _onErrorCallback = cb; }

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    void checkLogRotation();
    bool formatToFile(File& logFile, const LogEntry& entry);
    void writeToSerial(const LogEntry& entry);
    void checkBufferFull();
    void printFormattedTime(char* buf, size_t bufSize, uint32_t uptime, uint32_t realTime) const;
    void updateStatus();
    void addInternal(LogLevel level, const char* source, const char* message);
    void trimSourceAndMessage(const char* source, const char* message,
                              char* trimmedSource, size_t srcSize,
                              char* trimmedMessage, size_t msgSize) const;
    bool ensureDirectoryExists();
    size_t getFileSize(const char* path) const;

    // === ОТПРАВКА СОБЫТИЙ ===
    void publishLogEvent(const LogEntry& entry);
    void publishLogFlushed();
    void publishLogCleared();
    void publishLogError(const char* error);
    void publishBufferFull(size_t size);
    void publishLogRotated();

    // === ОБРАБОТЧИКИ СОБЫТИЙ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ЛОГИРОВАНИЕ (для самого LogManager) ===
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);

    // === ДАННЫЕ ===
    std::vector<LogEntry> _ramBuffer;           // RAM буфер логов
    size_t _maxRamEntries = 50;                 // Максимальный размер буфера
    SemaphoreHandle_t _logMutex = nullptr;      // Рекурсивный мьютекс

    bool _isDirty = false;                      // Есть ли несохраненные логи
    uint32_t _dirtyTimestamp = 0;               // Время последнего изменения
    uint32_t _lastFlushTime = 0;                // Время последнего сброса
    size_t _totalLogsCount = 0;                 // Общее количество логов
    LogLevel _minLevel = LOG_DEBUG;             // Минимальный уровень
    bool _initialized = false;                  // Флаг инициализации
    uint32_t _errors = 0;                       // Количество ошибок
    uint32_t _droppedLogs = 0;                  // Количество потерянных логов
    uint32_t _flushCount = 0;                   // Количество сбросов
    uint32_t _totalBytesWritten = 0;            // Всего записано байт
    bool _autoFlush = true;                     // Автоматический сброс
    uint32_t _flushIntervalMs = 10000;          // Интервал сброса (мс)
    uint32_t _moduleId = MODULE_ID_LOG;

    // === КОЛБЭКИ ===
    OnLogCallback _onLogCallback = nullptr;
    OnBufferFullCallback _onBufferFull = nullptr;
    OnStatusChangeCallback _onStatusChange = nullptr;
    OnErrorCallback _onErrorCallback = nullptr;

    // === КОНСТАНТЫ ===
    static constexpr const char* _logPath = "/system.log";
    static constexpr const char* _backupPath = "/system.bak";
    static constexpr const char* _indexPath = "/system.idx";
    static constexpr size_t MAX_LOG_FILE_SIZE = 40960;      // 40 КБ
    static constexpr uint32_t FLUSH_DELAY_MS = 10000;       // 10 секунд
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;       // 500 мс
    static constexpr size_t MAX_SOURCE_LEN = 15;            // Максимальная длина источника
    static constexpr size_t MAX_MESSAGE_LEN = 127;          // Максимальная длина сообщения

    // ANSI-Цвета для Serial
    static constexpr const char* ANSI_RESET = "\033[0m";
    static constexpr const char* ANSI_RED = "\033[1;31m";
    static constexpr const char* ANSI_YELLOW = "\033[1;33m";
    static constexpr const char* ANSI_BLUE = "\033[1;34m";
    static constexpr const char* ANSI_GREEN = "\033[1;32m";
    static constexpr const char* ANSI_MAGENTA = "\033[1;35m";
    static constexpr const char* ANSI_CYAN = "\033[1;36m";
    static constexpr const char* ANSI_WHITE = "\033[1;37m";
    static constexpr const char* ANSI_GRAY = "\033[0;37m";

    static constexpr const char* TIME_FORMAT = "%Y-%m-%d %H:%M:%S";
};
