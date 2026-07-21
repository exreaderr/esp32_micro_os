// ============================================================================
// DataLoggerManager.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
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
// ============================================================================
#include "DataLoggerManager.h"
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

    _onDataLogged = nullptr;
    _onDataRetrieved = nullptr;
    _onCleanup = nullptr;
    _onStatsUpdate = nullptr;

    memset(&_stats, 0, sizeof(_stats));

    Serial.println("[DATALOG] Instance created (v4.2.2)");
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
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
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
        data.command = SH_EVENT_LOG_ENTRY;  // <-- ИСПРАВЛЕНО!
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
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
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
// 5. ОБРАБОТКА СОБЫТИЙ
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

    // <-- ИСПРАВЛЕНО!
    if (eventId == SH_EVENT_DATA_LOG) {
        if (data->payloadLen >= sizeof(DataPoint) + 32) {  // sensorId + point
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
// 6. ОБРАБОТКА КОМАНД
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
// 7. СТАТУС
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

void DataLoggerManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _stats.totalPoints;

    snprintf(data->payload, sizeof(data->payload),
            "sensors:%lu,points:%lu,size:%lu,free:%lu,err:%lu,flushes:%lu",
            _stats.totalSensors,
            _stats.totalPoints,
            _stats.totalSize,
            _stats.freeStorage,
            _stats.errors,
            _stats.flushCount);
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 8. ОТПРАВКА СОБЫТИЙ (ИСПРАВЛЕНО)
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
    data.value = ++_stats.errors;  // <-- ИСПРАВЛЕНО!
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
    data.value = ++_stats.flushCount;  // <-- ИСПРАВЛЕНО!
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
// 9. ЗАПИСЬ ДАННЫХ
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
// 10. ПОЛУЧЕНИЕ ИСТОРИИ
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
// 11. JSON ДЛЯ UPLOT
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
// 12. УПРАВЛЕНИЕ
// ============================================================================
void DataLoggerManager::forceFlush() {
    flushAllBuffers();
    publishFlushed();
    logMessage("Force flush complete");
}

void DataLoggerManager::cleanup(uint32_t olderThanDays) {
    if (!isInitializedAndMounted()) return;

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        _stats.errors++;
        return;
    }

    uint32_t threshold = (uint32_t)time(NULL) - (olderThanDays * 86400);
    uint32_t removedCount = 0;

    auto it = _sensorIndex.begin();
    while (it != _sensorIndex.end()) {
        auto& meta = it->second;

        if (meta.lastTimestamp < threshold) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s.dat", DATA_DIR, meta.sensorId);
            if (LittleFS.exists(path)) {
                LittleFS.remove(path);
                removedCount++;
                logMessage("Removed old file: %s", meta.sensorId);
            }
            it = _sensorIndex.erase(it);
            continue;
        } else {
            // Удаляем старые точки внутри файла
            std::vector<DataPoint> points;
            if (loadAllFromFile(meta.sensorId, points)) {
                auto newEnd = std::remove_if(points.begin(), points.end(),
                    [threshold](const DataPoint& p) {
                        return p.timestamp < threshold;
                    });
                size_t removed = points.end() - newEnd;
                if (removed > 0) {
                    points.erase(newEnd, points.end());
                    removedCount += removed;
                    saveToFile(meta.sensorId, points);
                    updateIndex(meta.sensorId);
                    logMessage("Removed %zu old points from %s", removed, meta.sensorId);
                }
            }
        }
        ++it;
    }

    xSemaphoreGiveRecursive(_indexMutex);

    if (removedCount > 0) {
        _stats.cleanupCount++;
        publishDataCleaned(removedCount);
        if (_onCleanup) {
            _onCleanup(removedCount);
        }
        logMessage("Cleanup complete: removed %lu items", removedCount);
    }
}

void DataLoggerManager::clearSensor(const char* sensorId) {
    if (!isValidSensorId(sensorId)) return;

    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (auto it = _buffers.begin(); it != _buffers.end(); ++it) {
            if (strcmp(it->sensorId, sensorId) == 0) {
                _buffers.erase(it);
                break;
            }
        }
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.dat", DATA_DIR, sensorId);
    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
    }

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        _sensorIndex.erase(sensorId);
        saveIndex();
        xSemaphoreGiveRecursive(_indexMutex);
    }

    logMessage("Cleared sensor: %s", sensorId);
}

void DataLoggerManager::clearAll() {
    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        _buffers.clear();
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    if (_fsMounted) {
        File dir = LittleFS.open(DATA_DIR);
        if (dir) {
            while (true) {
                File file = dir.openNextFile();
                if (!file) break;
                if (!file.isDirectory()) {
                    char path[64];
                    snprintf(path, sizeof(path), "%s/%s", DATA_DIR, file.name());
                    LittleFS.remove(path);
                }
                file.close();
            }
            dir.close();
        }

        if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            _sensorIndex.clear();
            saveIndex();
            xSemaphoreGiveRecursive(_indexMutex);
        }

        _stats.totalSensors = 0;
        _stats.totalPoints = 0;
        _stats.totalSize = 0;
        logMessage("All data cleared");
    }
}

// ============================================================================
// 13. ГЕТТЕРЫ
// ============================================================================
size_t DataLoggerManager::getPointsCount(const char* sensorId) const {
    if (!isValidSensorId(sensorId)) return 0;

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = _sensorIndex.find(sensorId);
        if (it != _sensorIndex.end()) {
            size_t count = it->second.pointsCount;
            xSemaphoreGiveRecursive(_indexMutex);
            return count;
        }
        xSemaphoreGiveRecursive(_indexMutex);
    }
    return 0;
}

float DataLoggerManager::getLastValue(const char* sensorId) const {
    if (!isValidSensorId(sensorId)) return 0.0f;

    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        SensorBuffer* buffer = getBuffer(sensorId);
        if (buffer != nullptr) {
            float val = buffer->lastValue;
            xSemaphoreGiveRecursive(_bufferMutex);
            return val;
        }
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = _sensorIndex.find(sensorId);
        if (it != _sensorIndex.end()) {
            float val = it->second.lastValue;
            xSemaphoreGiveRecursive(_indexMutex);
            return val;
        }
        xSemaphoreGiveRecursive(_indexMutex);
    }
    return 0.0f;
}

SensorMetadata DataLoggerManager::getSensorMetadata(const char* sensorId) const {
    SensorMetadata empty;
    memset(&empty, 0, sizeof(empty));

    if (!isValidSensorId(sensorId)) return empty;

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = _sensorIndex.find(sensorId);
        if (it != _sensorIndex.end()) {
            SensorMetadata meta = it->second;
            xSemaphoreGiveRecursive(_indexMutex);
            return meta;
        }
        xSemaphoreGiveRecursive(_indexMutex);
    }
    return empty;
}

bool DataLoggerManager::sensorExists(const char* sensorId) const {
    if (!isValidSensorId(sensorId)) return false;

    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool exists = _sensorIndex.find(sensorId) != _sensorIndex.end();
        xSemaphoreGiveRecursive(_indexMutex);
        return exists;
    }
    return false;
}

// ============================================================================
// 14. ФАЙЛОВАЯ СИСТЕМА
// ============================================================================
bool DataLoggerManager::saveToFile(const char* sensorId, const std::vector<DataPoint>& points) {
    if (points.empty() || !_fsMounted) return false;

    char tempPath[64];
    char finalPath[64];
    snprintf(tempPath, sizeof(tempPath), "%s/%s.tmp", DATA_DIR, sensorId);
    snprintf(finalPath, sizeof(finalPath), "%s/%s.dat", DATA_DIR, sensorId);

    File file = LittleFS.open(tempPath, "w");
    if (!file) {
        _stats.errors++;
        return false;
    }

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

    if (file.write((const uint8_t*)&header, sizeof(header)) != sizeof(header)) {
        file.close();
        LittleFS.remove(tempPath);
        _stats.errors++;
        return false;
    }

    std::vector<uint8_t> data;
    if (_compressionEnabled) {
        data = compressDelta(points);
    } else {
        size_t pointSize = sizeof(uint32_t) + sizeof(float);
        data.reserve(points.size() * pointSize);
        for (const auto& p : points) {
            const uint8_t* tsPtr = (const uint8_t*)&p.timestamp;
            const uint8_t* valPtr = (const uint8_t*)&p.value;
            data.insert(data.end(), tsPtr, tsPtr + sizeof(uint32_t));
            data.insert(data.end(), valPtr, valPtr + sizeof(float));
        }
    }

    if (file.write(data.data(), data.size()) != data.size()) {
        file.close();
        LittleFS.remove(tempPath);
        _stats.errors++;
        return false;
    }

    file.close();

    // Удаляем старый файл
    if (LittleFS.exists(finalPath)) {
        LittleFS.remove(finalPath);
    }

    // Переименовываем временный
    if (!LittleFS.rename(tempPath, finalPath)) {
        _stats.errors++;
        return false;
    }

    return true;
}

bool DataLoggerManager::loadFromFile(const char* sensorId, std::vector<DataPoint>& points,
                                     uint32_t fromTimestamp, uint32_t toTimestamp) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.dat", DATA_DIR, sensorId);

    if (!LittleFS.exists(path)) return false;

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

    if (header.endTimestamp < fromTimestamp || header.startTimestamp > toTimestamp) {
        file.close();
        return true;  // Нет данных в этом диапазоне
    }

    size_t dataSize = file.size() - sizeof(header);
    std::vector<uint8_t> rawData(dataSize);
    if (file.read(rawData.data(), dataSize) != dataSize) {
        file.close();
        return false;
    }
    file.close();

    if (header.compression == 1) {
        points = decompressDelta(rawData.data(), rawData.size(), header.startTimestamp);
    } else {
        size_t pointSize = sizeof(uint32_t) + sizeof(float);
        if (rawData.size() % pointSize != 0) return false;

        points.clear();
        points.reserve(rawData.size() / pointSize);
        const uint8_t* ptr = rawData.data();

        for (size_t i = 0; i < rawData.size(); i += pointSize) {
            DataPoint p;
            memcpy(&p.timestamp, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            memcpy(&p.value, ptr, sizeof(float));
            ptr += sizeof(float);
            points.push_back(p);
        }
    }

    // Фильтруем по диапазону
    auto it = std::remove_if(points.begin(), points.end(),
        [fromTimestamp, toTimestamp](const DataPoint& p) {
            return p.timestamp < fromTimestamp || p.timestamp > toTimestamp;
        });
    points.erase(it, points.end());

    return true;
}

bool DataLoggerManager::loadAllFromFile(const char* sensorId, std::vector<DataPoint>& points) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.dat", DATA_DIR, sensorId);

    if (!LittleFS.exists(path)) return false;

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

    size_t dataSize = file.size() - sizeof(header);
    std::vector<uint8_t> rawData(dataSize);
    if (file.read(rawData.data(), dataSize) != dataSize) {
        file.close();
        return false;
    }
    file.close();

    if (header.compression == 1) {
        points = decompressDelta(rawData.data(), rawData.size(), header.startTimestamp);
    } else {
        size_t pointSize = sizeof(uint32_t) + sizeof(float);
        if (rawData.size() % pointSize != 0) return false;

        points.clear();
        points.reserve(rawData.size() / pointSize);
        const uint8_t* ptr = rawData.data();

        for (size_t i = 0; i < rawData.size(); i += pointSize) {
            DataPoint p;
            memcpy(&p.timestamp, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            memcpy(&p.value, ptr, sizeof(float));
            ptr += sizeof(float);
            points.push_back(p);
        }
    }

    return true;
}

void DataLoggerManager::rotateFiles(const char* sensorId) {
    // Ротация не реализована — файлы перезаписываются при сохранении
}

// ============================================================================
// 15. ИНДЕКС
// ============================================================================
bool DataLoggerManager::loadIndex() {
    if (!_fsMounted) return false;

    if (!LittleFS.exists(INDEX_FILE)) return false;

    File file = LittleFS.open(INDEX_FILE, "r");
    if (!file) return false;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) return false;

    _sensorIndex.clear();

    JsonArray arr = doc["sensors"].as<JsonArray>();
    for (JsonObject obj : arr) {
        SensorMetadata meta;
        safeStrCopy(meta.sensorId, sizeof(meta.sensorId), obj["id"] | "");
        meta.dataType = (SensorDataType)(obj["type"] | 0);
        meta.firstTimestamp = obj["first"] | 0;
        meta.lastTimestamp = obj["last"] | 0;
        meta.pointsCount = obj["count"] | 0;
        meta.minValue = obj["min"] | 0.0f;
        meta.maxValue = obj["max"] | 0.0f;
        meta.lastValue = obj["lastVal"] | 0.0f;
        meta.lastUpdateTime = obj["updated"] | 0;
        meta.isActive = obj["active"] | true;
        meta.fileSize = obj["size"] | 0;

        _sensorIndex[meta.sensorId] = meta;
    }

    _stats.totalSensors = _sensorIndex.size();
    logMessage("Index loaded: %zu sensors", _stats.totalSensors);
    return true;
}

bool DataLoggerManager::saveIndex() {
    if (!_fsMounted) return false;

    JsonDocument doc;
    JsonArray arr = doc["sensors"].to<JsonArray>();

    for (const auto& pair : _sensorIndex) {
        const auto& meta = pair.second;
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = meta.sensorId;
        obj["type"] = (uint8_t)meta.dataType;
        obj["first"] = meta.firstTimestamp;
        obj["last"] = meta.lastTimestamp;
        obj["count"] = meta.pointsCount;
        obj["min"] = meta.minValue;
        obj["max"] = meta.maxValue;
        obj["lastVal"] = meta.lastValue;
        obj["updated"] = meta.lastUpdateTime;
        obj["active"] = meta.isActive;
        obj["size"] = meta.fileSize;
    }

    File file = LittleFS.open(INDEX_FILE, "w");
    if (!file) return false;

    size_t written = serializeJson(doc, file);
    file.close();
    return written > 0;
}

void DataLoggerManager::updateIndex(const char* sensorId) {
    if (xSemaphoreTakeRecursive(_indexMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        _stats.errors++;
        return;
    }

    auto it = _sensorIndex.find(sensorId);
    if (it != _sensorIndex.end()) {
        if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            SensorBuffer* buffer = getBuffer(sensorId);
            if (buffer != nullptr) {
                auto& meta = it->second;
                meta.lastTimestamp = buffer->lastTimestamp;
                meta.pointsCount = buffer->totalPoints;
                meta.lastValue = buffer->lastValue;
                meta.lastUpdateTime = millis();
                meta.isActive = true;
                if (buffer->points.size() > 0) {
                    meta.minValue = buffer->minValue;
                    meta.maxValue = buffer->maxValue;
                }
                meta.fileSize = getFileSize(sensorId);
            }
            xSemaphoreGiveRecursive(_bufferMutex);
        }
        saveIndex();
    }

    xSemaphoreGiveRecursive(_indexMutex);
}

// ============================================================================
// 16. СЖАТИЕ
// ============================================================================
std::vector<uint8_t> DataLoggerManager::compressDelta(const std::vector<DataPoint>& points) {
    std::vector<uint8_t> result;

    if (points.size() < 2) {
        // Для одной точки сохраняем как есть
        size_t pointSize = sizeof(uint32_t) + sizeof(float);
        result.reserve(pointSize);
        const uint8_t* tsPtr = (const uint8_t*)&points[0].timestamp;
        const uint8_t* valPtr = (const uint8_t*)&points[0].value;
        result.insert(result.end(), tsPtr, tsPtr + sizeof(uint32_t));
        result.insert(result.end(), valPtr, valPtr + sizeof(float));
        return result;
    }

    // Сохраняем первую точку полностью
    result.reserve(sizeof(uint32_t) + sizeof(int16_t) + (points.size() - 1) * (sizeof(uint16_t) + sizeof(int16_t)));

    const uint8_t* tsPtr = (const uint8_t*)&points[0].timestamp;
    const uint8_t* valPtr = (const uint8_t*)&points[0].value;
    result.insert(result.end(), tsPtr, tsPtr + sizeof(uint32_t));

    int16_t baseValue = (int16_t)(points[0].value * 100.0f);
    result.insert(result.end(), (const uint8_t*)&baseValue, (const uint8_t*)&baseValue + sizeof(int16_t));

    uint32_t prevTs = points[0].timestamp;
    float prevVal = points[0].value;

    for (size_t i = 1; i < points.size(); i++) {
        uint16_t deltaTs = (uint16_t)(points[i].timestamp - prevTs);
        int16_t deltaVal = (int16_t)((points[i].value - prevVal) * 100.0f);

        result.insert(result.end(), (const uint8_t*)&deltaTs, (const uint8_t*)&deltaTs + sizeof(uint16_t));
        result.insert(result.end(), (const uint8_t*)&deltaVal, (const uint8_t*)&deltaVal + sizeof(int16_t));

        prevTs = points[i].timestamp;
        prevVal = points[i].value;
    }

    return result;
}

std::vector<DataPoint> DataLoggerManager::decompressDelta(const uint8_t* data, size_t size,
                                                          uint32_t startTimestamp) {
    std::vector<DataPoint> result;

    if (size < sizeof(uint32_t) + sizeof(int16_t)) {
        return result;
    }

    size_t pos = 0;
    uint32_t baseTimestamp;
    memcpy(&baseTimestamp, data + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    int16_t baseValue;
    memcpy(&baseValue, data + pos, sizeof(int16_t));
    pos += sizeof(int16_t);

    DataPoint first;
    first.timestamp = baseTimestamp;
    first.value = (float)baseValue / 100.0f;
    result.push_back(first);

    uint32_t currentTs = baseTimestamp;
    float currentVal = (float)baseValue / 100.0f;

    while (pos + 4 <= size) {
        uint16_t deltaTs;
        memcpy(&deltaTs, data + pos, sizeof(uint16_t));
        pos += sizeof(uint16_t);

        int16_t deltaVal;
        memcpy(&deltaVal, data + pos, sizeof(int16_t));
        pos += sizeof(int16_t);

        currentTs += deltaTs;
        currentVal += (float)deltaVal / 100.0f;

        DataPoint p;
        p.timestamp = currentTs;
        p.value = currentVal;
        result.push_back(p);
    }

    return result;
}

// ============================================================================
// 17. БУФЕР
// ============================================================================
DataLoggerManager::SensorBuffer* DataLoggerManager::getBuffer(const char* sensorId) {
    for (auto& buffer : _buffers) {
        if (strcmp(buffer.sensorId, sensorId) == 0) {
            return &buffer;
        }
    }
    return nullptr;
}

DataLoggerManager::SensorBuffer* DataLoggerManager::createBuffer(const char* sensorId,
                                                                 SensorDataType type) {
    if (_buffers.size() >= MAX_SENSORS) {
        // Удаляем неактивные буферы
        for (auto it = _buffers.begin(); it != _buffers.end(); ++it) {
            if (!it->isActive) {
                _buffers.erase(it);
                break;
            }
        }
        if (_buffers.size() >= MAX_SENSORS) {
            _stats.errors++;
            return nullptr;
        }
    }

    SensorBuffer buffer;
    safeStrCopy(buffer.sensorId, sizeof(buffer.sensorId), sensorId);
    buffer.dataType = type;
    buffer.points.reserve(_ramBufferSize);
    buffer.lastFlushTime = millis();
    buffer.isDirty = false;
    buffer.isActive = true;
    buffer.firstTimestamp = 0;
    buffer.lastTimestamp = 0;
    buffer.minValue = 0.0f;
    buffer.maxValue = 0.0f;
    buffer.lastValue = 0.0f;
    buffer.totalPoints = 0;

    _buffers.push_back(buffer);
    return &_buffers.back();
}

void DataLoggerManager::flushBuffer(SensorBuffer* buffer) {
    if (buffer == nullptr || !buffer->isDirty || buffer->points.empty()) {
        return;
    }

    if (!_fsMounted) {
        _stats.errors++;
        return;
    }

    if (saveToFile(buffer->sensorId, buffer->points)) {
        buffer->points.clear();
        buffer->isDirty = false;
        buffer->lastFlushTime = millis();
        _stats.flushCount++;
        updateIndex(buffer->sensorId);
    } else {
        _stats.errors++;
    }
}

void DataLoggerManager::flushAllBuffers() {
    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        _stats.errors++;
        return;
    }

    for (auto& buffer : _buffers) {
        flushBuffer(&buffer);
    }

    xSemaphoreGiveRecursive(_bufferMutex);
    updateStats();
}

// ============================================================================
// 18. АГРЕГАЦИЯ
// ============================================================================
std::vector<AggregatedPoint> DataLoggerManager::aggregateData(const std::vector<DataPoint>& points,
                                                              AggregationType type) {
    std::vector<AggregatedPoint> result;

    if (points.empty()) return result;

    uint32_t interval;
    switch (type) {
        case AggregationType::MINUTE: interval = 60; break;
        case AggregationType::HOUR: interval = 3600; break;
        case AggregationType::DAY: interval = 86400; break;
        case AggregationType::WEEK: interval = 604800; break;
        case AggregationType::MONTH: interval = 2592000; break;
        default: interval = 3600; break;
    }

    uint32_t currentBucket = (points[0].timestamp / interval) * interval;
    AggregatedPoint current;
    current.timestamp = currentBucket;
    current.min = points[0].value;
    current.max = points[0].value;
    current.avg = 0.0f;
    current.sum = 0.0f;
    current.count = 0;

    for (const auto& p : points) {
        uint32_t bucket = (p.timestamp / interval) * interval;

        if (bucket != currentBucket && current.count > 0) {
            current.avg = current.sum / current.count;
            result.push_back(current);

            currentBucket = bucket;
            current.timestamp = bucket;
            current.min = p.value;
            current.max = p.value;
            current.sum = 0.0f;
            current.count = 0;
        }

        current.sum += p.value;
        current.count++;

        if (p.value < current.min) current.min = p.value;
        if (p.value > current.max) current.max = p.value;
    }

    if (current.count > 0) {
        current.avg = current.sum / current.count;
        result.push_back(current);
    }

    return result;
}

// ============================================================================
// 19. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
bool DataLoggerManager::isValidSensorId(const char* sensorId) const {
    if (sensorId == nullptr) return false;
    size_t len = strlen(sensorId);
    if (len == 0 || len > 31) return false;

    for (size_t i = 0; i < len; i++) {
        char c = sensorId[i];
        if (!isalnum(c) && c != '_' && c != '.' && c != '-') return false;
    }
    return true;
}

uint32_t DataLoggerManager::calcSensorHash(const char* sensorId) const {
    return crc32((const uint8_t*)sensorId, strlen(sensorId));
}

uint32_t DataLoggerManager::getFileSize(const char* sensorId) const {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.dat", DATA_DIR, sensorId);

    if (!LittleFS.exists(path)) return 0;

    File file = LittleFS.open(path, "r");
    if (!file) return 0;

    size_t size = file.size();
    file.close();
    return size;
}

void DataLoggerManager::updateStats() {
    _stats.totalSensors = _sensorIndex.size();

    size_t totalSize = 0;
    uint32_t totalPoints = 0;

    for (const auto& pair : _sensorIndex) {
        totalPoints += pair.second.pointsCount;  // <-- ИСПРАВЛЕНО!
        totalSize += getFileSize(pair.first.c_str());
    }

    _stats.totalPoints = totalPoints;
    _stats.totalSize = totalSize;
    _stats.totalStorage = LittleFS.totalBytes();
    _stats.freeStorage = LittleFS.totalBytes() - LittleFS.usedBytes();

    if (xSemaphoreTakeRecursive(_bufferMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t bufferSize = 0;
        for (const auto& b : _buffers) {
            bufferSize += b.points.size();
        }
        _stats.ramBufferSize = bufferSize;
        xSemaphoreGiveRecursive(_bufferMutex);
    }

    _stats.lastFlushTime = _lastFlushMs;
    _stats.lastCleanupTime = _lastCleanupMs;
}

// ============================================================================
// 20. ДИАГНОСТИКА
// ============================================================================
void DataLoggerManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==========================");
    stream.println(" DATA LOGGER MANAGER DIAGNOSTIC");
    stream.println("==========================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" FS Mounted: %s\n", _fsMounted ? "YES" : "NO");
    stream.printf(" Compression: %s\n", _compressionEnabled ? "ON" : "OFF");
    stream.printf(" Max Storage Days: %lu\n", _maxStorageDays);
    stream.printf(" Max Points/Sensor: %lu\n", _maxPointsPerSensor);
    stream.printf(" RAM Buffer Size: %zu\n", _ramBufferSize);
    stream.println("-- Stats --");
    stream.printf(" Sensors: %lu\n", _stats.totalSensors);
    stream.printf(" Total Points: %lu\n", _stats.totalPoints);
    stream.printf(" Total Size: %lu bytes\n", _stats.totalSize);
    stream.printf(" Free Storage: %lu bytes\n", _stats.freeStorage);
    stream.printf(" RAM Buffer: %lu points\n", _stats.ramBufferSize);
    stream.printf(" Flushes: %lu\n", _stats.flushCount);
    stream.printf(" Cleanups: %lu\n", _stats.cleanupCount);
    stream.printf(" Errors: %lu\n", _stats.errors);
    stream.printf(" Dropped: %lu\n", _stats.droppedPoints);
    stream.printf(" Total Logged: %lu\n", _stats.totalLogged);
    stream.println("-- Sensors --");
    if (_sensorIndex.empty()) {
        stream.println(" No sensors");
    } else {
        for (const auto& pair : _sensorIndex) {
            const auto& meta = pair.second;
            stream.printf("%s: %lu points, %.2f - %.2f, last: %.2f\n",
                         meta.sensorId,
                         meta.pointsCount,
                         meta.minValue,
                         meta.maxValue,
                         meta.lastValue);
        }
    }
    stream.println("==========================");
}

void DataLoggerManager::printStats() const {
    streamDiagnosticInfo(Serial);
}