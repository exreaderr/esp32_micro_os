// ============================================================================
// DeviceManager.h - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление аппаратными устройствами и драйверами.
// Все обнаружения устройств и изменения публикуются в шину событий.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА опечатка IDEviceDriver -> IDeviceDriver
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНА ошибка в unregisterDriver (инвалидация итератора)
// - Исправлена гонка данных в scanI2cBus
// - Добавлен метод getDriver(const char*)
// - Добавлена защита от повторного входа в scanI2cBus
// - Улучшена работа с I2C (повторные попытки, таймауты)
// - Добавлена поддержка Ethernet PHY для WT32-ETH01
// - Добавлена полная потокобезопасность
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_event.h>

// Только IModule, без AppCore!
#include "core/IModule.h"

// ============================================================================
// 1. КОНСТАНТЫ ДЛЯ WT32-ETH01
// ============================================================================
#define ETH_PHY_POWER_PIN 16
#define ETH_PHY_ADDR 0
#define ETH_PHY_TYPE ETH_PHY_LAN8720

// ============================================================================
// 2. СОБЫТИЯ DEVICE MANAGER
// ============================================================================
enum DeviceEvents : int32_t {
    SH_EVENT_DEVICE_FOUND = SH_EVENT_USER_BASE + 0x0100,
    SH_EVENT_DEVICE_REMOVED = SH_EVENT_USER_BASE + 0x0101,
    SH_EVENT_DEVICE_CHANGED = SH_EVENT_USER_BASE + 0x0102,
    SH_EVENT_DRIVER_REGISTERED = SH_EVENT_USER_BASE + 0x0103,
    SH_EVENT_DRIVER_UNREGISTERED = SH_EVENT_USER_BASE + 0x0104,
    SH_EVENT_DRIVER_ERROR = SH_EVENT_USER_BASE + 0x0105,
    SH_EVENT_I2C_SCAN_COMPLETE = SH_EVENT_USER_BASE + 0x0106,
    SH_EVENT_DEVICE_ERROR = SH_EVENT_USER_BASE + 0x0107,
    SH_EVENT_ETH_INIT = SH_EVENT_USER_BASE + 0x0108,
    SH_EVENT_ETH_ERROR = SH_EVENT_USER_BASE + 0x0109,
    SH_EVENT_ETH_LINK_CHANGED = SH_EVENT_USER_BASE + 0x010A
};

// ============================================================================
// 3. СТРУКТУРЫ ДАННЫХ
// ============================================================================
/**
 * @brief Структура события устройства
 */
struct DeviceEventData {
    uint8_t address;        // I2C адрес
    uint8_t busType;        // 0 = I2C, 1 = SPI, 2 = 1-Wire
    uint32_t deviceId;      // ID устройства
    char deviceName[32];    // Имя устройства
    char driverName[32];    // Имя драйвера
    bool isReady;           // Готовность
    int32_t value;          // Дополнительное значение
};

/**
 * @brief Структура результата I2C сканирования
 */
struct I2cScanEntry {
    uint8_t address;
    bool isResponding;
    uint32_t lastSeen;
    uint32_t responseTime;  // микросекунды
    uint32_t errorCount;    // Количество ошибок
};

/**
 * @brief Статус DeviceManager
 */
struct DeviceManagerStatus {
    bool initialized = false;
    size_t activeDrivers = 0;
    size_t maxDrivers = 16;
    size_t detectedI2c = 0;
    uint32_t lastScanTime = 0;
    uint32_t scanInterval = 60000;
    uint8_t sdaPin = 8;
    uint8_t sclPin = 9;
    bool scanning = false;
    uint32_t errors = 0;
    uint32_t scanCount = 0;
    uint32_t totalDevicesFound = 0;
    bool ethInitialized = false;
    bool ethLinkUp = false;
    uint32_t ethSpeed = 0;
    char ethMac[18] = "";
};

// ============================================================================
// 4. АБСТРАКТНЫЙ ИНТЕРФЕЙС ДЛЯ ДРАЙВЕРОВ
// ============================================================================
/**
 * @brief Базовый интерфейс для всех драйверов устройств
 *
 * Все драйверы должны наследоваться от этого класса.
 * Обеспечивает унифицированный доступ к аппаратным устройствам.
 */
class IDeviceDriver {
public:
    virtual ~IDeviceDriver() = default;

    // === БАЗОВАЯ ИНФОРМАЦИЯ ===
    virtual const char* getDriverName() const = 0;
    virtual const char* getDriverVersion() const = 0;
    virtual uint32_t getDriverId() const = 0;

    // === ЖИЗНЕННЫЙ ЦИКЛ ===
    virtual bool init() = 0;
    virtual void update() = 0;
    virtual void reset() = 0;

    // === СТАТУС ===
    virtual bool isReady() const = 0;
    virtual const char* getStatus() const = 0;
    virtual void diagnostics(JsonObject& doc) = 0;

    // === ДАННЫЕ ===
    virtual void serializeData(JsonObject& doc) = 0;
    virtual void onEvent(int32_t eventId, const ShEventData* data) = 0;
};

// ============================================================================
// 5. ОСНОВНОЙ КЛАСС
// ============================================================================
/**
 * @brief Менеджер устройств
 *
 * Синглтон. Обеспечивает:
 * - Регистрацию и управление драйверами
 * - I2C сканирование
 * - Инициализацию Ethernet (WT32-ETH01)
 * - Публикацию событий об устройствах
 * - Манифест железа
 */
class DeviceManager : public IModule {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(uint8_t address, bool found)> OnI2cDeviceFoundCallback;
    typedef std::function<void(const char* driverName, bool success)> OnDriverChangeCallback;
    typedef std::function<void(const DeviceEventData* event)> OnDeviceEventCallback;
    typedef std::function<void(const DeviceManagerStatus& status)> OnStatusUpdateCallback;
    typedef std::function<void(bool linkUp, uint32_t speed)> OnEthChangeCallback;

    // === СИНГЛТОН ===
    static DeviceManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    DeviceManager();
    ~DeviceManager();

    // Запрещаем копирование
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "DeviceManager"; }
    const char* getVersion() const override { return "4.2.2"; }
    uint32_t getModuleId() const override { return MODULE_ID_DEVICE; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized; }

    // === ИНИЦИАЛИЗАЦИЯ ===
    void setPins(uint8_t sda, uint8_t scl) { _sdaPin = sda; _sclPin = scl; }
    void setI2cSpeed(uint32_t speed) { _i2cSpeed = speed; }
    bool initEthernet();
    void setEthPowerPin(uint8_t pin) { _ethPowerPin = pin; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === РЕГИСТРАЦИЯ ДРАЙВЕРОВ ===
    bool registerDriver(IDeviceDriver* driver);
    void unregisterDriver(const char* driverName);
    void unregisterDriver(uint32_t driverId);
    void clearDrivers();
    IDeviceDriver* getDriver(const char* driverName) const;
    IDeviceDriver* getDriver(uint32_t driverId) const;

    template<typename T>
    T* getDriver(uint32_t driverId = 0) const;

    // === I2C СКАНИРОВАНИЕ ===
    void scanI2cBus();
    void scanI2cBusAsync();  // Асинхронное сканирование
    bool isI2cAddressResponding(uint8_t address) const;
    const std::vector<I2cScanEntry>& getI2cMap() const { return _i2cMap; }
    void setScanInterval(uint32_t intervalMs) { _scanIntervalMs = intervalMs; }
    void forceScan() { scanI2cBus(); }

    // === МАНИФЕСТ ЖЕЛЕЗА ===
    void serializeHardwareManifest(Stream& stream);
    String getHardwareManifest() const;

    // === ГЕТТЕРЫ ===
    size_t getActiveDriversCount() const { return _activeDrivers.size(); }
    size_t getMaxDrivers() const { return MAX_DRIVERS_SLOTS; }
    size_t getDetectedI2cCount() const { return _detectedI2cCount; }
    bool isInitialized() const { return _initialized; }
    bool isEthReady() const { return _ethInitialized && _ethLinkUp; }
    const char* getEthMac() const { return _ethMac; }

    DeviceManagerStatus getDeviceStatus() const;
    String getDeviceStatusString() const;

    // === КОЛБЭКИ ===
    void setOnI2cDeviceFound(OnI2cDeviceFoundCallback cb) { _onI2cFound = cb; }
    void setOnDriverChange(OnDriverChangeCallback cb) { _onDriverChange = cb; }
    void setOnDeviceEvent(OnDeviceEventCallback cb) { _onDeviceEvent = cb; }
    void setOnStatusUpdate(OnStatusUpdateCallback cb) { _onStatusUpdate = cb; }
    void setOnEthChange(OnEthChangeCallback cb) { _onEthChange = cb; }

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    void initI2c();
    void updateStatus();
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);

    void publishDeviceEvent(const DeviceEventData* event, int32_t eventId);
    void publishI2cScanComplete();
    void publishDriverError(const char* driverName, const char* error);
    void publishEthEvent(int32_t eventId, bool success, const char* info);

    void ethernetEventHandler();
    static void ethTask(void* parameters);

    // === ОБРАБОТЧИКИ СОБЫТИЙ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ДАННЫЕ ===
    std::vector<IDeviceDriver*> _activeDrivers;
    std::vector<I2cScanEntry> _i2cMap;

    uint8_t _sdaPin = 8;
    uint8_t _sclPin = 9;
    uint32_t _i2cSpeed = 100000;
    uint32_t _lastScanMs = 0;
    uint32_t _scanIntervalMs = 60000;
    size_t _detectedI2cCount = 0;
    uint32_t _scanCount = 0;
    uint32_t _totalDevicesFound = 0;
    bool _initialized = false;
    bool _scanning = false;
    bool _scanPending = false;
    uint32_t _errors = 0;
    uint32_t _moduleId = MODULE_ID_DEVICE;

    // Ethernet
    bool _ethInitialized = false;
    bool _ethLinkUp = false;
    uint32_t _ethSpeed = 0;
    uint8_t _ethPowerPin = ETH_PHY_POWER_PIN;
    char _ethMac[18] = "";
    TaskHandle_t _ethTaskHandle = nullptr;

    // Рекурсивные мьютексы
    SemaphoreHandle_t _devMutex = nullptr;
    SemaphoreHandle_t _scanMutex = nullptr;

    // === КОЛБЭКИ ===
    OnI2cDeviceFoundCallback _onI2cFound = nullptr;
    OnDriverChangeCallback _onDriverChange = nullptr;
    OnDeviceEventCallback _onDeviceEvent = nullptr;
    OnStatusUpdateCallback _onStatusUpdate = nullptr;
    OnEthChangeCallback _onEthChange = nullptr;

    // === КОНСТАНТЫ ===
    static constexpr size_t MAX_DRIVERS_SLOTS = 16;
    static constexpr uint32_t I2C_SCAN_TIMEOUT_MS = 50;
    static constexpr uint32_t I2C_START_DELAY_MS = 10;
    static constexpr uint32_t I2C_SCAN_MAX_ADDR = 0x7F;
    static constexpr uint32_t I2C_SCAN_MIN_ADDR = 0x01;
    static constexpr uint32_t I2C_SCAN_RETRY_COUNT = 2;
    static constexpr uint32_t ETH_INIT_TIMEOUT_MS = 10000;
    static constexpr uint32_t ETH_TASK_STACK_SIZE = 4096;
};

// ============================================================================
// 6. ШАБЛОННЫЙ МЕТОД ДЛЯ ПОЛУЧЕНИЯ ДРАЙВЕРА ПО ТИПУ
// ============================================================================
template<typename T>
T* DeviceManager::getDriver(uint32_t driverId) const {
    if (_devMutex == nullptr) return nullptr;

    T* found = nullptr;
    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (auto* driver : _activeDrivers) {
            if (driverId == 0 || driver->getDriverId() == driverId) {
                T* casted = dynamic_cast<T*>(driver);
                if (casted != nullptr) {
                    found = casted;
                    break;
                }
            }
        }
        xSemaphoreGiveRecursive(_devMutex);
    }
    return found;
}
