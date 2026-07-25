// ============================================================================
// LogManager.h — МикроОС v5.0
// Единый центр логирования системы
// Потокобезопасный, ISR-безопасный, расширяемый
// ============================================================================
#pragma once

#include "core/IModule.h"
#include <array>
#include <functional>

// -----------------------------------------------------------------------------
// КОНФИГУРАЦИЯ ЛОГИРОВАНИЯ (compile-time)
// -----------------------------------------------------------------------------
#ifndef LOG_RAM_BUFFER_SIZE
    #define LOG_RAM_BUFFER_SIZE 256     // Максимум записей в RAM
#endif

#ifndef LOG_MAX_MESSAGE_LEN
    #define LOG_MAX_MESSAGE_LEN 128     // Максимальная длина сообщения
#endif

#ifndef LOG_MAX_TAG_LEN
    #define LOG_MAX_TAG_LEN 16          // Максимальная длина тега
#endif

#ifndef LOG_FILE_MAX_SIZE
    #define LOG_FILE_MAX_SIZE (256 * 1024)  // 256 KB max log file
#endif

#ifndef LOG_FLUSH_INTERVAL_MS
    #define LOG_FLUSH_INTERVAL_MS 30000      // Flush to disk every 30 sec
#endif

// -----------------------------------------------------------------------------
// СТРУКТУРА ЗАПИСИ ЛОГА
// -----------------------------------------------------------------------------
struct LogEntry {
    uint32_t timestamp = 0;              // UNIX timestamp
    LogLevel level = LOG_INFO;           // Уровень логирования
    uint32_t sourceModule = 0;           // ID модуля-источника
    char tag[LOG_MAX_TAG_LEN] = {0};     // Тег (категория)
    char message[LOG_MAX_MESSAGE_LEN] = {0}; // Сообщение
    uint16_t sequence = 0;               // Порядковый номер (для детекции потерь)

    void clear() {
        timestamp = 0;
        level = LOG_INFO;
        sourceModule = 0;
        memset(tag, 0, sizeof(tag));
        memset(message, 0, sizeof(message));
        sequence = 0;
    }

    bool isValid() const {
        return timestamp > 0 && message[0] != '\0';
    }
};

// -----------------------------------------------------------------------------
// СТАТИСТИКА ЛОГИРОВАНИЯ
// -----------------------------------------------------------------------------
struct LogStats {
    uint32_t totalEntries = 0;           // Всего записей за сессию
    uint32_t droppedEntries = 0;         // Потеряно (переполнение буфера)
    uint32_t flushCount = 0;             // Количество сбросов на диск
    uint32_t lastFlushMs = 0;            // Время последнего сброса
    uint32_t fileSize = 0;               // Текущий размер файла
    uint32_t fileRotations = 0;          // Количество ротаций файла
    uint16_t currentSequence = 0;        // Текущий порядковый номер
};

// -----------------------------------------------------------------------------
// ИНТЕРФЕЙС КОЛБЭКОВ
// -----------------------------------------------------------------------------
using LogCallback = std::function<void(const LogEntry& entry)>;

// -----------------------------------------------------------------------------
// LOG MANAGER
// -----------------------------------------------------------------------------
class LogManager : public IModule {
public:
    // IModule interface
    void init() override;
    void init(const ShEventData* config) override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;

    uint32_t getModuleId() const override { return MODULE_ID_LOG; }
    const char* getModuleName() const override { return "LogManager"; }
    const char* getVersion() const override { return "5.0.0"; }

    void getStatus(char* buffer, size_t bufferSize) const override;

    bool isReady() const override;
    bool isHealthy() const override;

    uint8_t getPriority() const override { return 50; }  // Высокий приоритет
    uint32_t getTickIntervalMs() const override { return 1000; }

    // --- Публичный API ---

    // Добавление записи (thread-safe, может вызываться из любого модуля)
    bool addLog(LogLevel level, uint32_t sourceModule,
                const char* tag, const char* message);

    // Добавление записи с форматированием (printf-style)
    bool addLogF(LogLevel level, uint32_t sourceModule,
                 const char* tag, const char* format, ...);

    // Получение записей из RAM буфера
    size_t getRamEntriesCount() const;
    bool getRamEntry(size_t index, LogEntry& out) const;

    // Получение последних N записей
    size_t getRecentEntries(LogEntry* outBuffer, size_t maxEntries) const;

    // Очистка RAM буфера
    void clearRamBuffer();

    // Принудительный сброс на диск
    bool flushToDisk();

    // Получение статистики
    void getStats(LogStats& out) const;

    // Установка уровня логирования (фильтрация)
    void setMinLogLevel(LogLevel level);
    LogLevel getMinLogLevel() const;

    // Установка callback'а для real-time обработки (например, WebSocket)
    void setRealtimeCallback(LogCallback callback);
    void clearRealtimeCallback();

    // Установка callback'а для критических ошибок (например, перезагрузка)
    void setCriticalCallback(LogCallback callback);
    void clearCriticalCallback();

private:
    // --- Инициализация ---
    bool initRamBuffer();
    bool initFileSystem();

    // --- Обработка событий ---
    void handleLogEvent(const ShEventData* data);

    // --- RAM буфер (кольцевой) ---
    bool pushToRam(const LogEntry& entry);
    bool popFromRam(LogEntry& out);  // Для сброса на диск

    // --- Файловая система ---
    bool writeEntryToFile(const LogEntry& entry);
    bool rotateFileIfNeeded();
    bool openLogFile(bool append);
    void closeLogFile();

    // --- Форматирование ---
    void formatEntry(const LogEntry& entry, char* out, size_t outSize) const;
    void formatEntryJson(const LogEntry& entry, JsonDocument& out) const;

    // --- Внутренние поля ---

    // RAM буфер (кольцевой, lock-free через индексы)
    alignas(4) std::array<LogEntry, LOG_RAM_BUFFER_SIZE> _ramBuffer;
    volatile uint32_t _writeIndex = 0;     // Индекс записи (только производитель)
    volatile uint32_t _readIndex = 0;       // Индекс чтения (только потребитель)
    volatile uint32_t _droppedCount = 0;  // Счётчик потерь

    // Файловая система
    void* _logFile = nullptr;  // File handle (абстракция, не FILE*)
    char _logFilePath[32] = "/logs/system.log";
    bool _fileSystemReady = false;

    // Статистика
    LogStats _stats;

    // Конфигурация
    LogLevel _minLevel = LOG_DEBUG;
    bool _initialized = false;
    bool _started = false;
    bool _healthy = true;

    // Callback'и
    LogCallback _realtimeCallback = nullptr;
    LogCallback _criticalCallback = nullptr;

    // Потокобезопасность
    SemaphoreHandle_t _logMutex = nullptr;
    SemaphoreHandle_t _fileMutex = nullptr;

    // Макросы для удобства (только внутри модуля)
    static const char* levelToString(LogLevel level);
    static const char* levelToColor(LogLevel level);  // Для ANSI терминала
};