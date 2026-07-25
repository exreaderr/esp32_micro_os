// ============================================================================
// StorageServer.cpp - РЕАЛИЗАЦИЯ ЦЕНТРАЛИЗОВАННОГО ДИСПЕТЧЕРА ФАЙЛОВОЙ СИСТЕМЫ v5.0
// ============================================================================
// Описание: Единственный модуль МикроОС, имеющий прямой доступ к LittleFS.
//           Все остальные модули взаимодействуют с ним через события.
//
// ИЗМЕНЕНИЯ v5.0:
// - Сохранена 100% функциональность v4.2.2
// - Добавлен метод publishStorageEvent() для публикации через новую шину
// - Добавлен вызов publishStorageEventInternal() во все обработчики
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "StorageServer.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static StorageServer _storageServerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
StorageServer::StorageServer() {
    _moduleId = MODULE_ID_STORAGE;

    // Создаем рекурсивный мьютекс
    _mutex = xSemaphoreCreateRecursiveMutex();
    if (_mutex == nullptr) {
        Serial.println("[STORAGE] CRITICAL: Failed to create mutex!");
        while (1) { delay(100); }
    }

    // Инициализация очереди
    memset(_requestQueue, 0, sizeof(_requestQueue));
    _queueHead = 0;
    _queueTail = 0;
    _queueCount = 0;
    _totalEventsPublished = 0;

    memset(&_stats, 0, sizeof(_stats));
    _lastError[0] = '\0';
    _requestCounter = 0;

    _initialized = false;
    _ready = false;
    _fsMounted = false;
    _autoFlush = true;
    _operationTimeoutMs = 10000;
    _lastProcessTime = 0;

    Serial.println("[STORAGE] Instance created (v5.0)");
}

StorageServer::~StorageServer() {
    stop();
    if (_mutex != nullptr) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
StorageServer& StorageServer::getInstance() {
    return _storageServerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void StorageServer::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void StorageServer::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[STORAGE] %s\n", msg);

    if (_initialized) {
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = _moduleId;
        data.targetModule = MODULE_ID_LOG;
        data.command = SH_EVENT_LOG_ENTRY;
        data.value = 0;
        safeStrCopy(data.payload, sizeof(data.payload), msg);
        data.payloadLen = strlen(data.payload);
        postEvent(SH_EVENT_CMD_EXECUTE, &data);
    }
}

void StorageServer::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void StorageServer::init() {
    if (_initialized) return;

    logMessage("Initializing...");

    // 1. Инициализация файловой системы
    if (!initFS()) {
        logMessage("CRITICAL: LittleFS mount failed!");
        _initialized = false;
        _ready = false;
        return;
    }

    // 2. Подписка на события
    esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &StorageServer::eventHandler,
        this,
        NULL
    );
    esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &StorageServer::eventHandler,
        this,
        NULL
    );

    _initialized = true;
    _ready = true;
    _lastProcessTime = millis();

    logMessage("Initialized successfully (v%s)", getVersion());
    publishMountedEvent();
}

void StorageServer::start() {
    if (!isReady()) {
        logMessage("Cannot start: not ready");
        return;
    }
    logMessage("Started");
}

void StorageServer::stop() {
    if (!_initialized) return;

    logMessage("Stopping...");

    // Обрабатываем оставшиеся запросы
    if (_queueCount > 0) {
        logMessage("Processing %d remaining requests...", _queueCount);
        uint32_t timeout = millis() + 5000;
        while (_queueCount > 0 && millis() < timeout) {
            processNextRequest();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    _initialized = false;
    _ready = false;
    logMessage("Stopped");
}

void StorageServer::tick() {
    if (!isReady()) return;

    esp_task_wdt_reset();

    // --- 1. Обработка запросов ---
    if (_queueCount > 0) {
        processNextRequest();
    }

    // --- 2. Обновление статистики (раз в 10 секунд) ---
    static uint32_t lastStatsUpdate = 0;
    if (millis() - lastStatsUpdate > 10000) {
        lastStatsUpdate = millis();
        updateStats();
        if (_onStatsUpdate) {
            _onStatsUpdate(_stats);
        }
    }
}

// ============================================================================
// 5. ОБРАБОТЧИКИ СОБЫТИЙ
// ============================================================================
void StorageServer::eventHandler(void* handlerArgs, esp_event_base_t base,
                                int32_t id, void* eventData) {
    StorageServer* instance = static_cast<StorageServer*>(handlerArgs);
    if (!instance || !instance->isReady()) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
                break;
            default:
                break;
        }
        return;
    }

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void StorageServer::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    // === КОМАНДЫ ===
    if (eventId == SH_EVENT_CMD_EXECUTE) {
        if (data->targetModule == _moduleId || data->targetModule == 0) {
            handleCommand(data);
        }
        return;
    }

    // === ЗАПРОСЫ НА ОПЕРАЦИИ ===
    switch (eventId) {
        case SH_EVENT_STORAGE_WRITE_REQ:
            if (enqueueRequest(*data)) {
                logMessage("Write request queued (file: %s)", data->payload);
            } else {
                logMessage("Queue full, dropping write request");
                publishErrorEvent("QUEUE_FULL");
            }
            break;

        case SH_EVENT_STORAGE_READ_REQ:
            if (enqueueRequest(*data)) {
                logMessage("Read request queued (file: %s)", data->payload);
            } else {
                logMessage("Queue full, dropping read request");
                publishErrorEvent("QUEUE_FULL");
            }
            break;

        case SH_EVENT_STORAGE_DELETE_REQ:
            if (enqueueRequest(*data)) {
                logMessage("Delete request queued (file: %s)", data->payload);
            } else {
                logMessage("Queue full, dropping delete request");
                publishErrorEvent("QUEUE_FULL");
            }
            break;

        case SH_EVENT_STORAGE_EXISTS_REQ:
            if (enqueueRequest(*data)) {
                logMessage("Exists request queued (file: %s)", data->payload);
            } else {
                logMessage("Queue full, dropping exists request");
                publishErrorEvent("QUEUE_FULL");
            }
            break;

        case SH_EVENT_STORAGE_LIST_REQ:
            if (enqueueRequest(*data)) {
                logMessage("List request queued");
            } else {
                logMessage("Queue full, dropping list request");
                publishErrorEvent("QUEUE_FULL");
            }
            break;

        default:
            break;
    }
}

bool StorageServer::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_STORAGE_WRITE_REQ ||
            eventId == SH_EVENT_STORAGE_READ_REQ ||
            eventId == SH_EVENT_STORAGE_DELETE_REQ ||
            eventId == SH_EVENT_STORAGE_EXISTS_REQ ||
            eventId == SH_EVENT_STORAGE_LIST_REQ);
}

// ============================================================================
// 6. ОБРАБОТКА КОМАНД
// ============================================================================
void StorageServer::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case CMD_GET_STATUS: {
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = _stats.currentQueueSize;
            const char* status = getStatus();
            safeStrCopy(response.payload, sizeof(response.payload), status);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case CMD_GET_DIAG: {
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = _stats.totalRequests;
            snprintf(response.payload, sizeof(response.payload),
                    "total:%lu,ok:%lu,fail:%lu,queue:%lu,fs_err:%lu",
                    _stats.totalRequests,
                    _stats.successfulRequests,
                    _stats.failedRequests,
                    _stats.currentQueueSize,
                    _stats.fsErrorCount);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0700: // FORCE_FLUSH
            forceFlush();
            break;

        case 0x0701: // FORMAT_FS
            formatFS();
            break;

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 7. СТАТУС
// ============================================================================
const char* StorageServer::getStatus() const {
    static char statusBuffer[128];

    snprintf(statusBuffer, sizeof(statusBuffer),
            "FS:%s, Queue:%d, Req:%lu, OK:%lu, Fail:%lu, Free:%luKB",
            _fsMounted ? "OK" : "ERR",
            _queueCount,
            _stats.totalRequests,
            _stats.successfulRequests,
            _stats.failedRequests,
            _stats.freeSpace / 1024);

    return statusBuffer;
}

// ============================================================================
// 8. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void StorageServer::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _stats.currentQueueSize;

    snprintf(data->payload, sizeof(data->payload),
            "fs:%d,req:%lu,ok:%lu,fail:%lu,queue:%lu,events:%lu",
            _fsMounted ? 1 : 0,
            _stats.totalRequests,
            _stats.successfulRequests,
            _stats.failedRequests,
            _stats.currentQueueSize,
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 9. НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void StorageServer::publishStorageEventInternal(const char* eventType, const char* fileName,
                                                bool success, uint32_t errorCode) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = success ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "Storage: %s %s - %s (code: %lu)",
             eventType,
             fileName ? fileName : "",
             success ? "OK" : "FAIL",
             errorCode);

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void StorageServer::publishStorageEvent(const char* eventType, const char* fileName,
                                        bool success, uint32_t errorCode) {
    publishStorageEventInternal(eventType, fileName, success, errorCode);
}

// ============================================================================
// 10. ОТПРАВКА СОБЫТИЙ
// ============================================================================
void StorageServer::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_STORAGE_ERROR;
    data.value = _stats.fsErrorCount;
    safeStrCopy(data.payload, sizeof(data.payload), errorCode ? errorCode : "UNKNOWN_ERROR");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    publishStorageEventInternal("ERROR", nullptr, false, 0);
}

void StorageServer::publishMountedEvent() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_STORAGE_MOUNTED;
    data.value = 1;
    safeStrCopy(data.payload, sizeof(data.payload), "FS mounted");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    publishStorageEventInternal("MOUNTED", nullptr, true, 0);
}

void StorageServer::publishUnmountedEvent() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_STORAGE_UNMOUNTED;
    data.value = 0;
    safeStrCopy(data.payload, sizeof(data.payload), "FS unmounted");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    publishStorageEventInternal("UNMOUNTED", nullptr, true, 0);
}

// ============================================================================
// 11. ИНИЦИАЛИЗАЦИЯ ФАЙЛОВОЙ СИСТЕМЫ
// ============================================================================
bool StorageServer::initFS() {
    logMessage("Mounting LittleFS...");

    if (!LittleFS.begin(false)) {
        logMessage("LittleFS mount failed, trying format...");
        if (!LittleFS.begin(true)) {
            logMessage("LittleFS format failed!");
            _fsMounted = false;
            _stats.fsErrorCount++;
            return false;
        }
    }

    _fsMounted = true;
    _stats.fsMountCount++;

    // Обновляем статистику
    updateStats();

    logMessage("LittleFS mounted: total=%luKB, used=%luKB, free=%luKB",
              _stats.totalSpace / 1024,
              (_stats.totalSpace - _stats.freeSpace) / 1024,
              _stats.freeSpace / 1024);

    return true;
}

// ============================================================================
// 12. УПРАВЛЕНИЕ ОЧЕРЕДЬЮ ЗАПРОСОВ
// ============================================================================
bool StorageServer::enqueueRequest(const ShEventData& event) {
    if (_queueCount >= _maxQueueSize) {
        _stats.failedRequests++;
        return false;
    }

    FileOperationRequest& request = _requestQueue[_queueTail];

    // Заполняем запрос
    request.requestId = ++_requestCounter;
    request.requesterModuleId = event.senderId;
    request.timestamp = millis();
    request.isCompleted = false;
    request.isSuccess = false;
    request.errorCode = 0;

    // Копируем имя файла из payload
    safeStrCopy(request.fileName, sizeof(request.fileName), event.payload);

    // Определяем тип операции
    switch (event.command) {
        case SH_EVENT_STORAGE_WRITE_REQ:
            request.operation = FileOperationType::WRITE;
            request.dataSize = event.value;
            if (request.dataSize > sizeof(request.dataBuffer)) {
                request.dataSize = sizeof(request.dataBuffer);
            }
            memcpy(request.dataBuffer,
                   event.payload + strlen(event.payload) + 1,
                   request.dataSize);
            break;

        case SH_EVENT_STORAGE_READ_REQ:
            request.operation = FileOperationType::READ;
            request.dataSize = 0;
            break;

        case SH_EVENT_STORAGE_DELETE_REQ:
            request.operation = FileOperationType::DELETE;
            request.dataSize = 0;
            break;

        case SH_EVENT_STORAGE_EXISTS_REQ:
            request.operation = FileOperationType::EXISTS;
            request.dataSize = 0;
            break;

        case SH_EVENT_STORAGE_LIST_REQ:
            request.operation = FileOperationType::LIST;
            request.dataSize = 0;
            break;

        default:
            return false;
    }

    // Перемещаем указатель хвоста
    _queueTail = (_queueTail + 1) % MAX_QUEUE_SIZE;
    _queueCount++;
    _stats.currentQueueSize = _queueCount;
    if (_queueCount > _stats.maxQueueSize) {
        _stats.maxQueueSize = _queueCount;
    }

    return true;
}

// ============================================================================
// 13. ОБРАБОТКА ЗАПРОСОВ
// ============================================================================
void StorageServer::processNextRequest() {
    if (_queueCount == 0) return;

    // Берем запрос из головы очереди
    FileOperationRequest& request = _requestQueue[_queueHead];

    // Проверяем таймаут
    if (!request.isCompleted && (millis() - request.timestamp > _operationTimeoutMs)) {
        logMessage("Request timeout (ID: %lu)", request.requestId);
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x1001; // Таймаут
        _stats.failedRequests++;

        // Отправляем ответ об ошибке
        sendResponse(request);
        publishStorageEventInternal("TIMEOUT", request.fileName, false, request.errorCode);

        // Перемещаем голову
        _queueHead = (_queueHead + 1) % MAX_QUEUE_SIZE;
        _queueCount--;
        _stats.currentQueueSize = _queueCount;
        return;
    }

    // Если запрос уже обработан — удаляем его
    if (request.isCompleted) {
        _queueHead = (_queueHead + 1) % MAX_QUEUE_SIZE;
        _queueCount--;
        _stats.currentQueueSize = _queueCount;
        return;
    }

    // Выполняем операцию
    switch (request.operation) {
        case FileOperationType::WRITE:
            handleWriteRequest(request);
            break;

        case FileOperationType::READ:
            handleReadRequest(request);
            break;

        case FileOperationType::DELETE:
            handleDeleteRequest(request);
            break;

        case FileOperationType::EXISTS:
            handleExistsRequest(request);
            break;

        case FileOperationType::LIST:
            handleListRequest(request);
            break;

        default:
            request.isCompleted = true;
            request.isSuccess = false;
            request.errorCode = 0x1002; // Неизвестная операция
            _stats.failedRequests++;
            break;
    }

    // Отправляем ответ
    if (request.isCompleted) {
        sendResponse(request);

        // Обновляем статистику
        if (request.isSuccess) {
            _stats.successfulRequests++;
        } else {
            _stats.failedRequests++;
        }

        // Публикуем событие через новую шину
        const char* opType = "UNKNOWN";
        switch (request.operation) {
            case FileOperationType::WRITE: opType = "WRITE"; break;
            case FileOperationType::READ: opType = "READ"; break;
            case FileOperationType::DELETE: opType = "DELETE"; break;
            case FileOperationType::EXISTS: opType = "EXISTS"; break;
            case FileOperationType::LIST: opType = "LIST"; break;
            default: break;
        }
        publishStorageEventInternal(opType, request.fileName, request.isSuccess, request.errorCode);

        if (_onOperationComplete) {
            _onOperationComplete(request.fileName, request.isSuccess, request.errorCode);
        }

        // Перемещаем голову
        _queueHead = (_queueHead + 1) % MAX_QUEUE_SIZE;
        _queueCount--;
        _stats.currentQueueSize = _queueCount;
    }
}

// ============================================================================
// 14. ОПЕРАЦИИ С ФАЙЛАМИ
// ============================================================================
void StorageServer::handleWriteRequest(FileOperationRequest& request) {
    if (strlen(request.fileName) == 0) {
        logMessage("Empty file name");
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x2001;
        return;
    }

    // Атомарная запись: сначала во временный файл
    char tempName[40];
    snprintf(tempName, sizeof(tempName), "%s%s.tmp", TEMP_FILE_PREFIX, request.fileName);

    File file = LittleFS.open(tempName, "w");
    if (!file) {
        logMessage("Failed to open temp file for write: %s", request.fileName);
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x2002;
        _stats.fsErrorCount++;
        return;
    }

    size_t bytesWritten = file.write(request.dataBuffer, request.dataSize);
    file.close();

    if (bytesWritten != request.dataSize) {
        logMessage("Write failed: wrote %zu of %u bytes", bytesWritten, request.dataSize);
        LittleFS.remove(tempName);
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x2003;
        _stats.fsErrorCount++;
        return;
    }

    // Атомарное переименование
    if (LittleFS.exists(request.fileName)) {
        LittleFS.remove(request.fileName);
    }
    if (!LittleFS.rename(tempName, request.fileName)) {
        logMessage("Atomic rename failed for: %s", request.fileName);
        LittleFS.remove(tempName);
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x2004;
        _stats.fsErrorCount++;
        return;
    }

    _stats.totalBytesWritten += bytesWritten;
    logMessage("File written: %s (%u bytes)", request.fileName, request.dataSize);
    request.isCompleted = true;
    request.isSuccess = true;
}

void StorageServer::handleReadRequest(FileOperationRequest& request) {
    if (strlen(request.fileName) == 0) {
        logMessage("Empty file name");
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x3001;
        return;
    }

    if (!LittleFS.exists(request.fileName)) {
        logMessage("File not found: %s", request.fileName);
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x3002;
        return;
    }

    File file = LittleFS.open(request.fileName, "r");
    if (!file) {
        logMessage("Failed to open file for read: %s", request.fileName);
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x3003;
        _stats.fsErrorCount++;
        return;
    }

    size_t bytesRead = file.read(request.dataBuffer, sizeof(request.dataBuffer) - 1);
    file.close();

    request.dataSize = bytesRead;
    request.dataBuffer[bytesRead] = '\0';

    _stats.totalBytesRead += bytesRead;
    logMessage("File read: %s (%u bytes)", request.fileName, bytesRead);
    request.isCompleted = true;
    request.isSuccess = true;
}

void StorageServer::handleDeleteRequest(FileOperationRequest& request) {
    if (strlen(request.fileName) == 0) {
        logMessage("Empty file name");
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x4001;
        return;
    }

    if (!LittleFS.exists(request.fileName)) {
        logMessage("File does not exist: %s (already deleted)", request.fileName);
        request.isCompleted = true;
        request.isSuccess = true; // Файла нет — считаем операцию успешной
        return;
    }

    if (!LittleFS.remove(request.fileName)) {
        logMessage("Failed to delete file: %s", request.fileName);
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x4002;
        _stats.fsErrorCount++;
        return;
    }

    _stats.totalFilesDeleted++;
    logMessage("File deleted: %s", request.fileName);
    request.isCompleted = true;
    request.isSuccess = true;
}

void StorageServer::handleExistsRequest(FileOperationRequest& request) {
    if (strlen(request.fileName) == 0) {
        logMessage("Empty file name");
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x5001;
        return;
    }

    bool exists = LittleFS.exists(request.fileName);
    request.dataSize = 1;
    request.dataBuffer[0] = exists ? 1 : 0;

    logMessage("File %s: %s", request.fileName, exists ? "EXISTS" : "NOT FOUND");
    request.isCompleted = true;
    request.isSuccess = true;
}

void StorageServer::handleListRequest(FileOperationRequest& request) {
    const char* dir = "/";
    if (strlen(request.fileName) > 0) {
        dir = request.fileName;
    }

    File root = LittleFS.open(dir);
    if (!root) {
        logMessage("Failed to open directory: %s", dir);
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x6001;
        _stats.fsErrorCount++;
        return;
    }

    if (!root.isDirectory()) {
        logMessage("Not a directory: %s", dir);
        root.close();
        request.isCompleted = true;
        request.isSuccess = false;
        request.errorCode = 0x6002;
        return;
    }

    // Формируем список файлов в буфере
    String list;
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            list += String(file.name()) + "\n";
        }
        file = root.openNextFile();
    }
    root.close();

    if (list.length() > 0) {
        request.dataSize = min(list.length(), (size_t)(sizeof(request.dataBuffer) - 1));
        memcpy(request.dataBuffer, list.c_str(), request.dataSize);
        request.dataBuffer[request.dataSize] = '\0';
    } else {
        request.dataSize = 0;
        request.dataBuffer[0] = '\0';
    }

    logMessage("Directory listing: %d files", list.length() > 0 ? 1 : 0);
    request.isCompleted = true;
    request.isSuccess = true;
}

// ============================================================================
// 15. ОТПРАВКА ОТВЕТОВ
// ============================================================================
void StorageServer::sendResponse(const FileOperationRequest& request) {
    ShEventData response;
    response.type = SH_EVENT_STORAGE_WRITE_RESP; // Универсальный ответ
    response.senderId = _moduleId;
    response.targetModule = request.requesterModuleId;

    // Копируем имя файла
    safeStrCopy(response.payload, sizeof(response.payload), request.fileName);

    // Передаем результат
    response.value = request.isSuccess ? 1 : 0;
    response.command = request.errorCode;

    // Для READ и EXISTS передаем данные
    if (request.isSuccess && (request.operation == FileOperationType::READ ||
                              request.operation == FileOperationType::EXISTS ||
                              request.operation == FileOperationType::LIST)) {
        size_t copySize = request.dataSize;
        if (copySize > sizeof(response.payload) - strlen(response.payload) - 1) {
            copySize = sizeof(response.payload) - strlen(response.payload) - 1;
        }
        memcpy(response.payload + strlen(response.payload) + 1,
               request.dataBuffer, copySize);
    }

    postEvent(response.type, &response);
}

// ============================================================================
// 16. API ДЛЯ ПРЯМОГО ВЫЗОВА
// ============================================================================
bool StorageServer::writeFile(const char* fileName, const uint8_t* data, size_t size) {
    if (!isReady() || fileName == nullptr || data == nullptr) return false;

    ShEventData event;
    event.type = SH_EVENT_STORAGE_WRITE_REQ;
    event.senderId = _moduleId;
    event.targetModule = _moduleId;
    safeStrCopy(event.payload, sizeof(event.payload), fileName);
    event.value = size;

    size_t copySize = min(size, sizeof(event.payload) - strlen(fileName) - 1);
    memcpy(event.payload + strlen(fileName) + 1, data, copySize);

    // Отправляем и ждем ответ (синхронно)
    postEvent(event.type, &event);

    // Ждем обработки (упрощенно)
    uint32_t timeout = millis() + _operationTimeoutMs;
    while (_queueCount > 0 && millis() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return true;
}

bool StorageServer::readFile(const char* fileName, uint8_t* buffer, size_t bufferSize, size_t& outSize) {
    if (!isReady() || fileName == nullptr || buffer == nullptr) return false;

    File file = LittleFS.open(fileName, "r");
    if (!file) return false;

    outSize = file.read(buffer, bufferSize);
    file.close();
    return true;
}

bool StorageServer::deleteFile(const char* fileName) {
    if (!isReady() || fileName == nullptr) return false;

    if (!LittleFS.exists(fileName)) return true;
    return LittleFS.remove(fileName);
}

bool StorageServer::fileExists(const char* fileName) const {
    if (!_fsMounted || fileName == nullptr) return false;
    return LittleFS.exists(fileName);
}

std::vector<String> StorageServer::listFiles(const char* directory) const {
    std::vector<String> result;
    if (!_fsMounted || directory == nullptr) return result;

    File root = LittleFS.open(directory);
    if (!root || !root.isDirectory()) return result;

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            result.push_back(String(file.name()));
        }
        file = root.openNextFile();
    }
    root.close();
    return result;
}

size_t StorageServer::getFileSize(const char* fileName) const {
    if (!_fsMounted || fileName == nullptr) return 0;
    if (!LittleFS.exists(fileName)) return 0;

    File file = LittleFS.open(fileName, "r");
    if (!file) return 0;
    size_t size = file.size();
    file.close();
    return size;
}

// ============================================================================
// 17. УПРАВЛЕНИЕ
// ============================================================================
void StorageServer::forceFlush() {
    logMessage("Force flush not needed - operations are immediate");
    publishStorageEventInternal("FLUSH", nullptr, true, 0);
}

bool StorageServer::formatFS() {
    logMessage("Formatting LittleFS...");

    stop();

    if (!LittleFS.format()) {
        logMessage("Format failed!");
        _stats.fsErrorCount++;
        publishStorageEventInternal("FORMAT", nullptr, false, 0x7001);
        return false;
    }

    logMessage("Format successful");
    _stats.fsMountCount++;

    if (!initFS()) {
        publishStorageEventInternal("FORMAT", nullptr, false, 0x7002);
        return false;
    }

    publishStorageEventInternal("FORMAT", nullptr, true, 0);
    return true;
}

// ============================================================================
// 18. СТАТИСТИКА
// ============================================================================
void StorageServer::updateStats() {
    _stats.freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
    _stats.totalSpace = LittleFS.totalBytes();
    _stats.currentQueueSize = _queueCount;
}

// ============================================================================
// 19. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void StorageServer::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================");
    stream.println(" STORAGE SERVER DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Ready: %s\n", _ready ? "YES" : "NO");
    stream.printf(" FS Mounted: %s\n", _fsMounted ? "YES" : "NO");
    stream.println("-- Stats --");
    stream.printf(" Total Requests: %lu\n", _stats.totalRequests);
    stream.printf(" Successful: %lu\n", _stats.successfulRequests);
    stream.printf(" Failed: %lu\n", _stats.failedRequests);
    stream.printf(" Queue Size: %lu/%lu\n", _stats.currentQueueSize, _maxQueueSize);
    stream.printf(" Max Queue: %lu\n", _stats.maxQueueSize);
    stream.printf(" Bytes Written: %lu\n", _stats.totalBytesWritten);
    stream.printf(" Bytes Read: %lu\n", _stats.totalBytesRead);
    stream.printf(" Files Deleted: %lu\n", _stats.totalFilesDeleted);
    stream.printf(" FS Mounts: %lu\n", _stats.fsMountCount);
    stream.printf(" FS Errors: %lu\n", _stats.fsErrorCount);
    stream.printf(" Free Space: %lu bytes\n", _stats.freeSpace);
    stream.printf(" Total Space: %lu bytes\n", _stats.totalSpace);
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("-- Config --");
    stream.printf(" Max Queue: %d\n", _maxQueueSize);
    stream.printf(" Operation Timeout: %lu ms\n", _operationTimeoutMs);
    stream.printf(" Auto Flush: %s\n", _autoFlush ? "ON" : "OFF");
    stream.println("==============================");
}

void StorageServer::printStats() const {
    streamDiagnosticInfo(Serial);
}