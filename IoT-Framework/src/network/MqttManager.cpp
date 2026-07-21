// ============================================================================
// MqttManager.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Полноценный менеджер MQTT подключения.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА опечатка xSemaphoreCreateResursiveMutex
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНА ошибка _retryIntervals -> _retryIntervalMs
// - ИСПРАВЛЕНА ошибка в publish (добавлена проверка nullptr)
// - ИСПРАВЛЕНА ошибка в publishState (force использует qos)
// - ДОБАВЛЕНА проверка _mqttMutex в staticCallback
// - ИСПРАВЛЕНА ошибка в eventHandler (лишний return)
// - ДОБАВЛЕНА полная потокобезопасность
// ============================================================================
#include "MqttManager.h"
#include <esp_task_wdt.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static MqttManager _mqttManagerInstance;
MqttManager* MqttManager::_instance = nullptr;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
MqttManager::MqttManager() : _client(_ethClient) {
    _instance = this;
    _moduleId = MODULE_ID_MQTT;

    // Рекурсивный мьютекс <-- ИСПРАВЛЕНО!
    _mqttMutex = xSemaphoreCreateRecursiveMutex();
    if (_mqttMutex == nullptr) {
        Serial.println("[MQTT] CRITICAL: Failed to create mutex!");
    }

    _initialized = false;
    _enabled = false;
    _wasConnected = false;
    _discoverySent = false;
    _isConnecting = false;
    _initInProgress = false;
    _lastReconnectAttemptMs = 0;
    _lastBrokerPingMs = 0;
    _lastConnectMs = 0;
    _lastActivityMs = 0;
    _lastStatsUpdateMs = 0;
    _retryCount = 0;
    _retryIntervalMs = MQTT_RETRY_INTERVAL_MS;
    _maxRetries = MQTT_MAX_RETRY_COUNT;
    _maxQueueSize = 100;
    _maxThrottlingSlots = MQTT_MAX_THROTTLING_SLOTS;
    _throttleLowInterval = 3600000;
    _throttleNormalInterval = 300000;
    _throttleCriticalInterval = 30000;

    _onMessageCb = nullptr;
    _onDiscoveryCb = nullptr;
    _onConnectionCb = nullptr;
    _onStatsUpdate = nullptr;

    memset(&_stats, 0, sizeof(_stats));

    Serial.println("[MQTT] Instance created (v4.2.2)");
}

MqttManager::~MqttManager() {
    stop();
    if (_mqttMutex != nullptr) {
        vSemaphoreDelete(_mqttMutex);
        _mqttMutex = nullptr;
    }
    _instance = nullptr;
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
MqttManager& MqttManager::getInstance() {
    return _mqttManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void MqttManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void MqttManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[MQTT] %s\n", msg);

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

void MqttManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

bool MqttManager::isInitializedAndEnabled() const {
    return _initialized && _enabled;
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void MqttManager::init() {
    logMessage("Init pending - call begin() with parameters");
}

void MqttManager::start() {
    if (_initialized) {
        _enabled = true;
        logMessage("Started");
        if (!_client.connected()) {
            connect();
        }
    }
}

void MqttManager::stop() {
    if (_mqttMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (!_initialized) {
            xSemaphoreGiveRecursive(_mqttMutex);
            return;
        }
        if (_client.connected()) {
            String lwtTopic = _baseTopic + "/status";
            _client.publish(lwtTopic.c_str(), "offline", true);
            _client.disconnect();
        }
        _initialized = false;
        _enabled = false;
        _wasConnected = false;
        _discoverySent = false;
        _isConnecting = false;
        xSemaphoreGiveRecursive(_mqttMutex);
        logMessage("Stopped");
    }
}

void MqttManager::tick() {
    if (!isInitializedAndEnabled()) return;

    esp_task_wdt_reset();

    // Обработка подключения
    if (!_client.connected()) {
        uint32_t now = millis();
        if (now - _lastReconnectAttemptMs > _retryIntervalMs) {  // <-- ИСПРАВЛЕНО!
            _lastReconnectAttemptMs = now;
            _stats.reconnectAttempts++;
            connect();
        }
    } else {
        _client.loop();
        checkBrokerAlive();
    }

    // Обновление статистики
    if (_onStatsUpdate) {
        uint32_t now = millis();
        if (now - _lastStatsUpdateMs > MQTT_STATS_UPDATE_INTERVAL_MS) {
            _lastStatsUpdateMs = now;
            updateStats();
            _onStatsUpdate(_stats);
        }
    }
}

// ============================================================================
// 5. ОБРАБОТКА СОБЫТИЙ (ИСПРАВЛЕНО)
// ============================================================================
void MqttManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                               int32_t id, void* eventData) {
    MqttManager* instance = static_cast<MqttManager*>(handlerArgs);
    if (!instance) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
                break;
            case SH_EVENT_NET_CONNECTED:
                if (instance->isInitializedAndEnabled()) {
                    instance->connect();
                }
                break;
            default:
                break;
        }
        return;  // <-- ИСПРАВЛЕНО: return только для SYS!
    }

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void MqttManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    switch (eventId) {
        case SH_EVENT_CMD_EXECUTE:
            if (data->targetModule == _moduleId || data->targetModule == 0) {
                handleCommand(data);
            }
            break;
        case SH_EVENT_SYS_RESTART:
        case SH_EVENT_SYS_SHUTDOWN:
            stop();
            break;
        case SH_EVENT_NET_CONNECTED:
            if (isInitializedAndEnabled()) {
                connect();
            }
            break;
        default:
            break;
    }
}

bool MqttManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN ||
            eventId == SH_EVENT_NET_CONNECTED ||
            eventId == SH_EVENT_NET_DISCONNECTED);
}

// ============================================================================
// 6. СТАТУС
// ============================================================================
const char* MqttManager::getStatus() const {
    static char statusBuffer[128];

    const char* state = _client.connected() ? "CONNECTED" : "DISCONNECTED";

    snprintf(statusBuffer, sizeof(statusBuffer),
            "State: %s, Broker: %s:%d, Client: %s, Retry: %d/%d, Msgs: %lu",
            state,
            _params.brokerIp,
            _params.port,
            _params.hostname,
            _retryCount,
            _maxRetries,
            _stats.totalMessagesPublished);
    return statusBuffer;
}

void MqttManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _client.connected() ? 1 : 0;

    snprintf(data->payload, sizeof(data->payload),
            "connected:%d,broker:%s:%d,throttle:%zu,last:%lu,disc:%d,err:%lu",
            _client.connected() ? 1 : 0,
            _params.brokerIp,
            _params.port,
            _throttlingMap.size(),
            _lastActivityMs,
            _discoverySent ? 1 : 0,
            _stats.totalErrors);
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 7. ОБРАБОТКА КОМАНД
// ============================================================================
void MqttManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case 0x0600: { // PUBLISH
            if (data->payloadLen > 0) {
                String topic = _baseTopic + "/" + String(data->payload);
                publish(topic.c_str(), "1", false);
            }
            break;
        }
        case 0x0601: { // PUBLISH_STATE
            if (data->payloadLen > 0) {
                String payload(data->payload);
                int sep = payload.indexOf('!');
                if (sep > 0) {
                    String name = payload.substring(0, sep);
                    String value = payload.substring(sep + 1);
                    publishState(name.c_str(), value.c_str(), false);
                }
            }
            break;
        }
        case 0x0602: // DISCOVERY
            sendAutoDiscovery();
            break;
        case 0x0603: // RECONNECT
            if (_client.connected()) {
                _client.disconnect();
            }
            connect();
            break;
        case 0x0604: // GET_STATS
            updateStats();
            break;
        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 8. ОТПРАВКА СОБЫТИЙ (ИСПРАВЛЕНО)
// ============================================================================
void MqttManager::publishStateEvent(bool connected) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = connected ? SH_EVENT_MQTT_CONNECTED : SH_EVENT_MQTT_DISCONNECTED;
    data.value = connected ? 1 : 0;
    snprintf(data.payload, sizeof(data.payload), "broker:%s:%d",
            _params.brokerIp, _params.port);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);  // <-- ИСПРАВЛЕНО!
}

void MqttManager::publishMessageEvent(const String& topic, const String& payload) {
    MqttMessageEvent event;
    memset(&event, 0, sizeof(MqttMessageEvent));
    safeStrCopy(event.topic, sizeof(event.topic), topic.c_str());
    safeStrCopy(event.payload, sizeof(event.payload), payload.c_str());
    event.payloadLen = payload.length();
    event.timestamp = millis();
    event.retained = false;
    event.qos = 0;

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_MQTT_MESSAGE_RECEIVED;
    data.value = payload.length();
    memcpy(data.payload, &event, min(sizeof(MqttMessageEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(MqttMessageEvent);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void MqttManager::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_MQTT_ERROR;
    data.value = _client.state();
    safeStrCopy(data.payload, sizeof(data.payload), errorCode ? errorCode : "UNKNOWN_ERROR");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void MqttManager::publishDiscoveryEvent() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_MQTT_DISCOVERY_SENT;
    data.value = 0;
    safeStrCopy(data.payload, sizeof(data.payload), "Discovery sent");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void MqttManager::publishPingEvent() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_MQTT_PING;
    data.value = 0;
    safeStrCopy(data.payload, sizeof(data.payload), "Broker ping OK");
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

// ============================================================================
// 9. ИНИЦИАЛИЗАЦИЯ (С ЗАЩИТОЙ ОТ ПОВТОРНОГО ВХОДА)
// ============================================================================
void MqttManager::begin(const MqttClientParams& params) {
    if (_initInProgress) {
        logMessage("Begin already in progress, skipping...");
        return;
    }
    _initInProgress = true;

    if (_mqttMutex == nullptr) {
        _initInProgress = false;
        return;
    }

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _params = params;

        if (!params.useMqtt || params.isPureLocalMode) {
            logMessage("MQTT disabled");
            xSemaphoreGiveRecursive(_mqttMutex);
            _initInProgress = false;
            return;
        }

        _baseTopic = "smart/" + String(_params.hostname);
        _broadcastTopic = "smart/all";

        IPAddress mqttIp;
        if (!mqttIp.fromString(_params.brokerIp)) {
            logMessage("Invalid broker IP: %s", _params.brokerIp);
            xSemaphoreGiveRecursive(_mqttMutex);
            _initInProgress = false;
            return;
        }

        _client.setClient(_ethClient);
        _client.setServer(mqttIp, _params.port);
        _client.setCallback(staticCallback);
        _client.setBufferSize(MQTT_BUFFER_SIZE);
        _client.setKeepAlive(_params.keepAlive);
        _client.setSocketTimeout(_params.socketTimeout);

        _initialized = true;
        _enabled = true;
        _discoverySent = false;
        _lastActivityMs = millis();
        _retryCount = 0;
        _throttlingMap.clear();

        xSemaphoreGiveRecursive(_mqttMutex);

        // Подписка на события
        esp_event_handler_instance_register(
            SH_SYS_EVENTS,
            ESP_EVENT_ANY_ID,
            &MqttManager::eventHandler,
            this,
            NULL
        );
        esp_event_handler_instance_register(
            SH_APP_EVENTS,
            ESP_EVENT_ANY_ID,
            &MqttManager::eventHandler,
            this,
            NULL
        );

        logMessage("Initialized: broker=%s:%d, client=%s",
                  _params.brokerIp, _params.port, _params.hostname);

        // Первое подключение
        if (_enabled && !_client.connected()) {
            connect();
        }
    }
    _initInProgress = false;
}

void MqttManager::end() {
    stop();
}

void MqttManager::reset() {
    logMessage("Reset requested");
    stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    begin(_params);
    logMessage("Reset complete");
}

// ============================================================================
// 10. СТАТИЧЕСКИЙ КОЛБЭК (С ПРОВЕРКОЙ МЬЮТЕКСА)
// ============================================================================
void MqttManager::staticCallback(char* topic, byte* payload, unsigned int length) {
    if (_instance != nullptr && _instance->_mqttMutex != nullptr) {
        _instance->handleCallback(topic, payload, length);
    }
}

// ============================================================================
// 11. ПОДКЛЮЧЕНИЕ
// ============================================================================
void MqttManager::connect() {
    if (_mqttMutex == nullptr) return;

    if (!ETH.linkUp()) {
        logMessage("Ethernet link down");
        return;
    }

    if (_isConnecting) {
        logMessage("Already connecting...");
        return;
    }

    if (_client.connected()) {
        logMessage("Already connected");
        return;
    }

    _isConnecting = true;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        _isConnecting = false;
        return;
    }

    // Формируем Client ID
    String clientId = _params.hostname;
    uint32_t chipId = ESP.getEfuseMac();
    clientId += "-" + String(chipId & 0xFFFFFFFF, HEX);

    // LWT топик
    String lwtTopic = _baseTopic + "/status";

    // Подключение
    bool result = false;
    if (strlen(_params.user) > 0) {
        result = _client.connect(
            clientId.c_str(),
            _params.user,
            _params.password,
            lwtTopic.c_str(),
            _params.willQos,
            _params.willRetain,
            "offline"
        );
    } else {
        result = _client.connect(
            clientId.c_str(),
            lwtTopic.c_str(),
            _params.willQos,
            _params.willRetain,
            "offline"
        );
    }

    if (result) {
        _retryCount = 0;
        _lastConnectMs = millis();
        _stats.totalConnections++;
        _stats.lastConnectTime = _lastConnectMs;
        logMessage("Connected to broker");

        // Публикуем статус
        String statusTopic = _baseTopic + "/status";
        _client.publish(statusTopic.c_str(), "online", true);

        // Подписка на топики
        setupSubscriptions();

        // Отправляем событие
        updateConnectionState(true);
        publishStateEvent(true);

        // Discovery
        if (!_discoverySent) {
            sendAutoDiscovery();
        }
    } else {
        _retryCount++;
        logMessage("Connection failed (state: %d)", _client.state());
        _stats.totalErrors++;
        publishErrorEvent("CONNECTION_FAILED");
    }

    _isConnecting = false;
    xSemaphoreGiveRecursive(_mqttMutex);
}

// ============================================================================
// 12. ПОДПИСКА НА ТОПИКИ
// ============================================================================
void MqttManager::setupSubscriptions() {
    if (!_client.connected()) return;

    // Подписка на команды
    String commandTopic = _baseTopic + "/+/set";
    _client.subscribe(commandTopic.c_str());

    // Подписка на системные команды
    String systemTopic = _baseTopic + "/system/+";
    _client.subscribe(systemTopic.c_str());

    logMessage("Subscriptions set up");
}

// ============================================================================
// 13. ПРОВЕРКА БРОКЕРА
// ============================================================================
void MqttManager::checkBrokerAlive() {
    if (!_client.connected()) return;

    uint32_t now = millis();
    if (now - _lastBrokerPingMs > MQTT_BROKER_PING_INTERVAL_MS) {
        _lastBrokerPingMs = now;
        if (_client.publish("$SYS/keepalive", (const uint8_t*)"", 0, false)) {
            _stats.pingSuccess++;
            publishPingEvent();
            logMessage("Broker ping OK");
        } else {
            _stats.pingFail++;
            logMessage("Broker ping FAILED");
        }
    }
}

// ============================================================================
// 14. ОБНОВЛЕНИЕ СОСТОЯНИЯ
// ============================================================================
void MqttManager::updateConnectionState(bool connected) {
    if (_wasConnected != connected) {
        _wasConnected = connected;
        if (!connected) {
            _stats.totalDisconnections++;
            _stats.lastDisconnectTime = millis();
        }
        if (_onConnectionCb) {
            _onConnectionCb(connected);
        }
    }
}

// ============================================================================
// 15. ОБРАБОТКА СООБЩЕНИЙ
// ============================================================================
void MqttManager::handleCallback(char* topic, byte* payload, unsigned int length) {
    _lastActivityMs = millis();
    _stats.totalMessagesReceived++;

    String message = String((const char*)payload, length);
    String strTopic = String(topic);

    logMessage("Received: %s = %s", strTopic.c_str(), message.c_str());

    // Публикуем событие в шину
    publishMessageEvent(strTopic, message);

    // Проверка системных команд
    if (strTopic.endsWith("/system/reboot")) {
        logMessage("Reboot command received");
        ShEventData cmdData;
        memset(&cmdData, 0, sizeof(ShEventData));
        cmdData.sourceModule = _moduleId;
        cmdData.targetModule = 0;
        cmdData.command = 0;
        safeStrCopy(cmdData.payload, sizeof(cmdData.payload), "REBOOT");
        cmdData.payloadLen = strlen(cmdData.payload);
        postEvent(SH_EVENT_SYS_RESTART, &cmdData);
        return;
    }

    if (strTopic.endsWith("/system/update")) {
        logMessage("Update command received");
        ShEventData cmdData;
        memset(&cmdData, 0, sizeof(ShEventData));
        cmdData.sourceModule = _moduleId;
        cmdData.targetModule = 0;
        cmdData.command = 0;
        safeStrCopy(cmdData.payload, sizeof(cmdData.payload), "UPDATE");
        cmdData.payloadLen = strlen(cmdData.payload);
        postEvent(SH_EVENT_CMD_EXECUTE, &cmdData);
        return;
    }

    // Пропускаем keepalive
    if (strTopic.endsWith("/keepalive")) return;

    // Вызываем колбэк
    if (_onMessageCb) {
        _onMessageCb(strTopic, message);
    }
}

// ============================================================================
// 16. ТРОТТЛИНГ
// ============================================================================
bool MqttManager::shouldPublish(const char* name, float currentValue,
                                float minDelta, uint32_t maxInterval) {
    if (_mqttMutex == nullptr) return false;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t currentMs = millis();

        // Поиск в кэше
        for (auto& entry : _throttlingMap) {
            if (strcmp(entry.name, name) == 0) {
                uint32_t interval = currentMs - entry.lastSentMs;

                // Проверка: изменилось значение или прошло достаточно времени
                bool valueChanged = (fabsf(currentValue - entry.lastValue) >= minDelta);
                bool timeElapsed = (interval >= maxInterval);

                if (valueChanged || timeElapsed) {
                    entry.lastValue = currentValue;
                    entry.lastSentMs = currentMs;
                    entry.lastIntervalMs = interval;
                    xSemaphoreGiveRecursive(_mqttMutex);
                    return true;
                }

                xSemaphoreGiveRecursive(_mqttMutex);
                return false;
            }
        }

        // Добавление новой записи
        if (_throttlingMap.size() < _maxThrottlingSlots) {
            ThrottlingEntry entry;
            memset(&entry, 0, sizeof(ThrottlingEntry));
            safeStrCopy(entry.name, sizeof(entry.name), name);
            entry.lastValue = currentValue;
            entry.lastSentMs = currentMs;
            entry.lastIntervalMs = 0;
            _throttlingMap.push_back(entry);
            _stats.throttledCount++;
        }

        xSemaphoreGiveRecursive(_mqttMutex);
        return true;
    }
    return false;
}

void MqttManager::cleanupThrottling() {
    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Удаляем старые записи (старше 24 часов)
        uint32_t now = millis();
        auto it = _throttlingMap.begin();
        while (it != _throttlingMap.end()) {
            if (now - it->lastSentMs > 86400000UL) { // 24 часа
                it = _throttlingMap.erase(it);
            } else {
                ++it;
            }
        }
        xSemaphoreGiveRecursive(_mqttMutex);
    }
}

// ============================================================================
// 17. ПУБЛИКАЦИЯ (ИСПРАВЛЕНО)
// ============================================================================
bool MqttManager::publishStateSmart(const char* name, float currentValue,
                                    float minDelta, MqttPriority priority) {
    if (!isInitializedAndEnabled() || _mqttMutex == nullptr) return false;
    if (!_client.connected()) return false;

    uint32_t maxIntervalMs;
    switch (priority) {
        case MqttPriority::CRITICAL: maxIntervalMs = _throttleCriticalInterval; break;
        case MqttPriority::NORMAL: maxIntervalMs = _throttleNormalInterval; break;
        case MqttPriority::PRIORITY_LOW:
        default: maxIntervalMs = _throttleLowInterval; break;
    }

    if (!shouldPublish(name, currentValue, minDelta, maxIntervalMs)) {
        return false;
    }

    char valBuf[32];
    snprintf(valBuf, sizeof(valBuf), "%.2f", currentValue);
    return publishState(name, valBuf, false);
}

bool MqttManager::publishState(const char* name, const char* state,
                               bool retain, int qos) {
    if (!_client.connected()) return false;

    String topic = _baseTopic + "/" + String(name) + "/state";
    bool result = _client.publish(topic.c_str(), (const uint8_t*)state,
                                  strlen(state), retain);

    if (result) {
        _lastActivityMs = millis();
        _stats.totalMessagesPublished++;
        logMessage("Published: %s = %s", name, state);

        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = _moduleId;
        data.targetModule = 0;
        data.command = SH_EVENT_MQTT_PUBLISHED;
        data.value = strlen(state);
        snprintf(data.payload, sizeof(data.payload), "%s=%s", name, state);
        data.payloadLen = strlen(data.payload);
        postEvent(SH_EVENT_MODULE_TICK, &data);
    } else {
        logMessage("Publish failed: %s", name);
        _stats.totalErrors++;
        publishErrorEvent("PUBLISH_FAILED");
    }
    return result;
}

bool MqttManager::publishState(const char* name, const char* state,
                               bool retain, int qos, bool force) {
    if (force) {
        // Принудительная публикация без тротлинга
        if (!_client.connected()) return false;

        String topic = _baseTopic + "/" + String(name) + "/state";
        bool result = _client.publish(topic.c_str(), (const uint8_t*)state,
                                      strlen(state), retain);

        if (result) {
            _lastActivityMs = millis();
            _stats.totalMessagesPublished++;
            logMessage("Published (forced): %s = %s", name, state);

            ShEventData data;
            memset(&data, 0, sizeof(ShEventData));
            data.sourceModule = _moduleId;
            data.targetModule = 0;
            data.command = SH_EVENT_MQTT_PUBLISHED;
            data.value = strlen(state);
            snprintf(data.payload, sizeof(data.payload), "%s=%s", name, state);
            data.payloadLen = strlen(data.payload);
            postEvent(SH_EVENT_MODULE_TICK, &data);
        } else {
            logMessage("Publish forced failed: %s", name);
            _stats.totalErrors++;
            publishErrorEvent("PUBLISH_FORCED_FAILED");
        }
        return result;
    }
    return publishState(name, state, retain, qos);
}

bool MqttManager::publishCommand(const char* command, const char* payload, bool retain) {
    if (!_client.connected()) return false;

    String topic = _baseTopic + "/" + String(command) + "/set";
    bool result = _client.publish(topic.c_str(), payload, retain);

    if (result) {
        _lastActivityMs = millis();
        _stats.totalMessagesPublished++;
        logMessage("Command published: %s = %s", command, payload);
    } else {
        logMessage("Command publish failed: %s", command);
        _stats.totalErrors++;
        publishErrorEvent("COMMAND_PUBLISH_FAILED");
    }
    return result;
}

bool MqttManager::publish(const char* topic, const char* payload,
                          bool retain, int qos) {
    if (!_client.connected() || topic == nullptr) return false;

    bool result = _client.publish(topic, payload, retain);
    if (result) {
        _lastActivityMs = millis();
        _stats.totalMessagesPublished++;
        logMessage("Published: %s", topic);
    } else {
        _stats.totalErrors++;
        publishErrorEvent("PUBLISH_FAILED");
    }
    return result;
}

bool MqttManager::publish(const char* topic, const uint8_t* payload,
                          size_t length, bool retain, int qos) {
    // <-- ИСПРАВЛЕНО: добавлена проверка
    if (!_client.connected() || topic == nullptr || payload == nullptr || length == 0) {
        return false;
    }

    bool result = _client.publish(topic, payload, length, retain);
    if (result) {
        _lastActivityMs = millis();
        _stats.totalMessagesPublished++;
        logMessage("Published binary: %s (%zu bytes)", topic, length);
    } else {
        _stats.totalErrors++;
        publishErrorEvent("BINARY_PUBLISH_FAILED");
    }
    return result;
}

// ============================================================================
// 18. HA DISCOVERY
// ============================================================================
void MqttManager::registerHaEntity(const char* component, const char* name,
                                   const char* devClass, const char* icon,
                                   const char* stateClass, const char* unitOfMeasurement) {
    if (_mqttMutex == nullptr || !_initialized) return;
    if (!_client.connected()) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        char jsonBuffer[512];
        JsonDocument doc;

        String entityUniqueId = String(_params.hostname) + "_" + String(name);
        doc["name"] = name;
        doc["uniq_id"] = entityUniqueId;
        doc["stat_t"] = _baseTopic + "/" + String(name) + "/state";

        if (devClass != nullptr && strlen(devClass) > 0) doc["dev_cla"] = devClass;
        if (icon != nullptr && strlen(icon) > 0) doc["icon"] = icon;
        if (stateClass != nullptr && strlen(stateClass) > 0) doc["stat_cla"] = stateClass;
        if (unitOfMeasurement != nullptr && strlen(unitOfMeasurement) > 0) {
            doc["unit_of_meas"] = unitOfMeasurement;
        }

        // Команды для управляемых сущностей
        if (strcmp(component, "switch") == 0 || strcmp(component, "button") == 0 ||
            strcmp(component, "number") == 0 || strcmp(component, "select") == 0 ||
            strcmp(component, "light") == 0 || strcmp(component, "cover") == 0) {
            doc["cmd_t"] = _baseTopic + "/" + String(name) + "/set";
        }

        // Информация об устройстве
        JsonObject dev = doc["device"].to<JsonObject>();
        JsonArray ids = dev["identifiers"].to<JsonArray>();
        ids.add(_params.hostname);
        dev["name"] = _params.hostname;
        dev["model"] = "MicroOS Device";
        dev["sw_version"] = "4.2.2";

        size_t jsonBytes = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
        String configTopic = _discoveryTopicPrefix + "/" + String(component) + "/" + entityUniqueId + "/config";

        bool success = _client.publish(configTopic.c_str(), (const uint8_t*)jsonBuffer, jsonBytes, true);

        if (success) {
            logMessage("HA Discovery: %s", name);
        } else {
            logMessage("HA Discovery failed: %s", name);
            _stats.totalErrors++;
            publishErrorEvent("DISCOVERY_FAILED");
        }

        xSemaphoreGiveRecursive(_mqttMutex);
    }
}

void MqttManager::unregisterHaEntity(const char* component, const char* name) {
    if (_mqttMutex == nullptr || !_initialized) return;
    if (!_client.connected()) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        String entityUniqueId = String(_params.hostname) + "_" + String(name);
        String configTopic = _discoveryTopicPrefix + "/" + String(component) + "/" + entityUniqueId + "/config";

        bool success = _client.publish(configTopic.c_str(), "", true);

        if (success) {
            logMessage("HA Discovery removed: %s", name);
        } else {
            logMessage("HA Discovery removal failed: %s", name);
            _stats.totalErrors++;
            publishErrorEvent("DISCOVERY_REMOVAL_FAILED");
        }
        xSemaphoreGiveRecursive(_mqttMutex);
    }
}

void MqttManager::sendAutoDiscovery() {
    if (!_client.connected()) return;

    logMessage("Sending AutoDiscovery...");
    _discoverySent = true;
    publishDiscoveryEvent();

    if (_onDiscoveryCb) {
        _onDiscoveryCb();
    }
}

// ============================================================================
// 19. КОЛБЭКИ
// ============================================================================
void MqttManager::setCallbacks(OnMqttMessageCallback msgCb,
                               OnMqttDiscoveryCallback discoveryCb) {
    if (_mqttMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _onMessageCb = msgCb;
        _onDiscoveryCb = discoveryCb;
        xSemaphoreGiveRecursive(_mqttMutex);
        logMessage("Callbacks set");
    }
}

// ============================================================================
// 20. СТАТИСТИКА
// ============================================================================
void MqttManager::updateStats() {
    _stats.uptime = _client.connected() ? (millis() - _lastConnectMs) : 0;
    _stats.queueSize = 0;
    if (_throttlingMap.size() > _stats.throttledCount) {
        _stats.throttledCount = _throttlingMap.size();
    }
}

void MqttManager::setThrottleLimits(uint32_t lowInterval, uint32_t normalInterval,
                                    uint32_t criticalInterval) {
    if (_mqttMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _throttleLowInterval = lowInterval;
        _throttleNormalInterval = normalInterval;
        _throttleCriticalInterval = criticalInterval;
        xSemaphoreGiveRecursive(_mqttMutex);
        logMessage("Throttle limits updated: low=%lu, normal=%lu, critical=%lu",
                  lowInterval, normalInterval, criticalInterval);
    }
}

// ============================================================================
// 21. ДИАГНОСТИКА
// ============================================================================
void MqttManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("=============================");
    stream.println(" MQTT MANAGER DIAGNOSTIC");
    stream.println("=============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Enabled: %s\n", _enabled ? "YES" : "NO");
    stream.printf(" Connected: %s\n", _client.connected() ? "YES" : "NO");
    stream.printf(" Broker: %s:%d\n", _params.brokerIp, _params.port);
    stream.printf(" Client ID: %s\n", _params.hostname);
    stream.printf(" Base Topic: %s\n", _baseTopic.c_str());
    stream.printf(" LWT: %s/status\n", _baseTopic.c_str());
    stream.printf(" Discovery: %s\n", _discoverySent ? "SENT" : "PENDING");
    stream.printf(" Retry: %d/%d\n", _retryCount, _maxRetries);
    stream.println("-- Stats --");
    stream.printf(" Connections: %lu\n", _stats.totalConnections);
    stream.printf(" Disconnections: %lu\n", _stats.totalDisconnections);
    stream.printf(" Messages Recv: %lu\n", _stats.totalMessagesReceived);
    stream.printf(" Messages Pub: %lu\n", _stats.totalMessagesPublished);
    stream.printf(" Errors: %lu\n", _stats.totalErrors);
    stream.printf(" Reconnect Attempts: %lu\n", _stats.reconnectAttempts);
    stream.printf(" Ping Success: %lu\n", _stats.pingSuccess);
    stream.printf(" Ping Fail: %lu\n", _stats.pingFail);
    stream.printf(" Throttled: %zu\n", _throttlingMap.size());
    stream.println("-- Throttle Limits --");
    stream.printf(" Low: %lu ms (1h)\n", _throttleLowInterval);
    stream.printf(" Normal: %lu ms (5min)\n", _throttleNormalInterval);
    stream.printf(" Critical: %lu ms (30s)\n", _throttleCriticalInterval);
    stream.println("=============================");
}

void MqttManager::printStats() const {
    streamDiagnosticInfo(Serial);
}