// ============================================================================
// LogManager.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Полноценная система логирования с поддержкой:
// - RAM буфер с автоматическим сбросом на диск
// - Ротация логов по размеру
// - Событийная модель публикации
// - ANSI-цвета для Serial
// - Поддержка форматированных сообщений
// - Потокобезопасность (рекурсивный мьютекс)
// - Защита от фрагментации кучи
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
// ============================================================================
#include "LogManager.h"
#include <sys/time.h>
#include <stdarg.h>
#include <esp_task_wdt.h>
#include "core/IModule.h"

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static LogManager _logManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
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
    _autoFlush = true;
    _flushIntervalMs = 10000;
    _onLogCallback = nullptr;
    _onBufferFull = nullptr;
    _onStatusChange = nullptr;
    _onErrorCallback = nullptr;

    // Создаем рекурсивный мьютекс
    _logMutex = xSemaphoreCreateRecursiveMutex();
    if (_logMutex == nullptr) {
        Serial.println("[LOG] FATAL: Failed to create mutex!");
        while (1) { delay(100); }
    }

    Serial.println("[LOG] Instance created (v4.2.2)");
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
// 2. СИНГЛТОН
// ============================================================================
LogManager& LogManager::getInstance() {
    return _logManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
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
    // Обрезаем source
    if (source != nullptr) {
        strncpy(trimmedSource, source, srcSize - 1);
        trimmedSource[srcSize - 1] = '\0';
    } else {
        strncpy(trimmedSource, "UNKN", srcSize - 1);
        trimmedSource[srcSize - 1] = '\0';
    }

    // Обрезаем message
    if (message != nullptr) {
        strncpy(trimmedMessage, message, msgSize - 1);
        trimmedMessage[msgSize - 1] = '\0';
    } else {
        trimmedMessage[0] = '\0';
    }
}

void LogManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    // Не используем addLog, чтобы избежать рекурсии
    // Вместо этого пишем напрямую в Serial
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
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void LogManager::init() {
    if (_initialized) return;

    logMessage("Initializing...");

    // 1. Монтируем LittleFS
    if (!ensureDirectoryExists()) {
        logMessage("LittleFS mount failed!");
        _initialized = false;
        return;
    }

    // 2. Проверяем наличие файла логов
    size_t fileSize = getFileSize(_logPath);
    if (fileSize > 0) {
        logMessage("Existing log file size: %zu bytes", fileSize);
    }

    // 3. Восстановление индекса
    if (LittleFS.exists(_indexPath)) {
        File idxFile = LittleFS.open(_indexPath, "r");
        if (idxFile) {
            String countStr = idxFile.readString();
            _totalLogsCount = countStr.toInt();
            idxFile.close();
            logMessage("Restored index: %zu entries", _totalLogsCount);
        }
    }

    // 4. Подписываемся на события
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

    // Добавляем первый лог
    addLog(LOG_INFO, "SYS", "LogManager initialized successfully v4.2.2");
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

    // Сброс WDT
    esp_task_wdt_reset();

    // Отложенный сброс на диск
    if (_autoFlush && _isDirty && (millis() - _dirtyTimestamp > _flushIntervalMs)) {
        flushToDisk();
    }
}

// ============================================================================
// 5. ОБРАБОТКА СОБЫТИЙ
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

    // Обработка команд
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

    // Системные события
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

    // Прикладные события
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
// 6. ОТПРАВКА СОБЫТИЙ (ИСПРАВЛЕНО)
// ============================================================================
void LogManager::publishLogEvent(const LogEntry& entry) {
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

    // Проверка command (защита от 0)
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
// 7. ДОБАВЛЕНИЕ ЛОГОВ (ИСПРАВЛЕНО)
// ============================================================================
void LogManager::addLog(LogLevel level, const char* source, const char* message) {
    if (!_initialized || _logMutex == nullptr) return;
    if (level < _minLevel) return;

    char trimmedSource[MAX_SOURCE_LEN + 1];
    char trimmedMessage[MAX_MESSAGE_LEN + 1];
    trimSourceAndMessage(source, message, trimmedSource, sizeof(trimmedSource),
                         trimmedMessage, sizeof(trimmedMessage));

    bool needFlush = false;

    // Рекурсивный захват мьютекса
    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Проверка переполнения буфера
        if (_ramBuffer.size() >= _maxRamEntries) {
            _ramBuffer.erase(_ramBuffer.begin());
            _droppedLogs++;
            checkBufferFull();
        }

        // Получение времени
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        uint32_t realTimestamp = (timeinfo.tm_year >= 100) ? (uint32_t)now : 0;

        // Создание записи
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

        // Вывод в Serial
        writeToSerial(entry);

        // Колбэк
        if (_onLogCallback != nullptr) {
            _onLogCallback(entry);
        }

        // Отправка события
        publishLogEvent(entry);

        // Запоминаем, нужно ли сбросить
        if (level == LOG_CRITICAL || level == LOG_PAZ) {
            needFlush = true;
        } else if (_autoFlush) {
            scheduleFlush();
        }

        xSemaphoreGiveRecursive(_logMutex);  // <-- ОДИН РАЗ!
    }

    // Сбрасываем ПОСЛЕ освобождения мьютекса
    if (needFlush) {
        flushToDisk();  // flushToDisk() сама берет мьютекс
    }
}

void LogManager::addLog(LogLevel level, const char* source, const String& message) {
    addLog(level, source, message.c_str());
}

void LogManager::addLog(uint8_t rawLevel, const char* source, const char* message) {
    if (rawLevel <= LOG_AUDIT) {
        addLog((LogLevel)rawLevel, source, message);
    }
}

void LogManager::addLogF(LogLevel level, const char* source, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    addLog(level, source, buffer);
}

void LogManager::addLogIf(bool condition, LogLevel level, const char* source, const char* message) {
    if (condition) {
        addLog(level, source, message);
    }
}

// ============================================================================
// 8. СБРОС НА ДИСК (ИСПРАВЛЕНО)
// ============================================================================
void LogManager::flushToDisk() {
    if (!_initialized || _logMutex == nullptr) return;

    // Проверка ротации
    checkLogRotation();

    std::vector<LogEntry> copy;
    copy.reserve(_maxRamEntries);

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (_ramBuffer.empty()) {
            _isDirty = false;
            xSemaphoreGiveRecursive(_logMutex);
            return;
        }

        // Эффективное перемещение (не копирование!)
        copy = std::move(_ramBuffer);
        _ramBuffer.clear();
        _ramBuffer.reserve(_maxRamEntries);

        _isDirty = false;
        _lastFlushTime = millis();
        xSemaphoreGiveRecursive(_logMutex);
    }

    // Запись в файл (без мьютекса)
    if (copy.empty()) return;

    // Проверяем наличие директории
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
            bytesWritten += strlen(entry.message) + 50; // Приблизительно
        }
    }
    logFile.close();

    // Обновляем статистику
    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        _totalLogsCount += copy.size();
        _flushCount++;
        _totalBytesWritten += bytesWritten;

        // Сохранение индекса
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
// 9. РОТАЦИЯ ЛОГОВ
// ============================================================================
void LogManager::checkLogRotation() {
    if (!LittleFS.exists(_logPath)) return;

    size_t size = getFileSize(_logPath);
    if (size > MAX_LOG_FILE_SIZE) {
        // Удаляем старый бэкап
        if (LittleFS.exists(_backupPath)) {
            LittleFS.remove(_backupPath);
        }
        // Переименовываем текущий лог в бэкап
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
// 10. ФОРМАТИРОВАНИЕ В ФАЙЛ
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
// 11. ВЫВОД В SERIAL
// ============================================================================
void LogManager::writeToSerial(const LogEntry& entry) {
    const char* color = levelToColor(entry.level);
    const char* reset = ANSI_RESET;

    char timeBuf[32];
    printFormattedTime(timeBuf, sizeof(timeBuf), entry.uptime_sec, entry.real_time);

    const char* levelStr = levelToString(entry.level);

    // Критические и ошибки выводим с цветом
    if (entry.level >= LOG_ERROR) {
        Serial.printf("%s[%s][%s][%s] %s%s\n",
                     color, timeBuf, levelStr, entry.source, entry.message, reset);
    } else {
        Serial.printf("[%s][%s][%s] %s\n",
                     timeBuf, levelStr, entry.source, entry.message);
    }
}

// ============================================================================
// 12. ФОРМАТИРОВАНИЕ ВРЕМЕНИ
// ============================================================================
void LogManager::printFormattedTime(char* buf, size_t bufSize,
                                   uint32_t uptime, uint32_t realTime) const {
    if (realTime == 0) {
        // Если нет реального времени, показываем uptime
        uint32_t days = uptime / 86400;
        uint32_t hours = (uptime % 86400) / 3600;
        uint32_t minutes = (uptime % 3600) / 60;
        uint32_t seconds = uptime % 60;
        snprintf(buf, bufSize, "UPTIME %02uD %02uh %02um %02us",
                days, hours, minutes, seconds);
    } else {
        time_t t = (time_t)realTime;
        struct tm timeinfo;
        localtime_r(&t, &timeinfo);
        strftime(buf, bufSize, TIME_FORMAT, &timeinfo);
    }
}

// ============================================================================
// 13. УПРАВЛЕНИЕ
// ============================================================================
void LogManager::clearAll() {
    if (!_initialized || _logMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        _ramBuffer.clear();
        _totalLogsCount = 0;
        _isDirty = false;

        if (LittleFS.exists(_logPath)) {
            LittleFS.remove(_logPath);
        }
        if (LittleFS.exists(_backupPath)) {
            LittleFS.remove(_backupPath);
        }
        if (LittleFS.exists(_indexPath)) {
            LittleFS.remove(_indexPath);
        }

        xSemaphoreGiveRecursive(_logMutex);
        publishLogCleared();
        updateStatus();
        addLog(LOG_INFO, "SYS", "All logs cleared");
    }
}

void LogManager::setMaxEntries(size_t maxEntries) {
    if (maxEntries < 10) maxEntries = 10;
    if (maxEntries > 500) maxEntries = 500;

    if (_logMutex && xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _maxRamEntries = maxEntries;
        _ramBuffer.reserve(_maxRamEntries);
        while (_ramBuffer.size() > _maxRamEntries) {
            _ramBuffer.erase(_ramBuffer.begin());
            _droppedLogs++;
        }
        xSemaphoreGiveRecursive(_logMutex);
        addLogF(LOG_INFO, "SYS", "Max entries set to %zu", maxEntries);
    }
}

// ============================================================================
// 14. ПОЛУЧЕНИЕ ЛОГОВ (ИСПРАВЛЕНО — БЕЗ ФРАГМЕНТАЦИИ)
// ============================================================================
bool LogManager::getLogsAsString(size_t limit, String& output) const {
    output.reserve(limit * 200);

    if (_logMutex == nullptr) return false;

    size_t count = 0;
    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t start = (_ramBuffer.size() > limit) ? _ramBuffer.size() - limit : 0;
        for (size_t i = start; i < _ramBuffer.size() && count < limit; i++) {
            const auto& entry = _ramBuffer[i];
            char timeBuf[32];
            printFormattedTime(timeBuf, sizeof(timeBuf), entry.uptime_sec, entry.real_time);

            char line[256];
            snprintf(line, sizeof(line), "%s;%s;%s;%s\n",
                    timeBuf,
                    levelToString(entry.level),
                    entry.source,
                    entry.message);
            output += line;
            count++;
        }
        xSemaphoreGiveRecursive(_logMutex);
    }
    return true;
}

String LogManager::getLogsAsString(size_t limit) {
    String result;
    getLogsAsString(limit, result);
    return result;
}

std::vector<LogEntry> LogManager::getRecentLogs(size_t count) const {
    std::vector<LogEntry> result;
    if (_logMutex == nullptr) return result;

    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t start = (_ramBuffer.size() > count) ? _ramBuffer.size() - count : 0;
        for (size_t i = start; i < _ramBuffer.size(); i++) {
            result.push_back(_ramBuffer[i]);
        }
        xSemaphoreGiveRecursive(_logMutex);
    }
    return result;
}

size_t LogManager::getRamBufferSize() const {
    if (_logMutex == nullptr) return 0;
    size_t size = 0;
    if (xSemaphoreTakeRecursive(_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        size = _ramBuffer.size();
        xSemaphoreGiveRecursive(_logMutex);
    }
    return size;
}

size_t LogManager::getFileSize() const {
    return getFileSize(_logPath);
}

// ============================================================================
// 15. СТАТУС
// ============================================================================
const char* LogManager::getStatus() const {
    static char statusBuffer[256];
    snprintf(statusBuffer, sizeof(statusBuffer),
            "Buffer: %zu/%zu, Total: %zu, Dirty: %s, Errors: %lu, Dropped: %lu",
            getRamBufferSize(),
            _maxRamEntries,
            _totalLogsCount,
            _isDirty ? "YES" : "NO",
            _errors,
            _droppedLogs);
    return statusBuffer;
}

void LogManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _initialized ? 1 : 0;

    snprintf(data->payload, sizeof(data->payload),
            "ver:%s,buf:%zu/%zu,total:%zu,dirty:%d,err:%lu,drop:%lu,flush:%lu",
            getVersion(),
            getRamBufferSize(),
            _maxRamEntries,
            _totalLogsCount,
            _isDirty ? 1 : 0,
            _errors,
            _droppedLogs,
            _flushCount);
    data->payloadLen = strlen(data->payload);
}

LogStatus LogManager::getLogStatus() const {
    LogStatus status;
    status.ramBufferSize = getRamBufferSize();
    status.maxRamEntries = _maxRamEntries;
    status.isDirty = _isDirty;
    status.lastFlushTime = _lastFlushTime;
    status.totalLogsCount = _totalLogsCount;  // <-- ИСПРАВЛЕНО!
    status.fileSize = getFileSize();
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
    String result = "=== LOG MANAGER STATUS ===\n";
    result += "Version: " + String(getVersion()) + "\n";
    result += "Initialized: " + String(s.isInitialized ? "YES" : "NO") + "\n";
    result += "Min Level: " + String(levelToString(s.minLevel)) + "\n";
    result += "Buffer: " + String(s.ramBufferSize) + "/" + String(s.maxRamEntries) + "\n";
    result += "Total Logs: " + String(s.totalLogsCount) + "\n";
    result += "Dirty: " + String(s.isDirty ? "YES" : "NO") + "\n";
    result += "Flushes: " + String(s.flushCount) + "\n";
    result += "Dropped: " + String(s.droppedLogs) + "\n";
    result += "Errors: " + String(s.errors) + "\n";
    result += "File Size: " + String(s.fileSize) + " bytes\n";
    result += "Backup Size: " + String(s.backupSize) + " bytes\n";
    result += "Total Written: " + String(s.totalBytesWritten) + " bytes\n";
    result += "Last Flush: " + String(s.lastFlushTime) + " ms\n";
    return result;
}

void LogManager::checkBufferFull() {
    if (_onBufferFull != nullptr) {
        _onBufferFull(_ramBuffer.size());
    }
    publishBufferFull(_ramBuffer.size());
}

void LogManager::updateStatus() {
    if (_onStatusChange != nullptr) {
        _onStatusChange(getLogStatus());
    }
}

// ============================================================================
// 16. СТАТИЧЕСКИЕ МЕТОДЫ
// ============================================================================
const char* LogManager::levelToString(LogLevel level) {
    switch (level) {
        case LOG_DEBUG:     return "DEBUG";
        case LOG_INFO:      return "INFO";
        case LOG_WARNING:   return "WARN";
        case LOG_ERROR:     return "ERROR";
        case LOG_CRITICAL:  return "CRIT";
        case LOG_PAZ:       return "PAZ";
        case LOG_SECURITY:  return "SEC";
        case LOG_AUDIT:     return "AUDIT";
        default:            return "UNKN";
    }
}

LogLevel LogManager::stringToLevel(const char* str) {
    if (str == nullptr) return LOG_INFO;
    if (strcmp(str, "DEBUG") == 0) return LOG_DEBUG;
    if (strcmp(str, "INFO") == 0) return LOG_INFO;
    if (strcmp(str, "WARN") == 0) return LOG_WARNING;
    if (strcmp(str, "ERROR") == 0) return LOG_ERROR;
    if (strcmp(str, "CRIT") == 0) return LOG_CRITICAL;
    if (strcmp(str, "PAZ") == 0) return LOG_PAZ;
    if (strcmp(str, "SEC") == 0) return LOG_SECURITY;
    if (strcmp(str, "AUDIT") == 0) return LOG_AUDIT;
    return LOG_INFO;
}

const char* LogManager::levelToColor(LogLevel level) {
    switch (level) {
        case LOG_DEBUG:     return ANSI_BLUE;
        case LOG_INFO:      return ANSI_GREEN;
        case LOG_WARNING:   return ANSI_YELLOW;
        case LOG_ERROR:     return ANSI_MAGENTA;
        case LOG_CRITICAL:  return ANSI_RED;
        case LOG_PAZ:       return ANSI_RED;
        case LOG_SECURITY:  return ANSI_CYAN;
        case LOG_AUDIT:     return ANSI_WHITE;
        default:            return ANSI_RESET;
    }
}

const char* LogManager::levelToAnsiColor(LogLevel level) {
    return levelToColor(level);
}

// ============================================================================
// 17. ДИАГНОСТИКА
// ============================================================================
void LogManager::streamDiagnosticInfo(Stream& stream) const {
    LogStatus s = getLogStatus();
    stream.println("====================");
    stream.println(" LOG MANAGER DIAGNOSTIC");
    stream.println("====================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", s.isInitialized ? "YES" : "NO");
    stream.printf(" Min Level: %s\n", levelToString(s.minLevel));
    stream.printf(" Buffer: %zu/%zu entries\n", s.ramBufferSize, s.maxRamEntries);
    stream.printf(" Total Logs: %zu\n", s.totalLogsCount);
    stream.printf(" Flushes: %lu\n", s.flushCount);
    stream.printf(" Dropped: %lu\n", s.droppedLogs);
    stream.printf(" Errors: %lu\n", s.errors);
    stream.printf(" Dirty: %s\n", s.isDirty ? "YES" : "NO");
    stream.printf(" File Size: %zu bytes\n", s.fileSize);
    stream.printf(" Backup Size: %zu bytes\n", s.backupSize);
    stream.printf(" Total Written: %lu bytes\n", s.totalBytesWritten);
    stream.printf(" Last Flush: %lu ms\n", s.lastFlushTime);
    stream.println("====================");
}

void LogManager::printStats() const {
    streamDiagnosticInfo(Serial);
}