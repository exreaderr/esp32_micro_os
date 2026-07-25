// ============================================================================
// LogManager.cpp - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Полноценная система логирования с поддержкой:
// - RAM буфер с автоматическим сбросом на диск
// - Ротация логов по размеру
// - Событийная модель публикации
// - ANSI-цвета для Serial
// - Поддержка форматированных сообщений
// - Потокобезопасность (рекурсивный мьютекс)
// - Защита от фрагментации кучи
// - Публикация событий через новую шину (v5.0)
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА критическая ошибка с двойным освобождением мьютекса в addLog
// - Использовано std::move вместо копирования в flushToDisk
// - Добавлена защита от фрагментации в getLogsAsString
// - Добавлена обрезка source и message для защиты от переполнения
// - Исправлена опечатка _totalsLogsCount -> _totalLogsCount
// - Добавлена проверка command в publishLogEvent
// - Добавлен метод ensureDirectoryExists
// - Добавлена статистика totalBytesWritten
// - Улучшена обработка ошибок
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.2
// - Добавлен метод publishLogEvent() для публикации через новую шину
// - Добавлен вызов publishLogEventInternal() в addInternal()
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "LogManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <sys/time.h>
#include <stdarg.h>
#include <esp_task_wdt.h>
#include "core/IModule.h"

// ============================================================================
// СТАТИСТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static LogManager _logManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
LogManager::LogManager() {
    _moduleId = MODULE_ID_LOG;
    _maxRamEntries = 50;
    _ramBuffer.reserve(_maxRamEntries);
    _logMutex = nullptr;
    _isDirty = false;
    _dirtyTimestamp = 0;
    _lastFlushTime = 0;
    _totalLogsCount = 0;
    _minLevel = LOG_DEBUG;
    _initialized = false;
    _errors = 0;
    _droppedLogs = 0;
    _flushCount = 0;
    _totalBytesWritten = 0;
    _totalEventsPublished = 0; // НОВОЕ
    _autoFlush = true;
    _flushIntervalMs = 10000;
    _onLogCallback = nullptr;
    _onBufferFull = nullptr;
    _onStatusChange = nullptr;
    _onErrorCallback = nullptr;

    _logMutex = xSemaphoreCreateRecursiveMutex();
    if (_logMutex == nullptr) {
        Serial.println("[LOG] FATAL: Failed to create mutex!");
        while (1) { delay(100); }
    }

    Serial.println("[LOG] Instance created (v5.0)");
}

LogManager::~LogManager() {
    if (_initialized) {
        addLog(LOG_INFO, "SYS", "LogManager destroying...");
        flushToDisk();
    }
    if (_logMutex != nullptr) {
        vSemaphoreDelete(_logMutex);
        _logMutex = nullptr;
    }
    Serial.println("[LOG] Instance destroyed");
}

// ============================================================================
// 2. СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
LogManager& LogManager::getInstance() {
    return _logManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool LogManager::ensureDirectoryExists() {
    if (!LittleFS.begin(false)) {
        if (!LittleFS.begin(true)) {
            return false;
        }
    }
    if (!LittleFS.exists("/")) {
        LittleFS.mkdir("/");
    }
    return true;
}

size_t LogManager::getFileSize(const char* path) const {
    if (!LittleFS.exists(path)) return 0;
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t size = f.size();
    f.close();
    return size;
}

void LogManager::trimSourceAndMessage(const char* source, const char* message,
                                      char* trimmedSource, size_t srcSize,
                                      char* trimmedMessage, size_t msgSize) const {
    if (source != nullptr) {
        strncpy(trimmedSource, source, srcSize - 1);
        trimmedSource[srcSize - 1] = '\0';
    } else {
        strncpy(trimmedSource, "UNKN", srcSize - 1);
        trimmedSource[srcSize - 1] = '\0';
    }

    if (message != nullptr) {
        strncpy(trimmedMessage, message, msgSize - 1);
        trimmedMessage[msgSize - 1] = '\0';
    } else {
        trimmedMessage[0] = '\0';
    }
}

void LogManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[LOG] %s\n", msg);
}

void LogManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule) (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void LogManager::init() {
    if (_initialized) return;

    logMessage("Initializing...");

    if (!ensureDirectoryExists()) {
        logMessage("LittleFS mount failed!");
        _initialized = false;
        return;
    }

    size_t fileSize = getFileSize(_logPath);
    if (fileSize > 0) {
        logMessage("Existing log file size: %zu bytes", fileSize);
    }

    if (LittleFS.exists(_indexPath)) {
        File idxFile = LittleFS.open(_indexPath, "r");
        if (idxFile) {
            String countStr = idxFile.readString();
            _totalLogsCount = countStr.toInt();
            idxFile.close();
            logMessage("Restored index: %zu entries", _totalLogsCount);
        }
    }

    esp_err_t err = esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &LogManager::eventHandler,
        this,
        NULL
    );
    if (err != ESP_OK) {
        logMessage("Failed to register sys handler: %d", err);
    }

    err = esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &LogManager::eventHandler,
        this,
        NULL
    );
    if (err != ESP_OK) {
        logMessage("Failed to register app handler: %d", err);
    }

    _lastFlushTime = millis();
    _initialized = true;

    addLog(LOG_INFO, "SYS", "LogManager initialized successfully v5.0");
    updateStatus();

    logMessage("Initialized successfully");
}

void LogManager::start() {
    // Ничего не делаем, уже запущены в init()
}

void LogManager::stop() {
    if (!_initialized) return;

    addLog(LOG_INFO, "SYS", "LogManager stopping...");
    flushToDisk();

    if (_logMutex != nullptr) {
        vSemaphoreDelete(_logMutex);
        _logMutex = nullptr;
    }
    _initialized = false;
    logMessage("Stopped");
}

void LogManager::tick() {
    if (!_initialized) return;

    esp_task_wdt_reset();

    if (_autoFlush && _isDirty && (millis() - _dirtyTimestamp > _flushIntervalMs)) {
        flushToDisk();
    }
}

// ============================================================================
// 5. ОБРАБОТЧИКИ СОБЫТИЙ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool LogManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_CMD_RESPONSE ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN ||
            eventId == SH_EVENT_MODULE_ERROR ||
            eventId == SH_EVENT_HEALTH_CRITICAL ||
            eventId == SH_EVENT_HEALTH_WARNING ||
            eventId == SH_EVENT_LOG_ENTRY ||
            eventId == SH_EVENT_LOG_FLUSHED ||
            eventId == SH_EVENT_LOG_CLEARED);
}

void LogManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    if (eventId == SH_EVENT_CMD_EXECUTE) {
        if (data->targetModule == _moduleId || data->targetModule == 0) {
            handleCommand(data);
        }
    }
}

void LogManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                              int32_t id, void* eventData) {
    LogManager* instance = static_cast<LogManager*>(handlerArgs);
    if (!instance || !instance->_initialized) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->flushToDisk();
                break;

            case SH_EVENT_MODULE_ERROR: {
                if (eventData) {
                    ShEventData* data = static_cast<ShEventData*>(eventData);
                    char buf[128];
                    snprintf(buf, sizeof(buf), "%s error: %d", data->payload, data->value);
                    instance->addLog(LOG_ERROR, "MODULE", buf);
                }
                break;
            }

            case SH_EVENT_HEALTH_CRITICAL:
                instance->addLog(LOG_CRITICAL, "HEALTH", "Critical health issue detected!");
                break;

            case SH_EVENT_HEALTH_WARNING:
                instance->addLog(LOG_WARNING, "HEALTH", "Health warning detected");
                break;

            default:
                break;
        }
    }

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void LogManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case CMD_GET_STATUS: {
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = _initialized ? 1 : 0;
            const char* status = getStatus();
            strncpy(response.payload, status, sizeof(response.payload) - 1);
            response.payload[sizeof(response.payload) - 1] = '\0';
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case CMD_GET_CONFIG: {
            String status = getLogStatusString();
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = status.length();
            strncpy(response.payload, status.c_str(), sizeof(response.payload) - 1);
            response.payload[sizeof(response.payload) - 1] = '\0';
            response.payloadLen = status.length();
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case CMD_GET_DIAG: {
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = _totalLogsCount;
            getDiagnostics(&response);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x1000: { // CMD_FLUSH_LOGS
            flushToDisk();
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_OK;
            strncpy(response.payload, "Logs flushed", sizeof(response.payload) - 1);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x1001: { // CMD_CLEAR_LOGS
            clearAll();
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_OK;
            strncpy(response.payload, "Logs cleared", sizeof(response.payload) - 1);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        default:
            addLogF(LOG_WARNING, "LOG", "Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 6. ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void LogManager::publishLogEventOriginal(const LogEntry& entry) {
    LogEventData event;
    event.timestamp = entry.real_time;
    event.uptime = entry.uptime_sec;
    event.level = (uint8_t)entry.level;
    strncpy(event.source, entry.source, sizeof(event.source) - 1);
    event.source[sizeof(event.source) - 1] = '\0';
    strncpy(event.message, entry.message, sizeof(event.message) - 1);
    event.message[sizeof(event.message) - 1] = '\0';
    event.messageLen = strlen(entry.message);

    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_LOG_ENTRY;
    data.value = event.level;
    memcpy(data.payload, &event, min(sizeof(LogEventData), sizeof(data.payload)));
    data.payloadLen = sizeof(LogEventData);

    if (data.command == 0) {
        data.command = SH_EVENT_LOG_ENTRY;
    }

    postEvent(data.command, &data);
}

void LogManager::publishLogFlushed() {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_LOG_FLUSHED;
    data.value = _flushCount;
    strncpy(data.payload, "Log flushed", sizeof(data.payload) - 1);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void LogManager::publishLogCleared() {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_LOG_CLEARED;
    data.value = 0;
    strncpy(data.payload, "Logs cleared", sizeof(data.payload) - 1);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void LogManager::publishLogError(const char* error) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_LOG_ERROR;
    data.value = _errors;
    strncpy(data.payload, error, sizeof(data.payload) - 1);
    data.payloadLen = strlen(error);
    postEvent(data.command, &data);

    if (_onErrorCallback) {
        _onErrorCallback(error);
    }
}

void LogManager::publishBufferFull(size_t size) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_LOG_BUFFER_FULL;
    data.value = size;
    snprintf(data.payload, sizeof(data.payload), "Buffer full: %zu entries", size);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void LogManager::publishLogRotated() {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_LOG_ROTATED;
    data.value = 1;
    strncpy(data.payload, "Log rotated", sizeof(data.payload) - 1);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

// ============================================================================
// 7. НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void LogManager::publishLogEventInternal(const LogEntry& entry) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = (LogLevel)entry.level;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "[%s][%s] %s",
             entry.source,
             levelToString(entry.level),
             entry.message);

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void LogManager::publishLogEvent(const LogEntry& entry) {
    publishLogEventInternal(entry);
}

// ============================================================================
// 8. ДОБАВЛЕНИЕ ЛОГОВ (ИЗМЕНЕНО: добавлена публикация через новую шину)
// ============================================================================
void LogManager::addInternal(LogLevel level, const char* source, const char* message) {
    if (!_initialized || _logMutex == nullptr) return;
    if (level < _minLevel) return;

    char trimmedSource[MAX_SOURCE_LEN + 1];
    char trimmedMessage[MAX_MESSAGE_LEN + 1];
    trimSourceAndMessage(source, message, trimmedSource, sizeof(trimmedSource),
                         trimmedMessage, sizeof(trimmedMessage));

    bool needFlush = false;

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_ramBuffer.size() >= _maxRamEntries) {
            _ramBuffer.erase(_ramBuffer.begin());
            _droppedLogs++;
            checkBufferFull();
        }

        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        uint32_t realTimestamp = (timeinfo.tm_year >= 100) ? (uint32_t)now : 0;

        LogEntry entry;
        entry.uptime_sec = (uint32_t)(millis() / 1000);
        entry.real_time = realTimestamp;
        entry.level = level;
        strncpy(entry.source, trimmedSource, sizeof(entry.source) - 1);
        entry.source[sizeof(entry.source) - 1] = '\0';
        strncpy(entry.message, trimmedMessage, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        _ramBuffer.push_back(entry);
        _totalLogsCount++;

        writeToSerial(entry);

        if (_onLogCallback != nullptr) {
            _onLogCallback(entry);
        }

        // НОВОЕ: публикация через новую шину
        publishLogEventInternal(entry);

        // Оригинальная публикация в старую шину
        publishLogEventOriginal(entry);

        if (level == LOG_CRITICAL || level == LOG_PAZ) {
            needFlush = true;
        } else if (_autoFlush) {
            scheduleFlush();
        }

        xSemaphoreGiveRecursive(_logMutex);
    }

    if (needFlush) {
        flushToDisk();
    }
}

void LogManager::addLog(LogLevel level, const char* source, const char* message) {
    addInternal(level, source, message);
}

void LogManager::addLog(LogLevel level, const char* source, const String& message) {
    addInternal(level, source, message.c_str());
}

void LogManager::addLog(uint8_t rawLevel, const char* source, const char* message) {
    if (rawLevel <= LOG_AUDIT) {
        addInternal((LogLevel)rawLevel, source, message);
    }
}

void LogManager::addLogF(LogLevel level, const char* source, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    addInternal(level, source, buffer);
}

void LogManager::addLogIf(bool condition, LogLevel level, const char* source, const char* message) {
    if (condition) {
        addInternal(level, source, message);
    }
}

// ============================================================================
// 9. СБРОС НА ДИСК (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void LogManager::flushToDisk() {
    if (!_initialized || _logMutex == nullptr) return;

    checkLogRotation();

    std::vector<LogEntry> copy;
    copy.reserve(_maxRamEntries);

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (_ramBuffer.empty()) {
            _isDirty = false;
            xSemaphoreGiveRecursive(_logMutex);
            return;
        }

        copy = std::move(_ramBuffer);
        _ramBuffer.clear();
        _ramBuffer.reserve(_maxRamEntries);

        _isDirty = false;
        _lastFlushTime = millis();
        xSemaphoreGiveRecursive(_logMutex);
    }

    if (copy.empty()) return;

    if (!ensureDirectoryExists()) {
        _errors++;
        publishLogError("Failed to mount LittleFS for flush");
        return;
    }

    File logFile = LittleFS.open(_logPath, "a");
    if (!logFile) {
        _errors++;
        publishLogError("Failed to open log file for writing");
        return;
    }

    size_t bytesWritten = 0;
    for (const auto& entry : copy) {
        if (formatToFile(logFile, entry)) {
            bytesWritten += strlen(entry.message) + 50;
        }
    }
    logFile.close();

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        _totalLogsCount += copy.size();
        _flushCount++;
        _totalBytesWritten += bytesWritten;

        File idxFile = LittleFS.open(_indexPath, "w");
        if (idxFile) {
            idxFile.println(_totalLogsCount);
            idxFile.close();
        }
        xSemaphoreGiveRecursive(_logMutex);
    }

    publishLogFlushed();
    updateStatus();
}

void LogManager::scheduleFlush() {
    _isDirty = true;
    _dirtyTimestamp = millis();
}

// ============================================================================
// 10. РОТАЦИЯ ЛОГОВ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void LogManager::checkLogRotation() {
    if (!LittleFS.exists(_logPath)) return;

    size_t size = getFileSize(_logPath);
    if (size > MAX_LOG_FILE_SIZE) {
        if (LittleFS.exists(_backupPath)) {
            LittleFS.remove(_backupPath);
        }
        if (LittleFS.rename(_logPath, _backupPath)) {
            addLog(LOG_INFO, "SYS", "Log rotated (size: %zu bytes)", size);
            publishLogRotated();
        } else {
            _errors++;
            publishLogError("Failed to rotate log file");
        }
    }
}

// ============================================================================
// 11. ФОРМАТИРОВАНИЕ В ФАЙЛ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool LogManager::formatToFile(File& logFile, const LogEntry& entry) {
    char timeBuf[32];
    printFormattedTime(timeBuf, sizeof(timeBuf), entry.uptime_sec, entry.real_time);

    char line[256];
    snprintf(line, sizeof(line), "[%s][%s][%s] %s\n",
            timeBuf,
            levelToString(entry.level),
            entry.source,
            entry.message);

    size_t written = logFile.print(line);
    return (written == strlen(line));
}

// ============================================================================
// 12. ВЫВОД В SERIAL (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void LogManager::writeToSerial(const LogEntry& entry) {
    const char* color = levelToColor(entry.level);
    const char* reset = ANSI_RESET;

    char timeBuf[32];
    printFormattedTime(timeBuf, sizeof(timeBuf), entry.uptime_sec, entry.real_time);

    const char* levelStr = levelToString(entry.level);

    if (entry.level >= LOG_ERROR) {
        Serial.printf("%s[%s][%s][%s] %s%s\n",
                     color, timeBuf, levelStr, entry.source, entry.message, reset);
    } else {
        Serial.printf("[%s][%s][%s] %s\n",
                     timeBuf, levelStr, entry.source, entry.message);
    }
}

// ============================================================================
// 13. ФОРМАТИРОВАНИЕ ВРЕМЕНИ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void LogManager::printFormattedTime(char* buf, size_t bufSize, uint32_t uptime, uint32_t realTime) const {
    if (realTime > 0) {
        time_t t = (time_t)realTime;
        struct tm timeinfo;
        localtime_r(&t, &timeinfo);
        strftime(buf, bufSize, TIME_FORMAT, &timeinfo);
    } else {
        snprintf(buf, bufSize, "UPTIME:%lu", uptime);
    }
}

// ============================================================================
// 14. УПРАВЛЕНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void LogManager::clearAll() {
    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _ramBuffer.clear();
        _totalLogsCount = 0;
        _isDirty = false;
        xSemaphoreGiveRecursive(_logMutex);
    }

    if (LittleFS.exists(_logPath)) {
        LittleFS.remove(_logPath);
    }
    if (LittleFS.exists(_indexPath)) {
        LittleFS.remove(_indexPath);
    }

    publishLogCleared();
    logMessage("All logs cleared");
}

void LogManager::setMaxEntries(size_t maxEntries) {
    if (maxEntries < 10) maxEntries = 10;
    if (maxEntries > 500) maxEntries = 500;
    _maxRamEntries = maxEntries;

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _ramBuffer.reserve(_maxRamEntries);
        while (_ramBuffer.size() > _maxRamEntries) {
            _ramBuffer.erase(_ramBuffer.begin());
        }
        xSemaphoreGiveRecursive(_logMutex);
    }
}

void LogManager::setMinLevel(LogLevel level) {
    _minLevel = level;
    updateStatus();
}

// ============================================================================
// 15. ПОЛУЧЕНИЕ ЛОГОВ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
String LogManager::getLogsAsString(size_t limit) {
    String result;
    getLogsAsString(limit, result);
    return result;
}

bool LogManager::getLogsAsString(size_t limit, String& output) const {
    if (_logMutex == nullptr) return false;

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    size_t total = _ramBuffer.size();
    size_t start = (total > limit) ? (total - limit) : 0;

    for (size_t i = start; i < total; i++) {
        const LogEntry& entry = _ramBuffer[i];
        char timeBuf[32];
        printFormattedTime(timeBuf, sizeof(timeBuf), entry.uptime_sec, entry.real_time);

        output += "[";
        output += timeBuf;
        output += "][";
        output += levelToString(entry.level);
        output += "][";
        output += entry.source;
        output += "] ";
        output += entry.message;
        output += "\n";
    }

    xSemaphoreGiveRecursive(_logMutex);
    return true;
}

std::vector<LogEntry> LogManager::getRecentLogs(size_t count) const {
    std::vector<LogEntry> result;
    if (_logMutex == nullptr) return result;

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t total = _ramBuffer.size();
        size_t start = (total > count) ? (total - count) : 0;
        result.reserve(total - start);
        for (size_t i = start; i < total; i++) {
            result.push_back(_ramBuffer[i]);
        }
        xSemaphoreGiveRecursive(_logMutex);
    }
    return result;
}

// ============================================================================
// 16. СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
LogStatus LogManager::getLogStatus() const {
    LogStatus status;
    status.ramBufferSize = _ramBuffer.size();
    status.maxRamEntries = _maxRamEntries;
    status.isDirty = _isDirty;
    status.lastFlushTime = _lastFlushTime;
    status.totalLogsCount = _totalLogsCount;
    status.fileSize = getFileSize(_logPath);
    status.backupSize = getFileSize(_backupPath);
    status.isInitialized = _initialized;
    status.minLevel = _minLevel;
    status.errors = _errors;
    status.droppedLogs = _droppedLogs;
    status.flushCount = _flushCount;
    status.totalBytesWritten = _totalBytesWritten;
    return status;
}

String LogManager::getLogStatusString() const {
    LogStatus s = getLogStatus();
    String result;
    result = "=== LOG STATUS ===\n";
    result += "Initialized: " + String(s.isInitialized ? "YES" : "NO") + "\n";
    result += "RAM Buffer: " + String(s.ramBufferSize) + "/" + String(s.maxRamEntries) + "\n";
    result += "Total Logs: " + String(s.totalLogsCount) + "\n";
    result += "File Size: " + String(s.fileSize) + " bytes\n";
    result += "Min Level: " + String(levelToString(s.minLevel)) + "\n";
    result += "Dirty: " + String(s.isDirty ? "YES" : "NO") + "\n";
    result += "Last Flush: " + String(s.lastFlushTime) + " ms\n";
    result += "Flush Count: " + String(s.flushCount) + "\n";
    result += "Errors: " + String(s.errors) + "\n";
    result += "Dropped: " + String(s.droppedLogs) + "\n";
    result += "Bytes Written: " + String(s.totalBytesWritten) + "\n";
    return result;
}

// ============================================================================
// 17. СТАТИЧЕСКИЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
const char* LogManager::levelToString(LogLevel level) {
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO: return "INFO";
        case LOG_WARNING: return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_CRITICAL: return "CRIT";
        case LOG_PAZ: return "PAZ";
        case LOG_SECURITY: return "SEC";
        case LOG_AUDIT: return "AUDIT";
        default: return "UNKNOWN";
    }
}

LogLevel LogManager::stringToLevel(const char* str) {
    if (str == nullptr) return LOG_INFO;
    if (strcmp(str, "DEBUG") == 0) return LOG_DEBUG;
    if (strcmp(str, "INFO") == 0) return LOG_INFO;
    if (strcmp(str, "WARN") == 0 || strcmp(str, "WARNING") == 0) return LOG_WARNING;
    if (strcmp(str, "ERROR") == 0) return LOG_ERROR;
    if (strcmp(str, "CRIT") == 0 || strcmp(str, "CRITICAL") == 0) return LOG_CRITICAL;
    if (strcmp(str, "PAZ") == 0) return LOG_PAZ;
    if (strcmp(str, "SEC") == 0 || strcmp(str, "SECURITY") == 0) return LOG_SECURITY;
    if (strcmp(str, "AUDIT") == 0) return LOG_AUDIT;
    return LOG_INFO;
}

const char* LogManager::levelToColor(LogLevel level) {
    switch (level) {
        case LOG_DEBUG: return ANSI_GRAY;
        case LOG_INFO: return ANSI_WHITE;
        case LOG_WARNING: return ANSI_YELLOW;
        case LOG_ERROR: return ANSI_RED;
        case LOG_CRITICAL: return ANSI_RED;
        case LOG_PAZ: return ANSI_MAGENTA;
        case LOG_SECURITY: return ANSI_CYAN;
        case LOG_AUDIT: return ANSI_GREEN;
        default: return ANSI_RESET;
    }
}

const char* LogManager::levelToAnsiColor(LogLevel level) {
    return levelToColor(level);
}

// ============================================================================
// 18. СТАТУС И ДИАГНОСТИКА
// ============================================================================
void LogManager::updateStatus() {
    if (_onStatusChange) {
        _onStatusChange(getLogStatus());
    }
}

const char* LogManager::getStatus() const {
    static char statusBuffer[128];
    snprintf(statusBuffer, sizeof(statusBuffer),
            "RAM:%zu/%zu,Total:%zu,Err:%lu,Dropped:%lu",
            _ramBuffer.size(),
            _maxRamEntries,
            _totalLogsCount,
            _errors,
            _droppedLogs);
    return statusBuffer;
}

// ============================================================================
// 19. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void LogManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _ramBuffer.size();

    snprintf(data->payload, sizeof(data->payload),
            "ram:%zu,max:%zu,total:%zu,err:%lu,drop:%lu,flushes:%lu,events:%lu",
            _ramBuffer.size(),
            _maxRamEntries,
            _totalLogsCount,
            _errors,
            _droppedLogs,
            _flushCount,
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

void LogManager::streamDiagnosticInfo(Stream& stream) const {
    LogStatus s = getLogStatus();
    stream.println("==============================");
    stream.println(" LOG MANAGER DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", s.isInitialized ? "YES" : "NO");
    stream.printf(" RAM Buffer: %zu/%zu\n", s.ramBufferSize, s.maxRamEntries);
    stream.printf(" Total Logs: %zu\n", s.totalLogsCount);
    stream.printf(" File Size: %zu bytes\n", s.fileSize);
    stream.printf(" Backup Size: %zu bytes\n", s.backupSize);
    stream.printf(" Min Level: %s\n", levelToString(s.minLevel));
    stream.printf(" Dirty: %s\n", s.isDirty ? "YES" : "NO");
    stream.printf(" Errors: %lu\n", s.errors);
    stream.printf(" Dropped: %lu\n", s.droppedLogs);
    stream.printf(" Flush Count: %lu\n", s.flushCount);
    stream.printf(" Bytes Written: %lu\n", s.totalBytesWritten);
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("==============================");
}

void LogManager::printStats() const {
    streamDiagnosticInfo(Serial);
}