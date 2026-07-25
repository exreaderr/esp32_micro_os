// ============================================================================
// DataLoggerManager.h - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление историей данных для графиков и аналитики.
// - Бинарное хранение с дельта-сжатием в LittleFS
// - Кольцевой буфер в RAM для быстрого доступа
// - Агрегация данных (минута, час, день, неделя, месяц)
// - JSON для uPlot (интерактивные графики)
// - Автоматическая очистка старых данных
// - Публикация событий через новую шину (v5.0)
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНА ошибка в onEvent (SH_EVENT_DATA_LOG)
// - ИСПРАВЛЕНА ошибка в publishErrorEvent (++stats)
// - ИСПРАВЛЕНА ошибка в publishFlushed (++stats)
// - ИСПРАВЛЕНА ошибка countCount -> pointsCount
// - ИСПРАВЛЕНА ошибка pointsSize
// - ДОБАВЛЕНА полная потокобезопасность
// - ДОБАВЛЕНА защита от повторного входа
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Добавлен метод publishDataEvent() для публикации через новую шину
// - Добавлен счетчик _totalEventsPublished для диагностики
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>
#include <map>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>
#include "core/IModule.h"
#include "core/ShEventData.h" // НОВОЕ: для констант событий

// ============================================================================
// 1. КОНСТАНТЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
#define DATA_LOGGER_MAGIC 0x44415441  // 'DATA'
#define DATA_LOGGER_VERSION 1
#define DATA_LOGGER_DIR "/datalog"
#define DATA_LOGGER_INDEX "/datalog/index.json"
#define DATA_LOGGER_MIN_STORAGE_FREE 4096
#define DATA_LOGGER_MAX_JSON_POINTS 2000
#define DATA_LOGGER_MAX_SENSORS 30
#define DATA_LOGGER_DEFAULT_BUFFER_SIZE 100
#define DATA_LOGGER_DEFAULT_FLUSH_INTERVAL_MS 60000
#define DATA_LOGGER_DEFAULT_MAX_DAYS 30
#define DATA_LOGGER_DEFAULT_MAX_POINTS 10000

// ============================================================================
// 2. СОБЫТИЯ DATA LOGGER MANAGER (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
enum DataLoggerEvents : int32_t {
    SH_EVENT_DATA_LOG = SH_EVENT_USER_BASE + 0x0C00,
    SH_EVENT_DATA_LOGGED = SH_EVENT_USER_BASE + 0x0C01,
    SH_EVENT_DATA_RETRIEVED = SH_EVENT_USER_BASE + 0x0C02,
    SH_EVENT_DATA_CLEANED = SH_EVENT_USER_BASE + 0x0C03,
    SH_EVENT_DATA_STORAGE_FULL = SH_EVENT_USER_BASE + 0x0C04,
    SH_EVENT_DATA_ERROR = SH_EVENT_USER_BASE + 0x0C05,
    SH_EVENT_DATA_FLUSHED = SH_EVENT_USER_BASE + 0x0C06,
    SH_EVENT_DATA_AGGREGATED = SH_EVENT_USER_BASE + 0x0C07
};

// ============================================================================
// 3. ТИПЫ И СТРУКТУРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
enum class SensorDataType : uint8_t {
    FLOAT = 0,
    INT = 1,
    UINT = 2,
    BOOL = 3
};

enum class AggregationType : uint8_t {
    RAW = 0,
    MINUTE = 1,
    HOUR = 2,
    DAY = 3,
    WEEK = 4,
    MONTH = 5
};

/**
 * @brief Одна точка данных
 */
struct DataPoint {
    uint32_t timestamp;  // Unix время
    float value;         // Значение
};

/**
 * @brief Агрегированная точка
 */
struct AggregatedPoint {
    uint32_t timestamp;
    float min;
    float max;
    float avg;
    float sum;
    uint32_t count;
};

/**
 * @brief Метаданные датчика
 */
struct SensorMetadata {
    char sensorId[32];
    SensorDataType dataType;
    uint32_t firstTimestamp;
    uint32_t lastTimestamp;
    uint32_t pointsCount;
    float minValue;
    float maxValue;
    float lastValue;
    uint32_t lastUpdateTime;
    bool isActive;
    uint32_t fileSize;
};

/**
 * @brief Статистика DataLoggerManager
 */
struct DataLoggerStats {
    uint32_t totalSensors = 0;
    uint32_t totalPoints = 0;
    uint32_t totalFiles = 0;
    uint32_t totalSize = 0;
    uint32_t ramBufferSize = 0;
    uint32_t errors = 0;
    uint32_t flushCount = 0;
    uint32_t cleanupCount = 0;
    uint32_t lastFlushTime = 0;
    uint32_t lastCleanupTime = 0;
    size_t freeStorage = 0;
    size_t totalStorage = 0;
    uint32_t droppedPoints = 0;
    uint32_t totalLogged = 0;
};

// ============================================================================
// 4. ОСНОВНОЙ КЛАСС (РАСШИРЕН)
// ============================================================================
/**
 * @brief Менеджер логирования данных
 *
 * Синглтон. Обеспечивает:
 * - Хранение временных рядов в LittleFS
 * - Дельта-сжатие для экономии места
 * - RAM буфер для быстрого доступа
 * - Агрегацию данных
 * - JSON для uPlot графиков
 * - Автоматическую очистку старых данных
 * - Публикацию событий через новую шину (v5.0)
 */
class DataLoggerManager : public IModule {
public:
    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    typedef std::function<void(const char* sensorId, float value)> OnDataLoggedCallback;
    typedef std::function<void(const char* sensorId, size_t count)> OnDataRetrievedCallback;
    typedef std::function<void(uint32_t removedCount)> OnCleanupCallback;
    typedef std::function<void(const DataLoggerStats& stats)> OnStatsUpdateCallback;

    // === СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    static DataLoggerManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    DataLoggerManager();
    ~DataLoggerManager();

    // Запрещаем копирование
    DataLoggerManager(const DataLoggerManager&) = delete;
    DataLoggerManager& operator=(const DataLoggerManager&) = delete;

    // === IModule (ВАШ, ДОПОЛНЕН) ===
    const char* getName() const override { return "DataLoggerManager"; }
    const char* getVersion() const override { return "5.0.0"; } // ИЗМЕНЕНО: версия
    uint32_t getModuleId() const override { return MODULE_ID_DATALOGGER; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized && _fsMounted; }

    // === СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ЛОГИРОВАНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    void logData(const char* sensorId, float value, uint32_t timestamp = 0);
    std::vector<DataPoint> getHistory(const char* sensorId,
                                      uint32_t fromTimestamp,
                                      uint32_t toTimestamp,
                                      uint32_t maxPoints = 500);

    // === JSON ДЛЯ ГРАФИКОВ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    String getHistoryJSON(const char* sensorId,
                         uint32_t fromTimestamp,
                         uint32_t toTimestamp,
                         uint32_t maxPoints = 500);
    String getAggregatedHistoryJSON(const char* sensorId,
                                   uint32_t fromTimestamp,
                                   uint32_t toTimestamp,
                                   AggregationType type = AggregationType::HOUR);

    // === УПРАВЛЕНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    void forceFlush();
    void cleanup(uint32_t olderThanDays = 30);
    void clearSensor(const char* sensorId);
    void clearAll();

    // === ГЕТТЕРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    size_t getPointsCount(const char* sensorId) const;
    size_t getSensorsCount() const { return _sensorIndex.size(); }
    float getLastValue(const char* sensorId) const;
    SensorMetadata getSensorMetadata(const char* sensorId) const;
    DataLoggerStats getStats() const { return _stats; }
    bool sensorExists(const char* sensorId) const;

    // === НАСТРОЙКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setMaxStorageDays(uint32_t days) { _maxStorageDays = days; }
    void setMaxPointsPerSensor(uint32_t points) { _maxPointsPerSensor = points; }
    void setFlushInterval(uint32_t ms) { _flushIntervalMs = ms; }
    void setCompressionEnabled(bool enable) { _compressionEnabled = enable; }
    void setRamBufferSize(size_t size) { _ramBufferSize = constrain(size, 10, 500U); }

    // === НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0) ===
    /**
     * @brief Публикует событие о логировании данных через новую событийную шину
     * @param sensorId ID датчика
     * @param value Значение
     * @param timestamp Время (Unix)
     *
     * @note Используется для совместимости с новой архитектурой.
     *       Модули могут подписываться на это событие для получения
     *       данных в реальном времени.
     */
    void publishDataEvent(const char* sensorId, float value, uint32_t timestamp);

    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setOnDataLogged(OnDataLoggedCallback cb) { _onDataLogged = cb; }
    void setOnDataRetrieved(OnDataRetrievedCallback cb) { _onDataRetrieved = cb; }
    void setOnCleanup(OnCleanupCallback cb) { _onCleanup = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }

    // === ДИАГНОСТИКА (ВАША, РАСШИРЕНА) ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ СТРУКТУРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    /**
     * @brief Буфер датчика в RAM (кольцевой)
     */
    struct SensorBuffer {
        char sensorId[32];
        SensorDataType dataType;
        std::vector<DataPoint> points;
        uint32_t lastFlushTime;
        bool isDirty;
        bool isActive;
        uint32_t firstTimestamp;
        uint32_t lastTimestamp;
        float minValue;
        float maxValue;
        float lastValue;
        size_t totalPoints;

        SensorBuffer() : dataType(SensorDataType::FLOAT), lastFlushTime(0),
                        isDirty(false), isActive(false), firstTimestamp(0),
                        lastTimestamp(0), minValue(0.0f), maxValue(0.0f),
                        lastValue(0.0f), totalPoints(0) {
            memset(sensorId, 0, sizeof(sensorId));
        }
    };

    /**
     * @brief Заголовок файла данных
     */
    struct DataFileHeader {
        uint32_t magic;           // 0x44415441 ('DATA')
        uint32_t version;         // 1
        uint32_t sensorIdHash;    // Хеш имени датчика (CRC32)
        uint32_t dataType;        // 0=float, 1=int, 2=uint, 3=bool
        uint32_t startTimestamp;  // Unix время начала
        uint32_t endTimestamp;    // Unix время конца
        uint32_t count;           // Количество точек
        uint32_t compression;     // 0=none, 1=delta
        uint32_t flags;           // Зарезервировано
        uint32_t reserved[4];
    };

    // === ВНУТРЕННИЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    // Файловая система
    bool saveToFile(const char* sensorId, const std::vector<DataPoint>& points);
    bool loadFromFile(const char* sensorId, std::vector<DataPoint>& points,
                     uint32_t fromTimestamp, uint32_t toTimestamp);
    bool loadAllFromFile(const char* sensorId, std::vector<DataPoint>& points);
    void rotateFiles(const char* sensorId);

    // Индекс
    bool loadIndex();
    bool saveIndex();
    void updateIndex(const char* sensorId);

    // Сжатие
    std::vector<uint8_t> compressDelta(const std::vector<DataPoint>& points);
    std::vector<DataPoint> decompressDelta(const uint8_t* data, size_t size,
                                          uint32_t startTimestamp);

    // Буфер
    SensorBuffer* getBuffer(const char* sensorId);
    SensorBuffer* createBuffer(const char* sensorId, SensorDataType type);
    void flushBuffer(SensorBuffer* buffer);
    void flushAllBuffers();

    // Агрегация
    std::vector<AggregatedPoint> aggregateData(const std::vector<DataPoint>& points,
                                              AggregationType type);

    // Валидация
    bool isValidSensorId(const char* sensorId) const;
    uint32_t calcSensorHash(const char* sensorId) const;
    uint32_t getFileSize(const char* sensorId) const;
    void updateStats();

    // Логирование
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    bool isInitializedAndMounted() const;

    // Отправка событий (оригинальные методы)
    void publishDataLogged(const char* sensorId, float value);
    void publishDataRetrieved(const char* sensorId, size_t count);
    void publishDataCleaned(uint32_t removedCount);
    void publishStorageFull();
    void publishErrorEvent(const char* errorCode);
    void publishFlushed();
    void publishAggregated(const char* sensorId, size_t count);

    // === НОВЫЙ МЕТОД: ВНУТРЕННЯЯ ПУБЛИКАЦИЯ ЧЕРЕЗ НОВУЮ ШИНУ ===
    void publishDataEventInternal(const char* sensorId, float value, uint32_t timestamp);

    // === ОБРАБОТЧИКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ДАННЫЕ (ВАШИ, РАСШИРЕНЫ) ===
    // Буферы
    std::vector<SensorBuffer> _buffers;
    SemaphoreHandle_t _bufferMutex = nullptr;  // Рекурсивный!

    // Индекс
    std::map<String, SensorMetadata> _sensorIndex;
    SemaphoreHandle_t _indexMutex = nullptr;  // Рекурсивный!

    // Статистика
    DataLoggerStats _stats;
    uint32_t _moduleId = MODULE_ID_DATALOGGER;

    // НОВОЕ: счетчик опубликованных событий
    uint32_t _totalEventsPublished = 0;

    // Состояние
    bool _initialized = false;
    bool _compressionEnabled = true;
    bool _fsMounted = false;
    bool _initInProgress = false;

    // Настройки
    uint32_t _maxStorageDays = DATA_LOGGER_DEFAULT_MAX_DAYS;
    uint32_t _maxPointsPerSensor = DATA_LOGGER_DEFAULT_MAX_POINTS;
    uint32_t _flushIntervalMs = DATA_LOGGER_DEFAULT_FLUSH_INTERVAL_MS;
    size_t _ramBufferSize = DATA_LOGGER_DEFAULT_BUFFER_SIZE;
    uint32_t _lastFlushMs = 0;
    uint32_t _lastCleanupMs = 0;
    uint32_t _lastStatsUpdateMs = 0;

    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    OnDataLoggedCallback _onDataLogged = nullptr;
    OnDataRetrievedCallback _onDataRetrieved = nullptr;
    OnCleanupCallback _onCleanup = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;

    // Константы
    static constexpr uint32_t MAGIC = DATA_LOGGER_MAGIC;
    static constexpr uint32_t DATA_LOGGER_VER = DATA_LOGGER_VERSION;
    static constexpr const char* DATA_DIR = DATA_LOGGER_DIR;
    static constexpr const char* INDEX_FILE = DATA_LOGGER_INDEX;
    static constexpr uint32_t MIN_STORAGE_FREE_BYTES = DATA_LOGGER_MIN_STORAGE_FREE;
    static constexpr uint32_t MAX_JSON_RESPONSE_POINTS = DATA_LOGGER_MAX_JSON_POINTS;
    static constexpr size_t MAX_SENSORS = DATA_LOGGER_MAX_SENSORS;
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
    static constexpr uint32_t CLEANUP_INTERVAL_MS = 3600000;  // 1 час
    static constexpr uint32_t STATS_UPDATE_INTERVAL_MS = 60000;  // 1 минута
};