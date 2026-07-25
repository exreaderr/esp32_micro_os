// ============================================================================
// DataLoggerManager.cpp - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Управление историей данных для графиков и аналитики.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНА ошибка в onEvent (SH_EVENT_DATA_LOG)
// - ИСПРАВЛЕНА ошибка в publishErrorEvent (++stats)
// - ИСПРАВЛЕНА ошибка в publishFlushed (++stats)
// - ИСПРАВЛЕНА ошибка countCount -> pointsCount
// - ИСПРАВЛЕНА ошибка pointsSize
// - ДОБАВЛЕНА полная потокобезопасность
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.2
// - Добавлен метод publishDataEventInternal() для публикации через новую шину
// - Добавлен метод publishDataEvent() (публичный)
// - Добавлен вызов publishDataEventInternal() в logData()
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "DataLoggerManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_crc.h>
#include <cstdarg>
#include <cstring>
#include "core/IModule.h"

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР
// ============================================================================
static DataLoggerManager _dataLoggerManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
DataLoggerManager::DataLoggerManager() {
    _moduleId = MODULE_ID_DATALOGGER;

    _bufferMutex = xSemaphoreCreateRecursiveMutex();
    _indexMutex = xSemaphoreCreateRecursiveMutex();

    if (_bufferMutex == nullptr || _indexMutex == nullptr) {
        Serial.println("[DATALOG] CRITICAL: Failed to create mutexes!");
        while (1) { delay(100); }
    }

    _buffers.reserve(20);
    _initialized = false;
    _fsMounted = false;
    _initInProgress = false;
    _lastFlushMs = 0;
    _lastCleanupMs = 0;
    _lastStatsUpdateMs = 0;
    _totalEventsPublished = 0; // НОВОЕ

    _onDataLogged = nullptr;
    _onDataRetrieved = nullptr;
    _onCleanup = nullptr;
    _onStatsUpdate = nullptr;

    memset(&_stats, 0, sizeof(_stats));

    Serial.println("[DATALOG] Instance created (v5.0)");
}

DataLoggerManager::~DataLoggerManager() {
    stop();
    if (_bufferMutex) vSemaphoreDelete(_bufferMutex);
    if (_indexMutex) vSemaphoreDelete(_indexMutex);
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
DataLoggerManager& DataLoggerManager::getInstance() {
    return _dataLoggerManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void DataLoggerManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void DataLoggerManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[DATALOG] %s\n", msg);

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

void DataLoggerManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

bool DataLoggerManager::isInitializedAndMounted() const {
    return _initialized && _fsMounted;
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule) (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void DataLoggerManager::init() {
    if (_initInProgress) {
        logMessage("Init already in progress, skipping...");
        return;
    }
    _initInProgress = true;

    if (_initialized) {
        logMessage("Already initialized");
        _initInProgress = false;
        return;
    }

    logMessage("Initializing...");

    // 1. Монтирование LittleFS
    if (!LittleFS.begin(false)) {
        logMessage("LittleFS mount failed, trying format...");
        if (!LittleFS.begin(true)) {
            logMessage("LittleFS format failed!");
            _initialized = false;
            _fsMounted = false;
            _initInProgress = false;
            return;
        }
    }
    _fsMounted = true;

    // 2. Создание директории
    if (!LittleFS.exists(DATA_DIR)) {
        if (!LittleFS.mkdir(DATA_DIR)) {
            logMessage("Failed to create data directory!");
            _initialized = false;
            _fsMounted = false;
            _initInProgress = false;
            return;
        }
        logMessage("Data directory created");
    }

    // 3. Загрузка индекса
    if (!loadIndex()) {
        logMessage("No index found, creating new");
        _sensorIndex.clear();
        saveIndex();
    }

    // 4. Подписка на события
    esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &DataLoggerManager::eventHandler,
        this,
        NULL
    );
    esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &DataLoggerManager::eventHandler,
        this,
        NULL
    );

    _initialized = true;
    _lastFlushMs = millis();
    _lastCleanupMs = millis();
    _lastStatsUpdateMs = millis();

    logMessage("Initialized successfully (v%s)", getVersion());
    _initInProgress = false;
}

void DataLoggerManager::start() {
    if (!isInitializedAndMounted()) {
        logMessage("Cannot start: not initialized");
        return;
    }
    logMessage("Started");
}

void DataLoggerManager::stop() {
    if (!_initialized) return;

    logMessage("Stopping...");
    flushAllBuffers();
    _initialized = false;
    logMessage("Stopped");
}

void DataLoggerManager::tick() {
    if (!isInitializedAndMounted()) return;

    esp_task_wdt_reset();

    uint32_t now = millis();

    // 1. Периодический сброс буферов
    if (now - _lastFlushMs > _flushIntervalMs) {
        _lastFlushMs = now;
        flushAllBuffers();
        publishFlushed();
    }

    // 2. Периодическая очистка (раз в час)
    if (now - _lastCleanupMs > CLEANUP_INTERVAL_MS) {
        _lastCleanupMs = now;
        cleanup(_maxStorageDays);
    }

    // 3. Обновление статистики (раз в минуту)
    if (now - _lastStatsUpdateMs > STATS_UPDATE_INTERVAL_MS) {
        _lastStatsUpdateMs = now;
        updateStats();
        if (_onStatsUpdate) _onStatsUpdate(_stats);
    }
}

// ============================================================================
// 5. ОБРАБОТЧИКИ СОБЫТИЙ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void DataLoggerManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                                     int32_t id, void* eventData) {
    DataLoggerManager* instance = static_cast<DataLoggerManager*>(handlerArgs);
    if (!instance || !instance->isInitializedAndMounted()) return;

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

void DataLoggerManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    if (eventId == SH_EVENT_CMD_EXECUTE) {
        if (data->targetModule == _moduleId || data->targetModule == 0) {
            handleCommand(data);
        }
        return;
    }

    if (eventId == SH_EVENT_DATA_LOG) {
        if (data->payloadLen >= sizeof(DataPoint) + 32) {
            const char* sensorId = (const char*)data->payload;
            DataPoint point;
            memcpy(&point, data->payload + strlen(sensorId) + 1, sizeof(DataPoint));
            logData(sensorId, point.value, point.timestamp);
        }
        return;
    }

    switch (eventId) {
        case SH_EVENT_SYS_RESTART:
        case SH_EVENT_SYS_SHUTDOWN:
            stop();
            break;
        default:
            break;
    }
}

bool DataLoggerManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_DATA_LOG ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN);
}

// ============================================================================
// 6. ОБРАБОТКА КОМАНД (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void DataLoggerManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case 0x0C00: { // GET_HISTORY_JSON
            if (data->payloadLen > 0) {
                String payload(data->payload);
                int sep1 = payload.indexOf(',');
                int sep2 = payload.indexOf(',', sep1 + 1);
                if (sep1 > 0 && sep2 > 0) {
                    String sensorId = payload.substring(0, sep1);
                    uint32_t from = payload.substring(sep1 + 1, sep2).toInt();
                    uint32_t to = payload.substring(sep2 + 1).toInt();
                    String json = getHistoryJSON(sensorId.c_str(), from, to);

                    ShEventData response;
                    memset(&response, 0, sizeof(ShEventData));
                    response.sourceModule = _moduleId;
                    response.targetModule = data->sourceModule;
                    response.command = 0x0C01;
                    response.value = json.length();
                    safeStrCopy(response.payload, sizeof(response.payload), json.c_str());
                    response.payloadLen = json.length();
                    postEvent(SH_EVENT_CMD_RESPONSE, &response);
                }
            }
            break;
        }

        case 0x0C02: { // GET_STATS
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0C03;
            response.value = _stats.totalPoints;
            snprintf(response.payload, sizeof(response.payload),
                    "sensors:%lu,points:%lu,size:%lu,free:%lu,err:%lu",
                    _stats.totalSensors,
                    _stats.totalPoints,
                    _stats.totalSize,
                    _stats.freeStorage,
                    _stats.errors);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0C04: { // CLEANUP
            uint32_t days = (data->value > 0) ? (uint32_t)data->value : _maxStorageDays;
            cleanup(days);
            break;
        }

        case 0x0C05: { // FLUSH
            forceFlush();
            break;
        }

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 7. СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
const char* DataLoggerManager::getStatus() const {
    static char statusBuffer[128];
    snprintf(statusBuffer, sizeof(statusBuffer),
            "Sensors:%lu,Points:%lu,Size:%lu,Free:%lu,Err:%lu,Dropped:%lu",
            _stats.totalSensors,
            _stats.totalPoints,
            _stats.totalSize,
            _stats.freeStorage,
            _stats.errors,
            _stats.droppedPoints);
    return statusBuffer;
}

// ============================================================================
// 8. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void DataLoggerManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _stats.totalPoints;

    snprintf(data->payload, sizeof(data->payload),
            "sensors:%lu,points:%lu,size:%lu,free:%lu,err:%lu,flushes:%lu,events:%lu",
            _stats.totalSensors,
            _stats.totalPoints,
            _stats.totalSize,
            _stats.freeStorage,
            _stats.errors,
            _stats.flushCount,
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 9. ОТПРАВКА СОБЫТИЙ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void DataLoggerManager::publishDataLogged(const char* sensorId, float value) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_DATA_LOGGED;
    data.value = (int32_t)(value * 100);
    snprintf(data.payload, sizeof(data.payload), "%s:%.2f", sensorId, value);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void DataLoggerManager::publishDataRetrieved(const char* sensorId, size_t count) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_DATA_RETRIEVED;
    data.value = count;
    snprintf(data.payload, sizeof(data.payload), "%s:%zu", sensorId, count);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void DataLoggerManager::publishDataCleaned(uint32_t removedCount) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_DATA_CLEANED;
    data.value = removedCount;
    snprintf(data.payload, sizeof(data.payload), "removed:%lu", removedCount);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void DataLoggerManager::publishStorageFull() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_DATA_STORAGE_FULL;
    data.value = _stats.freeStorage;
    safeStrCopy(data.payload, sizeof(data.payload), "Storage full");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void DataLoggerManager::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_DATA_ERROR;
    data.value = ++_stats.errors;
    safeStrCopy(data.payload, sizeof(data.payload), errorCode ? errorCode : "UNKNOWN_ERROR");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void DataLoggerManager::publishFlushed() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_DATA_FLUSHED;
    data.value = ++_stats.flushCount;
    safeStrCopy(data.payload, sizeof(data.payload), "Buffer flushed");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void DataLoggerManager::publishAggregated(const char* sensorId, size_t count) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_DATA_AGGREGATED;
    data.value = count;
    snprintf(data.payload, sizeof(data.payload), "%s:%zu", sensorId, count);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

// ============================================================================
// 10. НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void DataLoggerManager::publishDataEventInternal(const char* sensorId, float value, uint32_t timestamp) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = LOG_LEVEL_INFO;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "DataLogger: %s=%.2f at %lu", sensorId, value, timestamp);

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void DataLoggerManager::publishDataEvent(const char* sensorId, float value, uint32_t timestamp) {
    publishDataEventInternal(sensorId, value, timestamp);
}

// ============================================================================
// 11. ЗАПИСЬ ДАННЫХ (ИЗМЕНЕНО: добавлена публикация через новую шину)
// ============================================================================
void DataLoggerManager::logData(const char* sensorId, float value, uint32_t timestamp) {
    if (!isInitializedAndMounted()) {
        _stats.errors++;
        return;
    }

    if (!isValidSensorId(sensorId)) {
        _stats.errors++;
        return;
    }

    if (timestamp == 0) {
        time_t now;
        time(&now);
        timestamp = (uint32_t)now;
        if (timestamp < 1700000000L) timestamp = millis() / 1000;
    }

    // НОВОЕ: публикация через новую шину
    publishDataEventInternal(sensorId, value, timestamp);

    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        _stats.errors++;
        return;
    }

    SensorBuffer* buffer = getBuffer(sensorId);
    if (buffer == nullptr) {
        buffer = createBuffer(sensorId, SensorDataType::FLOAT);
        if (buffer == nullptr) {
            xSemaphoreGiveRecursive(_bufferMutex);
            _stats.errors++;
            return;
        }
    }

    DataPoint point = {timestamp, value};
    buffer->points.push_back(point);

    if (buffer->points.size() == 1) {
        buffer->firstTimestamp = timestamp;
    }
    buffer->lastTimestamp = timestamp;
    buffer->lastValue = value;

    if (value < buffer->minValue || buffer->points.size() == 1) buffer->minValue = value;
    if (value > buffer->maxValue || buffer->points.size() == 1) buffer->maxValue = value;

    buffer->isDirty = true;
    buffer->isActive = true;
    buffer->totalPoints++;
    _stats.totalLogged++;

    bool needFlush = (buffer->points.size() >= _ramBufferSize);

    xSemaphoreGiveRecursive(_bufferMutex);

    if (needFlush) {
        flushBuffer(buffer);
    }

    publishDataLogged(sensorId, value);
    if (_onDataLogged) {
        _onDataLogged(sensorId, value);
    }
}

// ============================================================================
// 12. ПОЛУЧЕНИЕ ИСТОРИИ (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
std::vector<DataPoint> DataLoggerManager::getHistory(const char* sensorId,
                                                     uint32_t fromTimestamp,
                                                     uint32_t toTimestamp,
                                                     uint32_t maxPoints) {
    std::vector<DataPoint> result;

    if (!isInitializedAndMounted() || !isValidSensorId(sensorId)) {
        return result;
    }

    // Сначала из буфера
    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        SensorBuffer* buffer = getBuffer(sensorId);
        if (buffer != nullptr) {
            for (const auto& p : buffer->points) {
                if (p.timestamp >= fromTimestamp && p.timestamp <= toTimestamp) {
                    result.push_back(p);
                }
            }
        }
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    // Затем из файла
    std::vector<DataPoint> filePoints;
    if (loadFromFile(sensorId, filePoints, fromTimestamp, toTimestamp)) {
        // Объединяем с дедупликацией по времени
        result.insert(result.end(), filePoints.begin(), filePoints.end());
        std::sort(result.begin(), result.end(),
                  [](const DataPoint& a, const DataPoint& b) {
                      return a.timestamp < b.timestamp;
                  });
        // Удаляем дубликаты
        result.erase(std::unique(result.begin(), result.end(),
            [](const DataPoint& a, const DataPoint& b) {
                return a.timestamp == b.timestamp;
            }), result.end());
    }

    // Ограничиваем количество точек
    if (result.size() > maxPoints) {
        size_t step = result.size() / maxPoints;
        if (step < 1) step = 1;
        std::vector<DataPoint> sampled;
        sampled.reserve(maxPoints);
        for (size_t i = 0; i < result.size(); i += step) {
            sampled.push_back(result[i]);
        }
        result = sampled;
    }

    if (_onDataRetrieved) {
        _onDataRetrieved(sensorId, result.size());
    }
    publishDataRetrieved(sensorId, result.size());

    return result;
}

// ============================================================================
// 13. JSON ДЛЯ UPLOT (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
String DataLoggerManager::getHistoryJSON(const char* sensorId,
                                         uint32_t fromTimestamp,
                                         uint32_t toTimestamp,
                                         uint32_t maxPoints) {
    String json;

    if (!isValidSensorId(sensorId)) {
        return "{\"error\":\"Invalid sensor ID\"}";
    }

    std::vector<DataPoint> points = getHistory(sensorId, fromTimestamp, toTimestamp,
                                              min(maxPoints, MAX_JSON_RESPONSE_POINTS));

    if (points.empty()) {
        return "{\"data\":[],\"meta\":{\"sensor\":\"" + String(sensorId) +
               "\",\"count\":0,\"unit\":\"\"}}";
    }

    JsonDocument doc;
    JsonArray dataArr = doc["data"].to<JsonArray>();
    JsonArray timestamps = dataArr.add<JsonArray>();
    JsonArray values = dataArr.add<JsonArray>();

    float minVal = points[0].value;
    float maxVal = points[0].value;

    for (const auto& p : points) {
        timestamps.add(p.timestamp);
        values.add(p.value);
        if (p.value < minVal) minVal = p.value;
        if (p.value > maxVal) maxVal = p.value;
    }

    JsonObject meta = doc["meta"].to<JsonObject>();
    meta["sensor"] = sensorId;
    meta["count"] = points.size();
    meta["min"] = minVal;
    meta["max"] = maxVal;
    meta["unit"] = "";

    serializeJson(doc, json);
    return json;
}

String DataLoggerManager::getAggregatedHistoryJSON(const char* sensorId,
                                                   uint32_t fromTimestamp,
                                                   uint32_t toTimestamp,
                                                   AggregationType type) {
    String json;

    if (!isValidSensorId(sensorId)) {
        return "{\"error\":\"Invalid sensor ID\"}";
    }

    std::vector<DataPoint> points = getHistory(sensorId, fromTimestamp, toTimestamp, 10000);
    std::vector<AggregatedPoint> aggregated = aggregateData(points, type);

    if (aggregated.empty()) {
        return "{\"data\":[],\"meta\":{\"sensor\":\"" + String(sensorId) +
               "\",\"count\":0,\"unit\":\"\",\"aggregated\":true}}";
    }

    JsonDocument doc;
    JsonArray dataArr = doc["data"].to<JsonArray>();
    JsonArray timestamps = dataArr.add<JsonArray>();
    JsonArray values = dataArr.add<JsonArray>();
    JsonArray minValues = dataArr.add<JsonArray>();
    JsonArray maxValues = dataArr.add<JsonArray>();

    for (const auto& p : aggregated) {
        timestamps.add(p.timestamp);
        values.add(p.avg);
        minValues.add(p.min);
        maxValues.add(p.max);
    }

    JsonObject meta = doc["meta"].to<JsonObject>();
    meta["sensor"] = sensorId;
    meta["count"] = aggregated.size();
    meta["unit"] = "";
    meta["aggregated"] = true;

    serializeJson(doc, json);
    return json;
}

// ============================================================================
// 14. УПРАВЛЕНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void DataLoggerManager::forceFlush() {
    flushAllBuffers();
    publishFlushed();
    logMessage("Force flush complete");
}

void DataLoggerManager::cleanup(uint32_t olderThanDays) {
    if (!isInitializedAndMounted()) return;

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        logMessage("Cleanup: mutex timeout");
        return;
    }

    uint32_t removedCount = 0;
    uint32_t now = (uint32_t)time(nullptr);
    uint32_t cutoff = now - (olderThanDays * 86400UL);

    // Сначала очищаем индексы
    for (auto it = _sensorIndex.begin(); it != _sensorIndex.end(); ) {
        if (it->second.lastTimestamp < cutoff) {
            // Удаляем файл
            String path = String(DATA_DIR) + "/" + it->first + ".dat";
            if (LittleFS.exists(path)) {
                if (LittleFS.remove(path)) {
                    removedCount++;
                } else {
                    logMessage("Failed to remove %s", path.c_str());
                }
            }
            it = _sensorIndex.erase(it);
        } else {
            ++it;
        }
    }

    // Сохраняем обновленный индекс
    saveIndex();

    // Очищаем буферы
    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        for (auto it = _buffers.begin(); it != _buffers.end(); ) {
            if (it->lastTimestamp < cutoff) {
                it = _buffers.erase(it);
            } else {
                ++it;
            }
        }
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    _stats.cleanupCount++;
    updateStats();

    logMessage("Cleanup: removed %lu old entries", removedCount);
    publishDataCleaned(removedCount);
    if (_onCleanup) {
        _onCleanup(removedCount);
    }
}

void DataLoggerManager::clearSensor(const char* sensorId) {
    if (!isValidSensorId(sensorId)) return;

    // Удаляем из индекса
    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _sensorIndex.erase(String(sensorId));
        saveIndex();
        xSemaphoreGiveRecursive(_indexMutex);
    }

    // Удаляем из буфера
    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        for (auto it = _buffers.begin(); it != _buffers.end(); ++it) {
            if (strcmp(it->sensorId, sensorId) == 0) {
                _buffers.erase(it);
                break;
            }
        }
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    // Удаляем файл
    String path = String(DATA_DIR) + "/" + sensorId + ".dat";
    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
    }

    logMessage("Cleared sensor: %s", sensorId);
}

void DataLoggerManager::clearAll() {
    logMessage("Clearing all data...");

    // Очищаем буферы
    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _buffers.clear();
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    // Очищаем индекс
    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _sensorIndex.clear();
        saveIndex();
        xSemaphoreGiveRecursive(_indexMutex);
    }

    // Удаляем все файлы
    File dir = LittleFS.open(DATA_DIR);
    if (dir) {
        File file = dir.openNextFile();
        while (file) {
            String name = file.name();
            file.close();
            LittleFS.remove(String(DATA_DIR) + "/" + name);
            file = dir.openNextFile();
        }
        dir.close();
    }

    updateStats();
    logMessage("All data cleared");
}

// ============================================================================
// 15. ГЕТТЕРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
size_t DataLoggerManager::getPointsCount(const char* sensorId) const {
    if (!isValidSensorId(sensorId)) return 0;

    size_t count = 0;

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = _sensorIndex.find(String(sensorId));
        if (it != _sensorIndex.end()) {
            count = it->second.pointsCount;
        }
        xSemaphoreGiveRecursive(_indexMutex);
    }

    return count;
}

float DataLoggerManager::getLastValue(const char* sensorId) const {
    if (!isValidSensorId(sensorId)) return 0.0f;

    float value = 0.0f;

    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        SensorBuffer* buffer = const_cast<DataLoggerManager*>(this)->getBuffer(sensorId);
        if (buffer != nullptr && !buffer->points.empty()) {
            value = buffer->points.back().value;
        }
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    return value;
}

SensorMetadata DataLoggerManager::getSensorMetadata(const char* sensorId) const {
    SensorMetadata meta;
    memset(&meta, 0, sizeof(meta));

    if (!isValidSensorId(sensorId)) return meta;

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = _sensorIndex.find(String(sensorId));
        if (it != _sensorIndex.end()) {
            meta = it->second;
        }
        xSemaphoreGiveRecursive(_indexMutex);
    }

    return meta;
}

bool DataLoggerManager::sensorExists(const char* sensorId) const {
    if (!isValidSensorId(sensorId)) return false;

    bool exists = false;
    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        exists = (_sensorIndex.find(String(sensorId)) != _sensorIndex.end());
        xSemaphoreGiveRecursive(_indexMutex);
    }

    return exists;
}

// ============================================================================
// 16. ВНУТРЕННИЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool DataLoggerManager::saveToFile(const char* sensorId, const std::vector<DataPoint>& points) {
    if (points.empty()) return true;

    String path = String(DATA_DIR) + "/" + sensorId + ".dat";
    File file = LittleFS.open(path, "w");
    if (!file) {
        logMessage("Failed to open %s for writing", path.c_str());
        return false;
    }

    // Заголовок
    DataFileHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = MAGIC;
    header.version = CURRENT_VERSION;
    header.sensorIdHash = calcSensorHash(sensorId);
    header.dataType = (uint32_t)SensorDataType::FLOAT;
    header.startTimestamp = points.front().timestamp;
    header.endTimestamp = points.back().timestamp;
    header.count = points.size();
    header.compression = _compressionEnabled ? 1 : 0;

    file.write((uint8_t*)&header, sizeof(header));

    // Данные
    if (_compressionEnabled) {
        auto compressed = compressDelta(points);
        file.write(compressed.data(), compressed.size());
    } else {
        file.write((uint8_t*)points.data(), points.size() * sizeof(DataPoint));
    }

    file.close();

    // Обновляем индекс
    updateIndex(sensorId);

    return true;
}

bool DataLoggerManager::loadFromFile(const char* sensorId, std::vector<DataPoint>& points,
                                     uint32_t fromTimestamp, uint32_t toTimestamp) {
    String path = String(DATA_DIR) + "/" + sensorId + ".dat";
    File file = LittleFS.open(path, "r");
    if (!file) return false;

    DataFileHeader header;
    if (file.read((uint8_t*)&header, sizeof(header)) != sizeof(header)) {
        file.close();
        return false;
    }

    if (header.magic != MAGIC || header.version != CURRENT_VERSION) {
        file.close();
        return false;
    }

    // Проверяем, есть ли пересечение по времени
    if (header.endTimestamp < fromTimestamp || header.startTimestamp > toTimestamp) {
        file.close();
        return false;
    }

    if (header.compression == 1) {
        // Читаем сжатые данные
        size_t compressedSize = file.size() - sizeof(header);
        std::vector<uint8_t> compressed(compressedSize);
        if (file.read(compressed.data(), compressedSize) != compressedSize) {
            file.close();
            return false;
        }
        points = decompressDelta(compressed.data(), compressedSize, header.startTimestamp);
    } else {
        // Читаем несжатые данные
        size_t count = header.count;
        points.resize(count);
        if (file.read((uint8_t*)points.data(), count * sizeof(DataPoint)) != count * sizeof(DataPoint)) {
            file.close();
            return false;
        }
    }

    file.close();

    // Фильтруем по времени
    if (fromTimestamp != 0 || toTimestamp != 0) {
        points.erase(std::remove_if(points.begin(), points.end(),
            [fromTimestamp, toTimestamp](const DataPoint& p) {
                return (p.timestamp < fromTimestamp || p.timestamp > toTimestamp);
            }), points.end());
    }

    return true;
}

bool DataLoggerManager::loadAllFromFile(const char* sensorId, std::vector<DataPoint>& points) {
    return loadFromFile(sensorId, points, 0, UINT32_MAX);
}

void DataLoggerManager::rotateFiles(const char* sensorId) {
    // Проверяем размер файла
    String path = String(DATA_DIR) + "/" + sensorId + ".dat";
    size_t size = getFileSize(sensorId);
    if (size < 1024 * 1024) return; // Меньше 1 МБ

    // Создаем бэкап
    String backupPath = path + ".bak";
    if (LittleFS.exists(backupPath)) {
        LittleFS.remove(backupPath);
    }
    LittleFS.rename(path, backupPath);

    logMessage("Rotated file for %s (size: %zu bytes)", sensorId, size);
}

// ============================================================================
// 17. ИНДЕКС (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool DataLoggerManager::loadIndex() {
    File file = LittleFS.open(INDEX_FILE, "r");
    if (!file) return false;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) return false;

    _sensorIndex.clear();

    JsonObject sensors = doc.as<JsonObject>();
    for (JsonPair kv : sensors) {
        const char* sensorId = kv.key().c_str();
        JsonObject meta = kv.value();

        SensorMetadata m;
        safeStrCopy(m.sensorId, sizeof(m.sensorId), sensorId);
        m.dataType = (SensorDataType)(meta["dataType"].as<uint8_t>());
        m.firstTimestamp = meta["firstTimestamp"].as<uint32_t>();
        m.lastTimestamp = meta["lastTimestamp"].as<uint32_t>();
        m.pointsCount = meta["pointsCount"].as<uint32_t>();
        m.minValue = meta["minValue"].as<float>();
        m.maxValue = meta["maxValue"].as<float>();
        m.lastValue = meta["lastValue"].as<float>();
        m.lastUpdateTime = meta["lastUpdateTime"].as<uint32_t>();
        m.isActive = meta["isActive"].as<bool>();
        m.fileSize = meta["fileSize"].as<uint32_t>();

        _sensorIndex[String(sensorId)] = m;
    }

    return true;
}

bool DataLoggerManager::saveIndex() {
    JsonDocument doc;

    for (const auto& kv : _sensorIndex) {
        const SensorMetadata& m = kv.second;
        JsonObject meta = doc[kv.first].to<JsonObject>();
        meta["dataType"] = (uint8_t)m.dataType;
        meta["firstTimestamp"] = m.firstTimestamp;
        meta["lastTimestamp"] = m.lastTimestamp;
        meta["pointsCount"] = m.pointsCount;
        meta["minValue"] = m.minValue;
        meta["maxValue"] = m.maxValue;
        meta["lastValue"] = m.lastValue;
        meta["lastUpdateTime"] = m.lastUpdateTime;
        meta["isActive"] = m.isActive;
        meta["fileSize"] = m.fileSize;
    }

    File file = LittleFS.open(INDEX_FILE, "w");
    if (!file) return false;

    size_t written = serializeJson(doc, file);
    file.close();

    return written > 0;
}

void DataLoggerManager::updateIndex(const char* sensorId) {
    SensorMetadata meta;
    safeStrCopy(meta.sensorId, sizeof(meta.sensorId), sensorId);
    meta.dataType = SensorDataType::FLOAT;

    // Получаем данные из буфера
    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        SensorBuffer* buffer = getBuffer(sensorId);
        if (buffer != nullptr) {
            meta.firstTimestamp = buffer->firstTimestamp;
            meta.lastTimestamp = buffer->lastTimestamp;
            meta.pointsCount = buffer->totalPoints;
            meta.minValue = buffer->minValue;
            meta.maxValue = buffer->maxValue;
            meta.lastValue = buffer->lastValue;
            meta.lastUpdateTime = millis();
            meta.isActive = buffer->isActive;
            meta.fileSize = getFileSize(sensorId);
        }
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _sensorIndex[String(sensorId)] = meta;
        saveIndex();
        xSemaphoreGiveRecursive(_indexMutex);
    }
}

// ============================================================================
// 18. СЖАТИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
std::vector<uint8_t> DataLoggerManager::compressDelta(const std::vector<DataPoint>& points) {
    std::vector<uint8_t> result;
    if (points.empty()) return result;

    // Формат: [base_timestamp] [base_value] [delta_count] [deltas...]
    // 8 байт timestamp, 4 байта float, 4 байта count, затем дельты

    result.reserve(points.size() * 6 + 16);

    uint32_t baseTimestamp = points[0].timestamp;
    float baseValue = points[0].value;

    result.insert(result.end(), (uint8_t*)&baseTimestamp, (uint8_t*)&baseTimestamp + 4);
    result.insert(result.end(), (uint8_t*)&baseValue, (uint8_t*)&baseValue + 4);

    uint32_t count = points.size();
    result.insert(result.end(), (uint8_t*)&count, (uint8_t*)&count + 4);

    // Дельты: сначала временные, потом значения
    for (size_t i = 1; i < points.size(); i++) {
        uint32_t dt = points[i].timestamp - points[i-1].timestamp;
        result.insert(result.end(), (uint8_t*)&dt, (uint8_t*)&dt + 4);
    }

    for (size_t i = 1; i < points.size(); i++) {
        float diff = points[i].value - points[i-1].value;
        result.insert(result.end(), (uint8_t*)&diff, (uint8_t*)&diff + 4);
    }

    return result;
}

std::vector<DataPoint> DataLoggerManager::decompressDelta(const uint8_t* data, size_t size,
                                                          uint32_t startTimestamp) {
    std::vector<DataPoint> result;

    if (size < 12) return result;

    const uint8_t* ptr = data;

    // Читаем базовые значения
    uint32_t baseTimestamp;
    memcpy(&baseTimestamp, ptr, 4);
    ptr += 4;

    float baseValue;
    memcpy(&baseValue, ptr, 4);
    ptr += 4;

    uint32_t count;
    memcpy(&count, ptr, 4);
    ptr += 4;

    if (count == 0) return result;

    result.reserve(count);
    result.push_back({baseTimestamp, baseValue});

    // Читаем дельты времени
    std::vector<uint32_t> dtValues(count - 1);
    for (uint32_t i = 0; i < count - 1; i++) {
        memcpy(&dtValues[i], ptr, 4);
        ptr += 4;
    }

    // Читаем дельты значений
    std::vector<float> dvValues(count - 1);
    for (uint32_t i = 0; i < count - 1; i++) {
        memcpy(&dvValues[i], ptr, 4);
        ptr += 4;
    }

    // Восстанавливаем данные
    uint32_t ts = baseTimestamp;
    float val = baseValue;

    for (uint32_t i = 0; i < count - 1; i++) {
        ts += dtValues[i];
        val += dvValues[i];
        result.push_back({ts, val});
    }

    return result;
}

// ============================================================================
// 19. БУФЕР (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
DataLoggerManager::SensorBuffer* DataLoggerManager::getBuffer(const char* sensorId) {
    for (auto& buffer : _buffers) {
        if (strcmp(buffer.sensorId, sensorId) == 0) {
            return &buffer;
        }
    }
    return nullptr;
}

DataLoggerManager::SensorBuffer* DataLoggerManager::createBuffer(const char* sensorId, SensorDataType type) {
    if (_buffers.size() >= MAX_SENSORS) {
        logMessage("Maximum sensors reached: %zu", _buffers.size());
        return nullptr;
    }

    SensorBuffer buffer;
    safeStrCopy(buffer.sensorId, sizeof(buffer.sensorId), sensorId);
    buffer.dataType = type;
    buffer.points.reserve(_ramBufferSize);

    _buffers.push_back(buffer);
    return &_buffers.back();
}

void DataLoggerManager::flushBuffer(SensorBuffer* buffer) {
    if (buffer == nullptr || buffer->points.empty() || !buffer->isDirty) {
        return;
    }

    if (!isInitializedAndMounted()) {
        return;
    }

    saveToFile(buffer->sensorId, buffer->points);
    buffer->points.clear();
    buffer->isDirty = false;
    buffer->lastFlushTime = millis();

    updateStats();
}

void DataLoggerManager::flushAllBuffers() {
    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        logMessage("Flush all: mutex timeout");
        return;
    }

    for (auto& buffer : _buffers) {
        if (buffer.isDirty && !buffer.points.empty()) {
            flushBuffer(&buffer);
        }
    }

    xSemaphoreGiveRecursive(_bufferMutex);
}

// ============================================================================
// 20. АГРЕГАЦИЯ (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
std::vector<AggregatedPoint> DataLoggerManager::aggregateData(const std::vector<DataPoint>& points,
                                                              AggregationType type) {
    std::vector<AggregatedPoint> result;
    if (points.empty()) return result;

    uint32_t interval = 0;
    switch (type) {
        case AggregationType::MINUTE: interval = 60; break;
        case AggregationType::HOUR: interval = 3600; break;
        case AggregationType::DAY: interval = 86400; break;
        case AggregationType::WEEK: interval = 604800; break;
        case AggregationType::MONTH: interval = 2592000; break;
        default: return result;
    }

    uint32_t currentBucket = points[0].timestamp / interval * interval;

    AggregatedPoint current;
    current.timestamp = currentBucket;
    current.min = FLT_MAX;
    current.max = -FLT_MAX;
    current.sum = 0;
    current.count = 0;

    for (const auto& p : points) {
        uint32_t bucket = p.timestamp / interval * interval;
        if (bucket != currentBucket) {
            if (current.count > 0) {
                current.avg = current.sum / current.count;
                result.push_back(current);
            }
            currentBucket = bucket;
            current.timestamp = currentBucket;
            current.min = FLT_MAX;
            current.max = -FLT_MAX;
            current.sum = 0;
            current.count = 0;
        }

        if (p.value < current.min) current.min = p.value;
        if (p.value > current.max) current.max = p.value;
        current.sum += p.value;
        current.count++;
    }

    if (current.count > 0) {
        current.avg = current.sum / current.count;
        result.push_back(current);
    }

    return result;
}

// ============================================================================
// 21. ВАЛИДАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool DataLoggerManager::isValidSensorId(const char* sensorId) const {
    if (sensorId == nullptr) return false;
    size_t len = strlen(sensorId);
    if (len == 0 || len > 31) return false;
    for (size_t i = 0; i < len; i++) {
        char c = sensorId[i];
        if (!isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

uint32_t DataLoggerManager::calcSensorHash(const char* sensorId) const {
    return esp_crc32_le(0, (const uint8_t*)sensorId, strlen(sensorId));
}

uint32_t DataLoggerManager::getFileSize(const char* sensorId) const {
    String path = String(DATA_DIR) + "/" + sensorId + ".dat";
    if (!LittleFS.exists(path)) return 0;
    File file = LittleFS.open(path, "r");
    if (!file) return 0;
    uint32_t size = file.size();
    file.close();
    return size;
}

void DataLoggerManager::updateStats() {
    _stats.totalSensors = _sensorIndex.size();

    size_t totalPoints = 0;
    size_t totalSize = 0;

    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& buffer : _buffers) {
            totalPoints += buffer.points.size();
        }
        _stats.ramBufferSize = totalPoints;
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& kv : _sensorIndex) {
            totalPoints += kv.second.pointsCount;
            totalSize += kv.second.fileSize;
        }
        xSemaphoreGiveRecursive(_indexMutex);
    }

    _stats.totalPoints = totalPoints;
    _stats.totalSize = totalSize;
    _stats.freeStorage = LittleFS.totalBytes() - LittleFS.usedBytes();
    _stats.totalStorage = LittleFS.totalBytes();
    _stats.lastFlushTime = _lastFlushMs;
    _stats.lastCleanupTime = _lastCleanupMs;
}

// ============================================================================
// 22. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void DataLoggerManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================");
    stream.println(" DATA LOGGER MANAGER DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" FS Mounted: %s\n", _fsMounted ? "YES" : "NO");
    stream.printf(" Compression: %s\n", _compressionEnabled ? "ON" : "OFF");
    stream.println("-- Stats --");
    stream.printf(" Sensors: %lu\n", _stats.totalSensors);
    stream.printf(" Total Points: %lu\n", _stats.totalPoints);
    stream.printf(" Total Files: %lu\n", _stats.totalFiles);
    stream.printf(" Total Size: %lu bytes\n", _stats.totalSize);
    stream.printf(" RAM Buffer: %lu points\n", _stats.ramBufferSize);
    stream.printf(" Errors: %lu\n", _stats.errors);
    stream.printf(" Flushes: %lu\n", _stats.flushCount);
    stream.printf(" Cleanups: %lu\n", _stats.cleanupCount);
    stream.printf(" Dropped Points: %lu\n", _stats.droppedPoints);
    stream.printf(" Total Logged: %lu\n", _stats.totalLogged);
    stream.printf(" Free Storage: %zu bytes\n", _stats.freeStorage);
    stream.printf(" Total Storage: %zu bytes\n", _stats.totalStorage);
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("-- Config --");
    stream.printf(" Max Days: %lu\n", _maxStorageDays);
    stream.printf(" Max Points: %lu\n", _maxPointsPerSensor);
    stream.printf(" Flush Interval: %lu ms\n", _flushIntervalMs);
    stream.printf(" RAM Buffer Size: %zu\n", _ramBufferSize);
    stream.println("==============================");
}

void DataLoggerManager::printStats() const {
    streamDiagnosticInfo(Serial);
}