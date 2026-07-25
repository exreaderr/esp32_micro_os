// ============================================================================
// StorageServer.h - ЦЕНТРАЛИЗОВАННЫЙ ДИСПЕТЧЕР ФАЙЛОВОЙ СИСТЕМЫ (LittleFS) v5.0
// ============================================================================
// Описание: Единственный модуль МикроОС, имеющий прямой доступ к LittleFS.
//           Все остальные модули взаимодействуют с ним через события,
//           отправляя запросы на чтение, запись или удаление файлов.
//
// АРХИТЕКТУРНЫЕ ПРИНЦИПЫ:
// 1. Монопольный доступ к файловой системе.
// 2. Асинхронная обработка запросов через очередь.
// 3. Потокобезопасность (защита от одновременных записей).
// 4. Атомарные операции (запись во временный файл с последующим переименованием).
// 5. Отказоустойчивость (восстановление после сбоев).
// 6. Полная интеграция с событийной шиной v5.0.
//
// ИЗМЕНЕНИЯ v5.0:
// - Добавлена публикация событий через новую шину (EVENT_STORAGE_*)
// - Добавлен метод publishStorageEvent() для публикации через AppCore
// - Добавлен счетчик _totalEventsPublished для диагностики
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#pragma once

#include "core/IModule.h"
#include "core/ShEventData.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

// ============================================================================
// 1. СОБЫТИЯ STORAGE SERVER
// ============================================================================
enum StorageEvents : int32_t {
    SH_EVENT_STORAGE_WRITE_REQ = SH_EVENT_USER_BASE + 0x0700,   // Запрос на запись
    SH_EVENT_STORAGE_READ_REQ = SH_EVENT_USER_BASE + 0x0701,    // Запрос на чтение
    SH_EVENT_STORAGE_DELETE_REQ = SH_EVENT_USER_BASE + 0x0702,  // Запрос на удаление
    SH_EVENT_STORAGE_EXISTS_REQ = SH_EVENT_USER_BASE + 0x0703,  // Запрос на проверку существования
    SH_EVENT_STORAGE_LIST_REQ = SH_EVENT_USER_BASE + 0x0704,    // Запрос на список файлов
    SH_EVENT_STORAGE_WRITE_RESP = SH_EVENT_USER_BASE + 0x0710,  // Ответ на запись
    SH_EVENT_STORAGE_READ_RESP = SH_EVENT_USER_BASE + 0x0711,   // Ответ на чтение
    SH_EVENT_STORAGE_DELETE_RESP = SH_EVENT_USER_BASE + 0x0712, // Ответ на удаление
    SH_EVENT_STORAGE_EXISTS_RESP = SH_EVENT_USER_BASE + 0x0713, // Ответ на проверку существования
    SH_EVENT_STORAGE_LIST_RESP = SH_EVENT_USER_BASE + 0x0714,   // Ответ на список файлов
    SH_EVENT_STORAGE_ERROR = SH_EVENT_USER_BASE + 0x0715,       // Ошибка
    SH_EVENT_STORAGE_MOUNTED = SH_EVENT_USER_BASE + 0x0716,     // Файловая система смонтирована
    SH_EVENT_STORAGE_UNMOUNTED = SH_EVENT_USER_BASE + 0x0717    // Файловая система размонтирована
};

// ============================================================================
// 2. СТРУКТУРЫ ДАННЫХ
// ============================================================================

/**
 * @brief Типы операций с файлами
 */
enum class FileOperationType : uint8_t {
    WRITE = 0,
    READ = 1,
    DELETE = 2,
    EXISTS = 3,
    LIST = 4,
    UNKNOWN = 0xFF
};

/**
 * @brief Структура запроса к файловой системе
 */
struct FileOperationRequest {
    uint32_t requestId;              // Уникальный ID запроса
    uint8_t requesterModuleId;       // ID модуля, отправившего запрос
    uint32_t timestamp;              // Время отправки запроса (для таймаутов)
    char fileName[32];               // Имя файла
    FileOperationType operation;     // Тип операции
    uint8_t dataBuffer[256];         // Буфер для данных (для записи или ответа)
    uint16_t dataSize;               // Реальный размер данных в буфере
    bool isCompleted;                // Флаг завершения операции
    bool isSuccess;                  // Флаг успешности операции
    uint32_t errorCode;              // Код ошибки (если isSuccess == false)
};

/**
 * @brief Статистика StorageServer
 */
struct StorageStats {
    uint32_t totalRequests = 0;      // Всего запросов
    uint32_t successfulRequests = 0; // Успешных запросов
    uint32_t failedRequests = 0;     // Неудачных запросов
    uint32_t currentQueueSize = 0;   // Текущий размер очереди
    uint32_t maxQueueSize = 0;       // Максимальный размер очереди
    uint32_t totalBytesWritten = 0;  // Всего записано байт
    uint32_t totalBytesRead = 0;     // Всего прочитано байт
    uint32_t totalFilesDeleted = 0;  // Всего удалено файлов
    uint32_t fsMountCount = 0;       // Количество монтирований FS
    uint32_t fsErrorCount = 0;       // Количество ошибок FS
    uint32_t lastErrorCode = 0;      // Последний код ошибки
    uint32_t freeSpace = 0;          // Свободное место в байтах
    uint32_t totalSpace = 0;         // Общее место в байтах
};

// ============================================================================
// 3. КЛАСС МОДУЛЯ STORAGE SERVER
// ============================================================================

class StorageServer : public IModule {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(const char* fileName, bool success, uint32_t errorCode)> OnOperationCompleteCallback;
    typedef std::function<void(const StorageStats& stats)> OnStatsUpdateCallback;
    typedef std::function<void(const char* error)> OnErrorCallback;

    // === СИНГЛТОН ===
    static StorageServer& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    StorageServer();
    ~StorageServer();

    // Запрещаем копирование
    StorageServer(const StorageServer&) = delete;
    StorageServer& operator=(const StorageServer&) = delete;

    // === IModule ===
    const char* getName() const override { return "StorageServer"; }
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_STORAGE; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _ready && _initialized && _fsMounted; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === API ДЛЯ ПРЯМОГО ВЫЗОВА (ДЛЯ ОБРАТНОЙ СОВМЕСТИМОСТИ) ===
    bool writeFile(const char* fileName, const uint8_t* data, size_t size);
    bool readFile(const char* fileName, uint8_t* buffer, size_t bufferSize, size_t& outSize);
    bool deleteFile(const char* fileName);
    bool fileExists(const char* fileName) const;
    std::vector<String> listFiles(const char* directory = "/") const;
    size_t getFileSize(const char* fileName) const;

    // === УПРАВЛЕНИЕ ===
    void forceFlush();
    bool formatFS();

    // === ГЕТТЕРЫ ===
    StorageStats getStats() const { return _stats; }
    size_t getQueueSize() const { return _queueCount; }
    bool isMounted() const { return _fsMounted; }
    const char* getLastError() const { return _lastError; }

    // === НАСТРОЙКИ ===
    void setMaxQueueSize(uint8_t size) { _maxQueueSize = constrain(size, 4, 20); }
    void setOperationTimeout(uint32_t ms) { _operationTimeoutMs = ms; }
    void setAutoFlush(bool enable) { _autoFlush = enable; }

    // === КОЛБЭКИ ===
    void setOnOperationComplete(OnOperationCompleteCallback cb) { _onOperationComplete = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }
    void setOnError(OnErrorCallback cb) { _onError = cb; }

    // === НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0) ===
    void publishStorageEvent(const char* eventType, const char* fileName, bool success, uint32_t errorCode);

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    bool initFS();
    void processNextRequest();
    void handleWriteRequest(FileOperationRequest& request);
    void handleReadRequest(FileOperationRequest& request);
    void handleDeleteRequest(FileOperationRequest& request);
    void handleExistsRequest(FileOperationRequest& request);
    void handleListRequest(FileOperationRequest& request);
    void sendResponse(const FileOperationRequest& request);
    bool enqueueRequest(const ShEventData& event);
    void updateStats();
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);

    // === НОВЫЙ МЕТОД: ВНУТРЕННЯЯ ПУБЛИКАЦИЯ СОБЫТИЙ ===
    void publishStorageEventInternal(const char* eventType, const char* fileName, bool success, uint32_t errorCode);

    // === ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ) ===
    void publishErrorEvent(const char* errorCode);
    void publishMountedEvent();
    void publishUnmountedEvent();

    // === ОБРАБОТЧИКИ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ДАННЫЕ ===
    uint8_t _id = 0xFE;                     // ID модуля (StorageServer)
    bool _initialized = false;
    bool _ready = false;
    bool _fsMounted = false;
    bool _autoFlush = true;

    // Очередь запросов
    static constexpr uint8_t MAX_QUEUE_SIZE = 10;
    FileOperationRequest _requestQueue[MAX_QUEUE_SIZE];
    uint8_t _queueHead = 0;
    uint8_t _queueTail = 0;
    uint8_t _queueCount = 0;
    uint8_t _maxQueueSize = MAX_QUEUE_SIZE;

    // Таймауты
    uint32_t _operationTimeoutMs = 10000;   // 10 секунд
    uint32_t _lastProcessTime = 0;

    // Статистика
    StorageStats _stats;
    char _lastError[64] = "";

    // Счетчик запросов
    uint32_t _requestCounter = 0;

    // НОВОЕ: счетчик опубликованных событий
    uint32_t _totalEventsPublished = 0;

    // Мьютекс (рекурсивный)
    SemaphoreHandle_t _mutex = nullptr;

    // === КОЛБЭКИ ===
    OnOperationCompleteCallback _onOperationComplete = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;
    OnErrorCallback _onError = nullptr;

    // Константы
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
    static constexpr const char* TEMP_FILE_PREFIX = "tmp_";
};