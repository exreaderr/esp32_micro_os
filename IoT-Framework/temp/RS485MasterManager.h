// ============================================================================
// RS485MasterManager.h - ULTIMATE MICRO-OS V4.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление RS485 шиной для связи с Помощниками.
// - Обнаружение Помощников при старте
// - Циклический опрос датчиков
// - Отправка команд управления (LED, звук, дисплей)
// ============================================================================
#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <vector>
#include <map>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "core/IModule.h"

// ============================================================================
// 1. СОБЫТИЯ RS485 MASTER
// ============================================================================

enum RS485MasterEvents : int32_t {
    SH_EVENT_RS485_SLAVE_FOUND = SH_EVENT_USER_BASE + 0x0D00,   // Помощник найден
    SH_EVENT_RS485_SLAVE_LOST = SH_EVENT_USER_BASE + 0x0D01,    // Помощник потерян
    SH_EVENT_RS485_SLAVE_DATA = SH_EVENT_USER_BASE + 0x0D02,    // Данные от Помощника
    SH_EVENT_RS485_SLAVE_ERROR = SH_EVENT_USER_BASE + 0x0D03,   // Ошибка Помощника
    SH_EVENT_RS485_COMMAND_SENT = SH_EVENT_USER_BASE + 0x0D04,  // Команда отправлена
    SH_EVENT_RS485_SCAN_COMPLETE = SH_EVENT_USER_BASE + 0x0D05, // Сканирование завершено
    SH_EVENT_RS485_BUS_ERROR = SH_EVENT_USER_BASE + 0x0D06     // Ошибка шины
};

// ============================================================================
// 2. СТРУКТУРЫ ДАННЫХ
// ============================================================================

/**
 * @brief Тип датчика Помощника
 */
enum class SlaveSensorType : uint8_t {
    UNKNOWN = 0,
    TEMPERATURE = 1,
    HUMIDITY = 2,
    PRESSURE = 3,
    ILLUMINANCE = 4,      // Освещенность (люкс)
    AIR_QUALITY = 5,      // Качество воздуха (TVOC, CO2)
    PRESENCE = 6,         // Радар присутствия
    LED_RGB = 7,          // RGB светодиод
    OLED = 8,             // OLED дисплей
    AUDIO = 9             // DFPlayer
};

/**
 * @brief Информация о датчике Помощника
 */
struct SlaveSensorInfo {
    uint8_t sensorId;                 // Локальный ID на Помощнике
    SlaveSensorType type;             // Тип датчика
    String name;                      // Имя (из конфига)
    String unit;                      // Единица измерения
    float value;                      // Текущее значение (нормализованное)
    bool available;                   // Доступен
    uint32_t lastUpdate;              // Время последнего обновления
    uint8_t i2cAddress;              // I2C адрес (если применимо)
};

/**
 * @brief Информация о Помощнике
 */
struct SlaveInfo {
    uint8_t slaveId;                  // ID Помощника (1-255)
    String firmware;                  // Версия прошивки
    uint32_t uptime;                  // Время работы (сек)
    bool online;                      // Онлайн
    uint32_t lastSeen;               // Время последнего ответа
    uint8_t rssi;                    // Качество связи (если есть)
    std::vector<SlaveSensorInfo> sensors; // Датчики Помощника
    uint8_t errorCount;               // Счетчик ошибок
};

/**
 * @brief Команда Помощнику
 */
struct SlaveCommand {
    uint8_t slaveId;
    uint8_t command;                  // Код команды
    uint8_t sensorId;                 // ID датчика (0 = всем)
    uint8_t data[32];                // Данные команды
    uint8_t dataLen;
    bool waitResponse;               // Ожидать ответ
    uint32_t timeoutMs;              // Таймаут ответа
};

// ============================================================================
// 3. ОСНОВНОЙ КЛАСС
// ============================================================================

class RS485MasterManager : public IModule {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(uint8_t slaveId)> OnSlaveFoundCallback;
    typedef std::function<void(uint8_t slaveId)> OnSlaveLostCallback;
    typedef std::function<void(uint8_t slaveId, const SlaveSensorInfo& sensor)> OnSensorDataCallback;
    typedef std::function<void(uint8_t slaveId, const char* error)> OnSlaveErrorCallback;

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    RS485MasterManager();
    ~RS485MasterManager();

    // === IModule ===
    const char* getName() const override { return "RS485MasterManager"; }
    const char* getVersion() const override { return "4.2.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_RS485_MASTER; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized; }

    // === ИНИЦИАЛИЗАЦИЯ ===
    void begin(HardwareSerial& serial, uint8_t dePin, uint8_t rePin, uint8_t slaveCount = 3);
    void end();

    // === УПРАВЛЕНИЕ ПОМОЩНИКАМИ ===
    bool sendCommand(const SlaveCommand& cmd);
    bool sendCommand(uint8_t slaveId, uint8_t command, uint8_t sensorId = 0,
                     const uint8_t* data = nullptr, uint8_t dataLen = 0);
    bool sendCommandAsync(uint8_t slaveId, uint8_t command, uint8_t sensorId = 0,
                          const uint8_t* data = nullptr, uint8_t dataLen = 0);
    void scanSlaves();
    void pollSlave(uint8_t slaveId);
    void pollAllSlaves();

    // === ГЕТТЕРЫ ===
    const SlaveInfo* getSlave(uint8_t slaveId) const;
    const std::vector<SlaveInfo>& getSlaves() const { return _slaves; }
    bool isSlaveOnline(uint8_t slaveId) const;
    float getSensorValue(uint8_t slaveId, uint8_t sensorId) const;
    const SlaveSensorInfo* getSensor(uint8_t slaveId, uint8_t sensorId) const;
    uint8_t getSlaveCount() const { return _slaves.size(); }
    size_t getTotalSensors() const;

    // === НАСТРОЙКИ ===
    void setPollInterval(uint32_t ms) { _pollIntervalMs = ms; }
    void setResponseTimeout(uint32_t ms) { _responseTimeoutMs = ms; }
    void setRetryCount(uint8_t count) { _retryCount = count; }
    void setSlaveTimeout(uint32_t ms) { _slaveTimeoutMs = ms; }

    // === КОЛБЭКИ ===
    void setOnSlaveFound(OnSlaveFoundCallback cb) { _onSlaveFound = cb; }
    void setOnSlaveLost(OnSlaveLostCallback cb) { _onSlaveLost = cb; }
    void setOnSensorData(OnSensorDataCallback cb) { _onSensorData = cb; }
    void setOnSlaveError(OnSlaveErrorCallback cb) { _onSlaveError = cb; }

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const override;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    void processIncoming();
    void sendPacket(uint8_t slaveId, uint8_t command, uint8_t sensorId,
                    const uint8_t* data, uint8_t dataLen);
    bool receivePacket(uint8_t& slaveId, uint8_t& command, uint8_t& sensorId,
                       uint8_t* data, uint8_t& dataLen, uint32_t timeout);
    uint8_t calculateCRC(const uint8_t* data, size_t len);

    void handleSlaveData(uint8_t slaveId, uint8_t sensorId, const uint8_t* data, uint8_t dataLen);
    void handleSlaveStatus(uint8_t slaveId, const uint8_t* data, uint8_t dataLen);
    void handleSlaveError(uint8_t slaveId, const uint8_t* data, uint8_t dataLen);

    void updateSlaveStatus(uint8_t slaveId);
    void removeSlave(uint8_t slaveId);
    void updateStats();

    // Отправка событий
    void publishSlaveFound(uint8_t slaveId);
    void publishSlaveLost(uint8_t slaveId);
    void publishSensorData(uint8_t slaveId, const SlaveSensorInfo& sensor);
    void publishSlaveError(uint8_t slaveId, const char* error);
    void publishCommandSent(uint8_t slaveId, uint8_t command);
    void publishScanComplete();
    void publishBusError(const char* error);

    void logMessage(const String& msg);

    // === ДАННЫЕ ===
    HardwareSerial* _serial = nullptr;
    uint8_t _dePin = 0;
    uint8_t _rePin = 0;
    uint8_t _maxSlaves = 3;

    std::vector<SlaveInfo> _slaves;
    SemaphoreHandle_t _slavesMutex = nullptr;

    // Состояние
    bool _initialized = false;
    bool _scanning = false;
    bool _pollingInProgress = false;

    // Настройки
    uint32_t _pollIntervalMs = 10000;      // 10 секунд
    uint32_t _responseTimeoutMs = 500;     // 500 мс
    uint8_t _retryCount = 3;
    uint32_t _slaveTimeoutMs = 60000;      // 60 секунд

    // Таймеры
    uint32_t _lastPollMs = 0;
    uint32_t _lastScanMs = 0;
    uint32_t _lastSlaveCheckMs = 0;
    uint32_t _moduleId = MODULE_ID_RS485_MASTER;

    // Буфер приема
    uint8_t _rxBuffer[256];
    uint8_t _rxPos = 0;
    uint32_t _rxTimeout = 0;

    // Статистика
    struct Stats {
        uint32_t totalPolls = 0;
        uint32_t successfulPolls = 0;
        uint32_t failedPolls = 0;
        uint32_t totalCommands = 0;
        uint32_t successfulCommands = 0;
        uint32_t timeoutErrors = 0;
        uint32_t crcErrors = 0;
        uint32_t slavesFound = 0;
        uint32_t slavesLost = 0;
    } _stats;

    // Колбэки
    OnSlaveFoundCallback _onSlaveFound = nullptr;
    OnSlaveLostCallback _onSlaveLost = nullptr;
    OnSensorDataCallback _onSensorData = nullptr;
    OnSlaveErrorCallback _onSlaveError = nullptr;

    // Константы протокола
    static constexpr uint8_t PROTOCOL_SOF = 0x7E;
    static constexpr uint8_t PROTOCOL_EOF = 0x7F;
    static constexpr uint8_t PROTOCOL_CMD_PING = 0x01;
    static constexpr uint8_t PROTOCOL_CMD_STATUS = 0x02;
    static constexpr uint8_t PROTOCOL_CMD_DATA = 0x03;
    static constexpr uint8_t PROTOCOL_CMD_COMMAND = 0x04;
    static constexpr uint8_t PROTOCOL_CMD_ERROR = 0x05;
    static constexpr uint8_t PROTOCOL_CMD_SCAN = 0x06;
    static constexpr uint8_t PROTOCOL_MAX_DATA = 32;

    static constexpr const char* TAG = "[RS485_MASTER]";
};

// ============================================================================
// 4. ГЛОБАЛЬНЫЙ ЭКЗЕМПЛЯР
// ============================================================================

extern RS485MasterManager& RS485Master;
