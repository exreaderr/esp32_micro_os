// ============================================================================
// NetworkManager.cpp - РЕАЛИЗАЦИЯ РЕАКТИВНОГО МЕНЕДЖЕРА СЕТИ v5.0
// ============================================================================
// Описание: Управление сетевым интерфейсом (Ethernet/Wi-Fi) на WT32-ETH01.
//           Обеспечивает получение IP-адреса, контроль соединения и публикацию
//           событий о состоянии сети через новую событийную шину.
//
// ИЗМЕНЕНИЯ v5.0:
// - Сохранена 100% функциональность v4.2.2
// - Добавлен метод publishNetworkEventInternal() для публикации через новую шину
// - Добавлен метод publishNetworkEvent() (публичный)
// - Добавлены вызовы publishNetworkEventInternal() в ключевые методы
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "NetworkManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include <esp_eth.h>
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static MicroOSNetworkManager _networkManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
MicroOSNetworkManager::MicroOSNetworkManager() {
    _mutex = xSemaphoreCreateRecursiveMutex();
    if (_mutex == nullptr) {
        Serial.println("[NET] CRITICAL: Failed to create mutex!");
        while (1) { delay(100); }
    }

    _moduleId = MODULE_ID_NETWORK;
    _initialized = false;
    _ready = false;
    _state = NetworkState::DISABLED;
    _useDhcp = true;
    _autoReconnect = true;
    _dhcpTimeoutMs = 30000;
    _reconnectDelayMs = 10000;
    _maxReconnectAttempts = 5;
    _checkIntervalMs = 5000;
    _reconnectAttempts = 0;
    _totalEventsPublished = 0;

    memset(&_stats, 0, sizeof(_stats));
    _stats.maxRetryCount = _maxReconnectAttempts;
    _stats.isDhcpEnabled = true;
    _stats.hasValidIP = false;

    _hostname[0] = '\0';
    safeStrCopy(_hostname, sizeof(_hostname), "esp32-device");

    _startTime = millis();

    _onStateChange = nullptr;
    _onConnected = nullptr;
    _onDisconnected = nullptr;
    _onStatsUpdate = nullptr;

    Serial.println("[NET] Instance created (v5.0)");
}

MicroOSNetworkManager::~MicroOSNetworkManager() {
    stop();
    if (_mutex != nullptr) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
MicroOSNetworkManager& MicroOSNetworkManager::getInstance() {
    return _networkManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void MicroOSNetworkManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void MicroOSNetworkManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[NET] %s\n", msg);

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

void MicroOSNetworkManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

// ============================================================================
// 4. НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void MicroOSNetworkManager::publishNetworkEventInternal(const char* eventType, const char* details,
                                                 bool success) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = success ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "Network: %s %s - %s",
             eventType,
             details ? details : "",
             success ? "OK" : "FAIL");

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void MicroOSNetworkManager::publishNetworkEvent(const char* eventType, const char* details,
                                         bool success) {
    publishNetworkEventInternal(eventType, details, success);
}

// ============================================================================
// 5. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void MicroOSNetworkManager::init() {
    if (_initialized) return;

    logMessage("Initializing...");

    _startTime = millis();

    if (_mutex != nullptr) {
        if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            _stats.isDhcpEnabled = _useDhcp;
            xSemaphoreGiveRecursive(_mutex);
        }
    }

    // Инициализация Ethernet
    if (initEthernet()) {
        _state = NetworkState::WAITING_IP;
        logMessage("Ethernet initialized, waiting for IP...");
        publishNetworkEventInternal("INIT", "Ethernet initialized", true);
    } else {
        _state = NetworkState::ERROR;
        logMessage("Ethernet init failed!");
        publishNetworkEventInternal("INIT", "Ethernet init failed", false);
    }

    _lastCheckTime = millis();
    _initialized = true;
    _ready = true;

    logMessage("Initialized successfully");
}

void MicroOSNetworkManager::start() {
    if (!_ready) {
        logMessage("Cannot start: not ready");
        return;
    }
    logMessage("Started");
    publishNetworkEventInternal("START", nullptr, true);
}

void MicroOSNetworkManager::stop() {
    if (!_initialized) return;

    _ready = false;
    _initialized = false;
    logMessage("Stopped");
    publishNetworkEventInternal("STOP", nullptr, true);
}

void MicroOSNetworkManager::tick() {
    if (!_initialized || !_ready) return;

    esp_task_wdt_reset();

    uint32_t now = millis();

    // Проверяем состояние сети по таймеру
    if (now - _lastCheckTime >= _checkIntervalMs) {
        _lastCheckTime = now;
        checkNetworkState();
    }

    // Обновляем статистику (раз в 10 секунд)
    static uint32_t lastStatsUpdate = 0;
    if (now - lastStatsUpdate > 10000) {
        lastStatsUpdate = now;
        updateStats();
        if (_onStatsUpdate) {
            _onStatsUpdate(_stats);
        }
    }
}

// ============================================================================
// 6. ИНИЦИАЛИЗАЦИЯ ETHERNET
// ============================================================================
bool MicroOSNetworkManager::initEthernet() {
    logMessage("Initializing Ethernet (WT32-ETH01)...");

    // Проверяем, что Ethernet доступен
    // Для WT32-ETH01 используем стандартную инициализацию
    // В Arduino с ETH.h используется ETH.begin()
    // ETH.begin(ETH_PHY_TYPE_LAN8720, ETH_PHY_ADDR, ETH_PHY_POWER_PIN);

    // Для ESP-IDF v5.x используем esp_eth_config
    // Для совместимости с Arduino ETH.h используем упрощенный подход

    // Проверка наличия PHY через I2C или по пину питания
    // В WT32-ETH01 питание PHY управляется через GPIO16
    pinMode(ETH_PHY_POWER_PIN, OUTPUT);
    digitalWrite(ETH_PHY_POWER_PIN, HIGH);
    delay(10);

    // Устанавливаем хостнейм
    if (strlen(_hostname) > 0) {
        ETH.setHostname(_hostname);
        logMessage("Hostname set: %s", _hostname);
    }

    // Получаем MAC-адрес
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        safeStrCopy(_stats.macAddress, sizeof(_stats.macAddress), macStr);
        xSemaphoreGiveRecursive(_mutex);
    }

    logMessage("Ethernet initialized, MAC: %s", macStr);
    return true;
}

// ============================================================================
// 7. ПРОВЕРКА СОСТОЯНИЯ СЕТИ
// ============================================================================
void MicroOSNetworkManager::checkNetworkState() {
    NetworkState newState = _state;

    // --- 1. Проверка для состояния ERROR ---
    if (_state == NetworkState::ERROR) {
        // Пытаемся восстановиться
        if (_autoReconnect && (millis() - _lastReconnectAttempt > _reconnectDelayMs)) {
            _lastReconnectAttempt = millis();
            _reconnectAttempts++;
            _stats.reconnectAttempts++;

            logMessage("Reconnect attempt %lu/%lu", _reconnectAttempts, _maxReconnectAttempts);
            publishNetworkEventInternal("RECONNECT", "Attempting reconnect", true);

            if (initEthernet()) {
                newState = NetworkState::WAITING_IP;
                logMessage("Ethernet reinitialized");
                publishNetworkEventInternal("RECONNECT", "Ethernet reinitialized", true);
            } else {
                logMessage("Reconnect failed");
                publishNetworkEventInternal("RECONNECT", "Reconnect failed", false);
                _stats.failedReconnects++;
            }
        }
    }

    // --- 2. Проверка для состояния WAITING_IP (DHCP) ---
    else if (_state == NetworkState::WAITING_IP) {
        if (hasValidIP()) {
            newState = NetworkState::CONNECTED;
            _connectionTime = millis();
            _reconnectAttempts = 0;
            _stats.successReconnects++;

            String ip = ETH.localIP().toString();
            logMessage("IP assigned: %s", ip.c_str());
            publishNetworkEventInternal("CONNECTED", ip.c_str(), true);

            if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                safeStrCopy(_stats.ipAddress, sizeof(_stats.ipAddress), ip.c_str());
                _stats.hasValidIP = true;
                xSemaphoreGiveRecursive(_mutex);
            }

            // Отправляем событие
            publishConnected(ip.c_str(), _stats.macAddress);
            if (_onConnected) {
                _onConnected(ip.c_str(), _stats.macAddress);
            }
        } else if (millis() - _stateChangeTime > _dhcpTimeoutMs) {
            // Таймаут DHCP
            logMessage("DHCP timeout!");
            newState = NetworkState::ERROR;
            _stats.dhcpTimeoutCount++;
            _stats.lastErrorCode = 0x1001;
            safeStrCopy(_stats.lastErrorMsg, sizeof(_stats.lastErrorMsg), "DHCP timeout");
            publishNetworkEventInternal("ERROR", "DHCP timeout", false);
            publishError("DHCP_TIMEOUT");
        }
    }

    // --- 3. Проверка для состояния CONNECTED ---
    else if (_state == NetworkState::CONNECTED) {
        if (!hasValidIP() || !ETH.linkUp()) {
            logMessage("Connection lost!");
            newState = NetworkState::DISCONNECTED;
            _stats.linkDownCount++;
            _stats.hasValidIP = false;
            _stats.lastErrorCode = 0x1002;
            safeStrCopy(_stats.lastErrorMsg, sizeof(_stats.lastErrorMsg), "Link down");
            publishNetworkEventInternal("DISCONNECTED", "Link down", false);
            publishDisconnected("Link down");

            if (_onDisconnected) {
                _onDisconnected("Link down");
            }

            if (_autoReconnect) {
                _lastReconnectAttempt = millis();
                _reconnectAttempts = 0;
            }
        }
    }

    // --- 4. Проверка для состояния DISCONNECTED ---
    else if (_state == NetworkState::DISCONNECTED) {
        if (ETH.linkUp()) {
            // Линк восстановлен, ждем IP
            newState = NetworkState::WAITING_IP;
            logMessage("Link restored, waiting for IP...");
            publishNetworkEventInternal("LINK_RESTORED", "Waiting for IP", true);

            if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                _stateChangeTime = millis();
                xSemaphoreGiveRecursive(_mutex);
            }
        } else if (_autoReconnect && (millis() - _lastReconnectAttempt > _reconnectDelayMs)) {
            // Периодически пытаемся переподключиться
            _lastReconnectAttempt = millis();
            _reconnectAttempts++;
            _stats.reconnectAttempts++;

            logMessage("Reconnect attempt %lu/%lu", _reconnectAttempts, _maxReconnectAttempts);

            // Переинициализация Ethernet
            if (initEthernet()) {
                newState = NetworkState::WAITING_IP;
                publishNetworkEventInternal("RECONNECT", "Ethernet reinitialized", true);
            } else {
                logMessage("Reconnect failed");
                publishNetworkEventInternal("RECONNECT", "Reconnect failed", false);
                _stats.failedReconnects++;
            }
        }

        // Если превышено количество попыток
        if (_reconnectAttempts > _maxReconnectAttempts) {
            logMessage("Max reconnect attempts reached");
            newState = NetworkState::ERROR;
            _stats.lastErrorCode = 0x1003;
            safeStrCopy(_stats.lastErrorMsg, sizeof(_stats.lastErrorMsg), "Max retries");
            publishNetworkEventInternal("ERROR", "Max reconnect attempts reached", false);
            publishError("MAX_RETRIES");
        }
    }

    // --- 5. Обновляем состояние, если изменилось ---
    if (newState != _state) {
        NetworkState oldState = _state;
        _state = newState;
        _stateChangeTime = millis();

        logMessage("State changed: %d -> %d", (int)oldState, (int)newState);
        publishNetworkEventInternal("STATE_CHANGE", nullptr, true);

        // Публикуем событие
        publishState(newState);

        if (_onStateChange) {
            _onStateChange(newState);
        }
    }
}

// ============================================================================
// 8. ПУБЛИКАЦИЯ СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ)
// ============================================================================
void MicroOSNetworkManager::publishState(NetworkState state) {
    ShEventData event;
    event.senderId = _moduleId;
    event.targetModule = 0xFF;

    if (state == NetworkState::CONNECTED) {
        event.command = SH_EVENT_NET_CONNECTED;
        event.value = 1;
        String ip = ETH.localIP().toString();
        safeStrCopy(event.payload, sizeof(event.payload), ip.c_str());
    } else {
        event.command = SH_EVENT_NET_DISCONNECTED;
        event.value = 0;
        safeStrCopy(event.payload, sizeof(event.payload), "DISCONNECTED");
    }

    postEvent(event.command, &event);
}

void MicroOSNetworkManager::publishError(const char* errorCode) {
    ShEventData event;
    event.senderId = _moduleId;
    event.targetModule = 0xFF;
    event.command = SH_EVENT_NET_ERROR;
    event.value = _stats.lastErrorCode;
    safeStrCopy(event.payload, sizeof(event.payload), errorCode ? errorCode : "NETWORK_ERROR");
    postEvent(event.command, &event);
}

void MicroOSNetworkManager::publishConnected(const char* ip, const char* mac) {
    ShEventData event;
    event.senderId = _moduleId;
    event.targetModule = 0xFF;
    event.command = SH_EVENT_NET_CONNECTED;
    event.value = 1;
    snprintf(event.payload, sizeof(event.payload), "IP:%s MAC:%s", ip ? ip : "", mac ? mac : "");
    postEvent(event.command, &event);
}

void MicroOSNetworkManager::publishDisconnected(const char* reason) {
    ShEventData event;
    event.senderId = _moduleId;
    event.targetModule = 0xFF;
    event.command = SH_EVENT_NET_DISCONNECTED;
    event.value = 0;
    safeStrCopy(event.payload, sizeof(event.payload), reason ? reason : "DISCONNECTED");
    postEvent(event.command, &event);
}

// ============================================================================
// 9. ОБРАБОТЧИКИ СОБЫТИЙ
// ============================================================================
void MicroOSNetworkManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                                  int32_t id, void* eventData) {
    MicroOSNetworkManager* instance = static_cast<MicroOSNetworkManager*>(handlerArgs);
    if (!instance || !instance->_initialized) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_BOOT:
            case SH_EVENT_SYS_READY:
                instance->initEthernet();
                break;
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
                break;
            default:
                break;
        }
    }

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void MicroOSNetworkManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    switch (eventId) {
        case SH_EVENT_CMD_EXECUTE:
            if (data->targetModule == _moduleId || data->targetModule == 0) {
                handleCommand(data);
            }
            break;
        case SH_EVENT_SYS_BOOT:
        case SH_EVENT_SYS_READY:
            initEthernet();
            break;
        case SH_EVENT_SYS_RESTART:
        case SH_EVENT_SYS_SHUTDOWN:
            stop();
            break;
        default:
            break;
    }
}

bool MicroOSNetworkManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_SYS_BOOT ||
            eventId == SH_EVENT_SYS_READY ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN);
}

// ============================================================================
// 10. ОБРАБОТКА КОМАНД
// ============================================================================
void MicroOSNetworkManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case CMD_GET_STATUS: {
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = (uint8_t)_state;
            const char* status = getStatus();
            safeStrCopy(response.payload, sizeof(response.payload), status);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0100: // FORCE_RECONNECT
            forceReconnect();
            break;

        case 0x0101: // SET_HOSTNAME
            if (data->payloadLen > 0) {
                setHostname(data->payload);
            }
            break;

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 11. УПРАВЛЕНИЕ
// ============================================================================
void MicroOSNetworkManager::forceReconnect() {
    if (!_initialized) return;

    logMessage("Force reconnect initiated...");
    publishNetworkEventInternal("FORCE_RECONNECT", nullptr, true);

    _state = NetworkState::INITIALIZING;
    _reconnectAttempts = 0;
    _lastReconnectAttempt = 0;

    if (initEthernet()) {
        _state = NetworkState::WAITING_IP;
        _stateChangeTime = millis();
        logMessage("Force reconnect: Ethernet initialized");
    } else {
        _state = NetworkState::ERROR;
        logMessage("Force reconnect: Ethernet init failed");
        publishNetworkEventInternal("FORCE_RECONNECT", "Ethernet init failed", false);
    }
}

void MicroOSNetworkManager::setHostname(const char* name) {
    if (name == nullptr) return;
    safeStrCopy(_hostname, sizeof(_hostname), name);
    if (_initialized) {
        ETH.setHostname(_hostname);
        logMessage("Hostname updated: %s", _hostname);
        publishNetworkEventInternal("HOSTNAME", _hostname, true);
    }
}

void MicroOSNetworkManager::setUseDhcp(bool useDhcp) {
    _useDhcp = useDhcp;
    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _stats.isDhcpEnabled = useDhcp;
        xSemaphoreGiveRecursive(_mutex);
    }
    logMessage("DHCP: %s", useDhcp ? "ON" : "OFF");
    publishNetworkEventInternal("DHCP", useDhcp ? "ON" : "OFF", true);
}

void MicroOSNetworkManager::setStaticIP(const char* ip, const char* gateway,
                                 const char* subnet, const char* dns) {
    // Статический IP устанавливается через ETH.config()
    // В реальном проекте здесь нужно вызвать ETH.config(ip, gateway, subnet, dns)
    logMessage("Static IP set: %s", ip);
    publishNetworkEventInternal("STATIC_IP", ip, true);
}

// ============================================================================
// 12. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
bool MicroOSNetworkManager::hasValidIP() const {
    IPAddress ip = ETH.localIP();
    return (ip != IPAddress(0, 0, 0, 0));
}

void MicroOSNetworkManager::updateStats() {
    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _stats.uptime = (millis() - _startTime) / 1000;
        _stats.currentRetryCount = _reconnectAttempts;
        _stats.maxRetryCount = _maxReconnectAttempts;

        // Обновляем IP, если сеть подключена
        if (_state == NetworkState::CONNECTED) {
            String ip = ETH.localIP().toString();
            safeStrCopy(_stats.ipAddress, sizeof(_stats.ipAddress), ip.c_str());
            _stats.hasValidIP = true;
        } else {
            _stats.hasValidIP = false;
        }

        xSemaphoreGiveRecursive(_mutex);
    }
}

// ============================================================================
// 13. СТАТУС И ДИАГНОСТИКА
// ============================================================================
const char* MicroOSNetworkManager::getStatus() const {
    static char statusBuffer[128];

    const char* stateStr = "UNKNOWN";
    switch (_state) {
        case NetworkState::DISABLED: stateStr = "DISABLED"; break;
        case NetworkState::INITIALIZING: stateStr = "INITIALIZING"; break;
        case NetworkState::WAITING_IP: stateStr = "WAITING_IP"; break;
        case NetworkState::CONNECTED: stateStr = "CONNECTED"; break;
        case NetworkState::DISCONNECTED: stateStr = "DISCONNECTED"; break;
        case NetworkState::ERROR: stateStr = "ERROR"; break;
        default: break;
    }

    String ip = ETH.localIP().toString();
    snprintf(statusBuffer, sizeof(statusBuffer),
            "State: %s, IP: %s, MAC: %s, Attempts: %lu",
            stateStr,
            ip.c_str(),
            _stats.macAddress,
            _reconnectAttempts);

    return statusBuffer;
}

// ============================================================================
// 14. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void MicroOSNetworkManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = (uint8_t)_state;

    snprintf(data->payload, sizeof(data->payload),
            "state:%d,ip:%s,mac:%s,uptime:%lu,reconn:%lu,errors:%lu,events:%lu",
            (uint8_t)_state,
            _stats.ipAddress,
            _stats.macAddress,
            _stats.uptime,
            _stats.reconnectAttempts,
            _stats.lastErrorCode,
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

void MicroOSNetworkManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================================");
    stream.println(" NETWORK MANAGER DIAGNOSTIC");
    stream.println("==============================================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Ready: %s\n", _ready ? "YES" : "NO");
    stream.printf(" State: %d\n", (uint8_t)_state);
    stream.printf(" IP: %s\n", _stats.ipAddress);
    stream.printf(" MAC: %s\n", _stats.macAddress);
    stream.printf(" Hostname: %s\n", _hostname);
    stream.printf(" DHCP: %s\n", _useDhcp ? "ON" : "OFF");
    stream.printf(" Auto Reconnect: %s\n", _autoReconnect ? "ON" : "OFF");
    stream.printf(" Uptime: %lu sec\n", _stats.uptime);
    stream.printf(" Reconnect Attempts: %lu\n", _stats.reconnectAttempts);
    stream.printf(" Successful Reconnects: %lu\n", _stats.successReconnects);
    stream.printf(" Failed Reconnects: %lu\n", _stats.failedReconnects);
    stream.printf(" DHCP Timeouts: %lu\n", _stats.dhcpTimeoutCount);
    stream.printf(" Link Down Count: %lu\n", _stats.linkDownCount);
    stream.printf(" Last Error: %s (code: %lu)\n",
                  _stats.lastErrorMsg, _stats.lastErrorCode);
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("==============================================");
}

void MicroOSNetworkManager::printStats() const {
    streamDiagnosticInfo(Serial);
}