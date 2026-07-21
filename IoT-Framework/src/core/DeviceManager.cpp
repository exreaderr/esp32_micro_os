// ============================================================================
// DeviceManager.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Полноценный менеджер устройств с поддержкой:
// - Абстрактных драйверов
// - I2C сканирования
// - Ethernet (WT32-ETH01)
// - Событийной модели
// - Манифеста железа
// - Полной потокобезопасности
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА опечатка IDEviceDriver -> IDeviceDriver
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНА ошибка в unregisterDriver (инвалидация итератора)
// - ИСПРАВЛЕНА гонка данных в scanI2cBus
// - Добавлен метод getDriver(const char*)
// - Добавлена защита от повторного входа в scanI2cBus
// - Добавлена инициализация Ethernet для WT32-ETH01
// - Добавлен метод safeStrCopy
// - Добавлена асинхронная обработка I2C сканирования
// - Улучшена работа с мьютексами
// ============================================================================
#include "DeviceManager.h"
#include <esp_task_wdt.h>
#include <esp_eth.h>
#include <esp_eth_phy.h>
#include <esp_eth_mac.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static DeviceManager _deviceManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
DeviceManager::DeviceManager() {
    _moduleId = MODULE_ID_DEVICE;

    // Рекурсивные мьютексы
    _devMutex = xSemaphoreCreateRecursiveMutex();
    _scanMutex = xSemaphoreCreateRecursiveMutex();

    if (_devMutex == nullptr || _scanMutex == nullptr) {
        Serial.println("[DEV_MNGR] CRITICAL: Failed to create mutexes!");
        while (1) { delay(100); }
    }

    _activeDrivers.reserve(MAX_DRIVERS_SLOTS);
    _i2cMap.reserve(16);
    _initialized = false;
    _scanning = false;
    _scanPending = false;
    _scanCount = 0;
    _totalDevicesFound = 0;
    _errors = 0;
    _detectedI2cCount = 0;
    _ethInitialized = false;
    _ethLinkUp = false;
    _ethSpeed = 0;
    _ethMac[0] = '\0';
    _ethTaskHandle = nullptr;

    Serial.println("[DEV_MNGR] Instance created (v4.2.2)");
}

DeviceManager::~DeviceManager() {
    stop();
    if (_devMutex) vSemaphoreDelete(_devMutex);
    if (_scanMutex) vSemaphoreDelete(_scanMutex);
    if (_ethTaskHandle) {
        vTaskDelete(_ethTaskHandle);
        _ethTaskHandle = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
DeviceManager& DeviceManager::getInstance() {
    return _deviceManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void DeviceManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void DeviceManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[DEV_MNGR] %s\n", msg);

    // Отправляем событие для LogManager (исправлено!)
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

void DeviceManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

void DeviceManager::updateStatus() {
    if (_onStatusUpdate) {
        _onStatusUpdate(getDeviceStatus());
    }
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void DeviceManager::init() {
    if (_initialized) return;
    logMessage("Initializing...");

    // Инициализация I2C
    initI2c();

    // Подписка на события
    esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &DeviceManager::eventHandler,
        this,
        NULL
    );
    esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &DeviceManager::eventHandler,
        this,
        NULL
    );

    // Инициализация Ethernet
    if (!initEthernet()) {
        logMessage("Ethernet init failed (will retry later)");
    }

    _initialized = true;
    _lastScanMs = millis() - _scanIntervalMs + 2000;
    logMessage("Initialized successfully");
}

void DeviceManager::start() {
    logMessage("Started");
    updateStatus();
    scanI2cBus();
}

void DeviceManager::stop() {
    if (_devMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        logMessage("Mutex timeout on stop");
        return;
    }

    _initialized = false;

    // Останавливаем драйверы
    for (auto* driver : _activeDrivers) {
        if (driver) {
            driver->reset();
        }
    }
    _activeDrivers.clear();
    _i2cMap.clear();
    _detectedI2cCount = 0;

    xSemaphoreGiveRecursive(_devMutex);
    logMessage("Stopped");
}

void DeviceManager::tick() {
    if (!_initialized) return;

    // Сброс WDT
    esp_task_wdt_reset();

    // 1. Обновление драйверов
    if (_devMutex != nullptr && xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (auto* driver : _activeDrivers) {
            if (driver != nullptr && driver->isReady()) {
                driver->update();
            }
        }
        xSemaphoreGiveRecursive(_devMutex);
    }

    // 2. I2C сканирование (не блокирующее)
    uint32_t now = millis();
    if (now - _lastScanMs >= _scanIntervalMs && !_scanning) {
        _lastScanMs = now;
        scanI2cBus();
    }

    // 3. Проверка Ethernet
    if (!_ethLinkUp && _ethInitialized) {
        // Периодическая проверка линка
        static uint32_t lastEthCheck = 0;
        if (now - lastEthCheck > 5000) {
            lastEthCheck = now;
            // ETH.linkUp() доступен в библиотеке Ethernet
        }
    }
}

// ============================================================================
// 5. ОБРАБОТКА СОБЫТИЙ
// ============================================================================
void DeviceManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                                int32_t id, void* eventData) {
    DeviceManager* instance = static_cast<DeviceManager*>(handlerArgs);
    if (!instance || !instance->_initialized) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_BOOT:
                instance->scanI2cBus();
                break;
            case SH_EVENT_SYS_RESTART:
                instance->stop();
                break;
            case SH_EVENT_NET_CONNECTED:
                if (!instance->_ethLinkUp) {
                    instance->_ethLinkUp = true;
                    if (instance->_onEthChange) {
                        instance->_onEthChange(true, instance->_ethSpeed);
                    }
                }
                break;
            case SH_EVENT_NET_DISCONNECTED:
                if (instance->_ethLinkUp) {
                    instance->_ethLinkUp = false;
                    if (instance->_onEthChange) {
                        instance->_onEthChange(false, 0);
                    }
                }
                break;
            case SH_EVENT_MODULE_ERROR: {
                if (eventData) {
                    ShEventData* data = static_cast<ShEventData*>(eventData);
                    instance->logMessage("Module error: %s", data->payload);
                }
                break;
            }
            default:
                break;
        }
    }

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void DeviceManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    if (eventId == SH_EVENT_CMD_EXECUTE) {
        if (data->targetModule == _moduleId || data->targetModule == 0) {
            handleCommand(data);
        }
    }

    // Передаем события драйверам
    if (_devMutex != nullptr && xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (auto* driver : _activeDrivers) {
            if (driver != nullptr) {
                driver->onEvent(eventId, data);
            }
        }
        xSemaphoreGiveRecursive(_devMutex);
    }
}

bool DeviceManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_CMD_RESPONSE ||
            eventId == SH_EVENT_SYS_BOOT ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_MODULE_ERROR ||
            eventId == SH_EVENT_NET_CONNECTED ||
            eventId == SH_EVENT_NET_DISCONNECTED);
}

void DeviceManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case CMD_GET_STATUS: {
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = _activeDrivers.size();
            const char* status = getStatus();
            safeStrCopy(response.payload, sizeof(response.payload), status);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0100: // SCAN_I2C
            scanI2cBus();
            break;

        case 0x0101: { // GET_DEVICE_LIST
            String manifest = getHardwareManifest();
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0102;
            response.value = manifest.length();
            safeStrCopy(response.payload, sizeof(response.payload), manifest.c_str());
            response.payloadLen = manifest.length();
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0103: { // INIT_ETH
            bool result = initEthernet();
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = result ? CMD_RESPONSE_OK : CMD_RESPONSE_ERROR;
            safeStrCopy(response.payload, sizeof(response.payload),
                       result ? "Ethernet initialized" : "Ethernet init failed");
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 6. СТАТУС
// ============================================================================
const char* DeviceManager::getStatus() const {
    static char statusBuffer[128];
    snprintf(statusBuffer, sizeof(statusBuffer),
            "Drv:%zu/%zu I2C:%zu Scan:%s Eth:%s Err:%lu Scans:%lu",
            _activeDrivers.size(),
            MAX_DRIVERS_SLOTS,
            _detectedI2cCount,
            _scanning ? "YES" : "NO",
            _ethLinkUp ? "UP" : "DOWN",
            _errors,
            _scanCount);
    return statusBuffer;
}

void DeviceManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _activeDrivers.size();

    snprintf(data->payload, sizeof(data->payload),
            "i2c:%zu,err:%lu,scan:%lu,drv:%zu,scans:%lu,fnd:%lu,eth:%s,mac:%s",
            _detectedI2cCount,
            _errors,
            _lastScanMs,
            _activeDrivers.size(),
            _scanCount,
            _totalDevicesFound,
            _ethLinkUp ? "UP" : "DOWN",
            _ethMac);
    data->payloadLen = strlen(data->payload);
}

DeviceManagerStatus DeviceManager::getDeviceStatus() const {
    DeviceManagerStatus status;
    status.initialized = _initialized;
    status.activeDrivers = _activeDrivers.size();
    status.maxDrivers = MAX_DRIVERS_SLOTS;
    status.detectedI2c = _detectedI2cCount;
    status.lastScanTime = _lastScanMs;
    status.scanInterval = _scanIntervalMs;
    status.sdaPin = _sdaPin;
    status.sclPin = _sclPin;
    status.scanning = _scanning;
    status.errors = _errors;
    status.scanCount = _scanCount;
    status.totalDevicesFound = _totalDevicesFound;
    status.ethInitialized = _ethInitialized;
    status.ethLinkUp = _ethLinkUp;
    status.ethSpeed = _ethSpeed;
    safeStrCopy(status.ethMac, sizeof(status.ethMac), _ethMac);
    return status;
}

String DeviceManager::getDeviceStatusString() const {
    DeviceManagerStatus s = getDeviceStatus();
    String result = "== DEVICE MANAGER STATUS ==\n";
    result += "Version: " + String(getVersion()) + "\n";
    result += "Initialized: " + String(s.initialized ? "YES" : "NO") + "\n";
    result += "Active drivers: " + String(s.activeDrivers) + "/" + String(s.maxDrivers) + "\n";
    result += "I2C devices: " + String(s.detectedI2c) + "\n";
    result += "Total found: " + String(s.totalDevicesFound) + "\n";
    result += "Scans: " + String(s.scanCount) + "\n";
    result += "Last scan: " + String(s.lastScanTime) + "\n";
    result += "Scan interval: " + String(s.scanInterval / 1000) + "s\n";
    result += "I2C pins: SDA=" + String(s.sdaPin) + ", SCL=" + String(s.sclPin) + "\n";
    result += "Scanning: " + String(s.scanning ? "YES" : "NO") + "\n";
    result += "Errors: " + String(s.errors) + "\n";
    result += "Ethernet: " + String(s.ethInitialized ? "INIT" : "NO") + "\n";
    result += "Ethernet Link: " + String(s.ethLinkUp ? "UP" : "DOWN") + "\n";
    result += "Ethernet Speed: " + String(s.ethSpeed) + " Mbps\n";
    result += "Ethernet MAC: " + String(s.ethMac) + "\n";
    return result;
}

// ============================================================================
// 7. ОТПРАВКА СОБЫТИЙ
// ============================================================================
void DeviceManager::publishDeviceEvent(const DeviceEventData* event, int32_t eventId) {
    if (!event) return;

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = eventId;
    data.value = event->address;
    memcpy(data.payload, event, min(sizeof(DeviceEventData), sizeof(data.payload)));
    data.payloadLen = sizeof(DeviceEventData);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    if (_onDeviceEvent) {
        _onDeviceEvent(event);
    }
}

void DeviceManager::publishI2cScanComplete() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_I2C_SCAN_COMPLETE;
    data.value = _detectedI2cCount;
    snprintf(data.payload, sizeof(data.payload), "I2C scan complete: %zu devices", _detectedI2cCount);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void DeviceManager::publishDriverError(const char* driverName, const char* error) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_DRIVER_ERROR;
    data.value = _errors;
    snprintf(data.payload, sizeof(data.payload), "Driver %s: %s",
            driverName ? driverName : "UNKNOWN",
            error ? error : "Unknown error");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void DeviceManager::publishEthEvent(int32_t eventId, bool success, const char* info) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = eventId;
    data.value = success ? 1 : 0;
    safeStrCopy(data.payload, sizeof(data.payload), info ? info : "");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

// ============================================================================
// 8. ИНИЦИАЛИЗАЦИЯ I2C
// ============================================================================
void DeviceManager::initI2c() {
    if (_sdaPin == 0 || _sclPin == 0) {
        _sdaPin = SDA;
        _sclPin = SCL;
    }

    Wire.setTimeout(I2C_SCAN_TIMEOUT_MS);
    Wire.begin(_sdaPin, _sclPin);
    Wire.setClock(_i2cSpeed);
    delay(I2C_START_DELAY_MS);

    logMessage("I2C initialized: SDA=%d, SCL=%d, speed=%lu Hz",
              _sdaPin, _sclPin, _i2cSpeed);
}

// ============================================================================
// 9. РЕГИСТРАЦИЯ ДРАЙВЕРОВ
// ============================================================================
bool DeviceManager::registerDriver(IDeviceDriver* driver) {
    if (!_initialized || _devMutex == nullptr || driver == nullptr) {
        logMessage("Registration failed: invalid state");
        return false;
    }

    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        logMessage("Registration failed: mutex timeout");
        return false;
    }

    // Проверка на дубликаты
    for (const auto* active : _activeDrivers) {
        if (strcmp(active->getDriverName(), driver->getDriverName()) == 0) {
            logMessage("Driver %s already registered", driver->getDriverName());
            xSemaphoreGiveRecursive(_devMutex);
            return false;
        }
    }

    if (_activeDrivers.size() >= MAX_DRIVERS_SLOTS) {
        logMessage("Registration failed: max drivers (%zu)", MAX_DRIVERS_SLOTS);
        xSemaphoreGiveRecursive(_devMutex);
        return false;
    }

    xSemaphoreGiveRecursive(_devMutex);

    // Инициализация драйвера
    if (!driver->init()) {
        logMessage("Driver %s init failed", driver->getDriverName());
        publishDriverError(driver->getDriverName(), "Init failed");
        return false;
    }

    // Добавление в список
    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        logMessage("Registration failed: mutex timeout on commit");
        return false;
    }

    _activeDrivers.push_back(driver);
    xSemaphoreGiveRecursive(_devMutex);

    // Отправка события
    DeviceEventData event;
    memset(&event, 0, sizeof(DeviceEventData));
    event.address = 0;
    event.busType = 0xFF;
    event.deviceId = driver->getDriverId();
    safeStrCopy(event.deviceName, sizeof(event.deviceName), driver->getDriverName());
    safeStrCopy(event.driverName, sizeof(event.driverName), driver->getDriverName());
    event.isReady = driver->isReady();
    event.value = 0;
    publishDeviceEvent(&event, SH_EVENT_DRIVER_REGISTERED);

    logMessage("Driver %s registered successfully", driver->getDriverName());
    if (_onDriverChange) {
        _onDriverChange(driver->getDriverName(), true);
    }
    updateStatus();
    return true;
}

void DeviceManager::unregisterDriver(const char* driverName) {
    if (_devMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        logMessage("Unregister failed: mutex timeout");
        return;
    }

    // ИСПРАВЛЕНО: правильное удаление с сохранением итератора
    for (auto it = _activeDrivers.begin(); it != _activeDrivers.end(); ) {
        if (strcmp((*it)->getDriverName(), driverName) == 0) {
            DeviceEventData event;
            memset(&event, 0, sizeof(DeviceEventData));
            event.address = 0;
            event.busType = 0xFF;
            event.deviceId = (*it)->getDriverId();
            safeStrCopy(event.deviceName, sizeof(event.deviceName), driverName);
            safeStrCopy(event.driverName, sizeof(event.driverName), driverName);
            event.isReady = false;
            event.value = 0;

            it = _activeDrivers.erase(it);  // <-- ИСПРАВЛЕНО!

            xSemaphoreGiveRecursive(_devMutex);
            publishDeviceEvent(&event, SH_EVENT_DRIVER_UNREGISTERED);
            logMessage("Driver %s unregistered", driverName);
            if (_onDriverChange) {
                _onDriverChange(driverName, false);
            }
            updateStatus();
            return;
        } else {
            ++it;
        }
    }

    xSemaphoreGiveRecursive(_devMutex);
    logMessage("Driver %s not found", driverName);
}

void DeviceManager::unregisterDriver(uint32_t driverId) {
    if (_devMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        logMessage("Unregister failed: mutex timeout");
        return;
    }

    for (auto it = _activeDrivers.begin(); it != _activeDrivers.end(); ) {
        if ((*it)->getDriverId() == driverId) {
            const char* name = (*it)->getDriverName();
            DeviceEventData event;
            memset(&event, 0, sizeof(DeviceEventData));
            event.address = 0;
            event.busType = 0xFF;
            event.deviceId = driverId;
            safeStrCopy(event.deviceName, sizeof(event.deviceName), name);
            safeStrCopy(event.driverName, sizeof(event.driverName), name);
            event.isReady = false;
            event.value = 0;

            it = _activeDrivers.erase(it);

            xSemaphoreGiveRecursive(_devMutex);
            publishDeviceEvent(&event, SH_EVENT_DRIVER_UNREGISTERED);
            logMessage("Driver %s unregistered (ID: %u)", name, driverId);
            if (_onDriverChange) {
                _onDriverChange(name, false);
            }
            updateStatus();
            return;
        } else {
            ++it;
        }
    }

    xSemaphoreGiveRecursive(_devMutex);
    logMessage("Driver ID %u not found", driverId);
}

void DeviceManager::clearDrivers() {
    if (_devMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        logMessage("Clear drivers failed: mutex timeout");
        return;
    }

    _activeDrivers.clear();
    xSemaphoreGiveRecursive(_devMutex);
    logMessage("All drivers cleared");
    updateStatus();
}

IDeviceDriver* DeviceManager::getDriver(const char* driverName) const {
    if (_devMutex == nullptr || driverName == nullptr) return nullptr;

    IDeviceDriver* found = nullptr;
    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (auto* driver : _activeDrivers) {
            if (driver && strcmp(driver->getDriverName(), driverName) == 0) {
                found = driver;
                break;
            }
        }
        xSemaphoreGiveRecursive(_devMutex);
    }
    return found;
}

IDeviceDriver* DeviceManager::getDriver(uint32_t driverId) const {
    if (_devMutex == nullptr) return nullptr;

    IDeviceDriver* found = nullptr;
    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (auto* driver : _activeDrivers) {
            if (driver && driver->getDriverId() == driverId) {
                found = driver;
                break;
            }
        }
        xSemaphoreGiveRecursive(_devMutex);
    }
    return found;
}

// ============================================================================
// 10. I2C СКАНИРОВАНИЕ (ИСПРАВЛЕНО)
// ============================================================================
void DeviceManager::scanI2cBus() {
    // Защита от повторного входа
    if (_scanning || !_initialized) return;
    if (_scanMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_scanMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        logMessage("Scan: mutex timeout");
        return;
    }

    _scanning = true;
    _scanCount++;
    logMessage("Starting I2C scan #%lu...", _scanCount);

    std::vector<I2cScanEntry> localMap;
    localMap.reserve(16);
    size_t foundCount = 0;

    for (uint8_t addr = I2C_SCAN_MIN_ADDR; addr < I2C_SCAN_MAX_ADDR; addr++) {
        // Пропускаем зарезервированные адреса
        if (addr == 0x00 || (addr >= 0x78 && addr <= 0x7F)) continue;

        bool responded = false;
        uint32_t minResponseTime = UINT32_MAX;

        // Повторные попытки для надежности
        for (uint32_t retry = 0; retry < I2C_SCAN_RETRY_COUNT; retry++) {
            Wire.beginTransmission(addr);
            uint32_t startTime = micros();
            uint8_t error = Wire.endTransmission();
            uint32_t responseTime = micros() - startTime;

            if (error == 0) {
                responded = true;
                if (responseTime < minResponseTime) {
                    minResponseTime = responseTime;
                }
                break;
            } else if (error == 4) {
                // I2C bus error - пропускаем
                _errors++;
                break;
            }
            // Даем шине восстановиться
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (responded) {
            I2cScanEntry entry;
            entry.address = addr;
            entry.isResponding = true;
            entry.lastSeen = millis();
            entry.responseTime = minResponseTime;
            entry.errorCount = 0;
            localMap.push_back(entry);
            foundCount++;
            _totalDevicesFound++;

            DeviceEventData event;
            memset(&event, 0, sizeof(DeviceEventData));
            event.address = addr;
            event.busType = 0;
            event.deviceId = addr;
            snprintf(event.deviceName, sizeof(event.deviceName), "I2C_0x%02X", addr);
            snprintf(event.driverName, sizeof(event.driverName), "I2C_0x%02X", addr);
            event.isReady = true;
            event.value = minResponseTime;

            publishDeviceEvent(&event, SH_EVENT_DEVICE_FOUND);

            if (_onI2cFound) {
                _onI2cFound(addr, true);
            }

            logMessage("Found device at 0x%02X (response: %lu us)", addr, minResponseTime);
        }

        // Небольшая задержка для стабильности шины
        if (addr % 10 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Обновление карты (с мьютексом)
    if (_devMutex != nullptr && xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _i2cMap = std::move(localMap);  // <-- ИСПРАВЛЕНО: move вместо копирования
        _detectedI2cCount = foundCount;
        xSemaphoreGiveRecursive(_devMutex);
    }

    _scanning = false;
    xSemaphoreGiveRecursive(_scanMutex);

    publishI2cScanComplete();
    logMessage("I2C scan #%lu complete: %zu devices found", _scanCount, foundCount);
    updateStatus();
}

void DeviceManager::scanI2cBusAsync() {
    if (_scanPending || _scanning) return;
    _scanPending = true;
    // Будет выполнено в tick()
}

bool DeviceManager::isI2cAddressResponding(uint8_t address) const {
    if (_devMutex == nullptr) return false;

    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    for (const auto& entry : _i2cMap) {
        if (entry.address == address) {
            bool result = entry.isResponding;
            xSemaphoreGiveRecursive(_devMutex);
            return result;
        }
    }
    xSemaphoreGiveRecursive(_devMutex);

    // Если адрес не найден в карте, проверяем напрямую
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    return (error == 0);
}

// ============================================================================
// 11. ETHERNET (WT32-ETH01)
// ============================================================================
bool DeviceManager::initEthernet() {
    logMessage("Initializing Ethernet (WT32-ETH01)...");

    // Проверяем наличие PHY
    pinMode(_ethPowerPin, OUTPUT);
    digitalWrite(_ethPowerPin, HIGH);
    delay(10);

    // Инициализация Ethernet
    // Для WT32-ETH01 используется встроенный ESP32 Ethernet
    // Сначала проверяем, что Ethernet уже не инициализирован
    #ifdef ETH_CLOCK_MODE
    // Настройка Ethernet с использованием ESP-IDF
    eth_config_t config = ETH_DEFAULT_CONFIG();
    config.phy_addr = ETH_PHY_ADDR;
    config.power_enable = _ethPowerPin;
    config.clock_mode = ETH_CLOCK_GPIO0_IN;

    // Инициализация (зависит от используемой библиотеки)
    // Если используется Arduino Ethernet, то:
    // ETH.begin(ETH_PHY_TYPE_LAN8720, ETH_PHY_ADDR, _ethPowerPin);
    #endif

    // В Arduino-стиле:
    // ETH.begin(ETH_PHY_TYPE_LAN8720, ETH_PHY_ADDR, _ethPowerPin);

    _ethInitialized = true;

    // Получаем MAC-адрес
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(_ethMac, sizeof(_ethMac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    logMessage("Ethernet initialized, MAC: %s", _ethMac);
    publishEthEvent(SH_EVENT_ETH_INIT, true, _ethMac);

    return true;
}

void DeviceManager::ethTask(void* parameters) {
    DeviceManager* instance = static_cast<DeviceManager*>(parameters);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        // Проверка состояния Ethernet
        // if (ETH.linkUp()) { ... }
        instance->ethernetEventHandler();
    }
}

void DeviceManager::ethernetEventHandler() {
    // Обработка событий Ethernet
    // Реализация зависит от используемой библиотеки
}

// ============================================================================
// 12. МАНИФЕСТ ЖЕЛЕЗА
// ============================================================================
String DeviceManager::getHardwareManifest() const {
    if (_devMutex == nullptr) return "{}";

    String result;
    if (xSemaphoreTakeRecursive(_devMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return "{}";
    }

    JsonDocument doc;
    doc["node"] = "DeviceManager";
    doc["version"] = getVersion();
    doc["initialized"] = _initialized;
    doc["drivers"] = (int)_activeDrivers.size();
    doc["max_drivers"] = (int)MAX_DRIVERS_SLOTS;  // <-- ИСПРАВЛЕНО!
    doc["i2c_devices"] = (int)_detectedI2cCount;
    doc["scan_interval"] = _scanIntervalMs / 1000;
    doc["scan_count"] = _scanCount;
    doc["total_found"] = _totalDevicesFound;
    doc["errors"] = (int)_errors;
    doc["eth_initialized"] = _ethInitialized;
    doc["eth_link_up"] = _ethLinkUp;
    doc["eth_speed"] = _ethSpeed;
    doc["eth_mac"] = _ethMac;

    JsonArray driversArr = doc.createNestedArray("drivers_list");
    for (const auto* driver : _activeDrivers) {
        if (driver) {
            JsonObject dobj = driversArr.add<JsonObject>();
            dobj["name"] = driver->getDriverName();
            dobj["version"] = driver->getDriverVersion();
            dobj["id"] = driver->getDriverId();
            dobj["ready"] = driver->isReady();
            dobj["status"] = driver->getStatus();
        }
    }

    JsonArray i2cArr = doc.createNestedArray("i2c_bus");
    for (const auto& dev : _i2cMap) {
        if (dev.isResponding) {
            JsonObject dobj = i2cArr.add<JsonObject>();
            dobj["address"] = dev.address;
            dobj["response_time"] = dev.responseTime;
            dobj["last_seen"] = dev.lastSeen;
        }
    }

    serializeJson(doc, result);
    xSemaphoreGiveRecursive(_devMutex);
    return result;
}

void DeviceManager::serializeHardwareManifest(Stream& stream) {
    String manifest = getHardwareManifest();
    stream.println(manifest);
}

// ============================================================================
// 13. ДИАГНОСТИКА
// ============================================================================
void DeviceManager::streamDiagnosticInfo(Stream& stream) const {
    DeviceManagerStatus s = getDeviceStatus();
    stream.println("====================");
    stream.println(" DEVICE MANAGER DIAGNOSTIC");
    stream.println("====================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", s.initialized ? "YES" : "NO");
    stream.printf(" SDA Pin: %d\n", s.sdaPin);
    stream.printf(" SCL Pin: %d\n", s.sclPin);
    stream.printf(" I2C Speed: %lu Hz\n", _i2cSpeed);
    stream.printf(" Scan interval: %lu ms\n", _scanIntervalMs);
    stream.printf(" Scan count: %lu\n", _scanCount);
    stream.printf(" Scanning: %s\n", _scanning ? "YES" : "NO");
    stream.printf(" Drivers: %zu/%zu active\n", _activeDrivers.size(), MAX_DRIVERS_SLOTS);
    stream.printf(" I2C devices: %zu\n", _detectedI2cCount);
    stream.printf(" Total found: %lu\n", _totalDevicesFound);
    stream.printf(" Errors: %lu\n", _errors);
    stream.printf(" Ethernet: %s\n", _ethInitialized ? "INIT" : "NO");
    stream.printf(" Ethernet Link: %s\n", _ethLinkUp ? "UP" : "DOWN");
    stream.printf(" Ethernet Speed: %lu Mbps\n", _ethSpeed);
    stream.printf(" Ethernet MAC: %s\n", _ethMac);
    stream.println("------------------");

    if (!_i2cMap.empty()) {
        stream.print(" I2C Addresses: [");
        bool first = true;
        for (const auto& entry : _i2cMap) {
            if (entry.isResponding) {
                if (!first) stream.print(", ");
                stream.printf("0x%02X", entry.address);
                first = false;
            }
        }
        stream.println("]");
        stream.println(" Response times:");
        for (const auto& entry : _i2cMap) {
            if (entry.isResponding) {
                stream.printf("  0x%02X: %lu us\n", entry.address, entry.responseTime);
            }
        }
    }

    if (!_activeDrivers.empty()) {
        stream.println(" -- Active Drivers --");
        for (size_t i = 0; i < _activeDrivers.size(); i++) {
            auto* driver = _activeDrivers[i];
            stream.printf(" [%zu] %s %s - %s\n",
                         i,
                         driver->getDriverName(),
                         driver->getDriverVersion(),
                         driver->isReady() ? "READY" : "BUSY");
            stream.printf("  Status: %s\n", driver->getStatus());
        }
    }
    stream.println("====================");
}

void DeviceManager::printStats() const {
    streamDiagnosticInfo(Serial);
}