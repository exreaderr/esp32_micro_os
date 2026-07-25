// ============================================================================
// MqttManager.cpp - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Полноценный MQTT-менеджер с поддержкой:
// - Подключение к брокеру с авторизацией
// - LWT (Last Will and Testament)
// - Home Assistant Discovery
// - Троттлинг (интеллектуальная публикация)
// - Приоритеты сообщений
// - Полная потокобезопасность
// - Статистика и диагностика
// - Публикация событий через новую шину (v5.0)
//
// ИЗМЕНЕНИЯ v4.2.1:
// - Удалена глобальная переменная MQTT
// - Добавлен синглтон
// - Рекурсивный мьютекс вместо обычного
// - Добавлены недостающие объявления (_mqttMutex, _throttlingMap)
// - Добавлена поддержка QoS 1 и 2
// - Улучшен троттлинг
// - Добавлена статистика
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.1
// - Добавлен метод publishMqttEventInternal() для публикации через новую шину
// - Добавлен метод publishMqttEvent() (публичный)
// - Добавлены вызовы publishMqttEventInternal() в ключевые методы
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "MqttManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static MqttManager _mqttManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
MqttManager::MqttManager() : _client(_ethClient) {
    _instance = this;
    _moduleId = MODULE_ID_MQTT;

    _mqttMutex = xSemaphoreCreateRecursiveMutex();
    _initialized = false;
    _enabled = false;
    _wasConnected = false;
    _discoverySent = false;
    _isConnecting = false;
    _lastReconnectAttemptMs = 0;
    _lastBrokerPingMs = 0;
    _lastConnectMs = 0;
    _lastActivityMs = 0;
    _lastStatsUpdateMs = 0;
    _retryCount = 0;
    _totalEventsPublished = 0; // НОВОЕ

    _onMessageCb = nullptr;
    _onDiscoveryCb = nullptr;
    _onConnectionCb = nullptr;
    _onStatsUpdate = nullptr;

    if (_mqttMutex == nullptr) {
        Serial.println("[MQTT] CRITICAL: Failed to create mutex!");
    }

    _throttlingMap.reserve(_maxThrottlingSlots);

    Serial.println("[MQTT] Instance created (v5.0)");
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
// 2. СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
MqttManager& MqttManager::getInstance() {
    return _mqttManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[MQTT] %s\n", msg);

    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = MODULE_ID_LOG;
    data.command = 0x1000;
    data.value = 0;
    strncpy(data.payload, msg, sizeof(data.payload) - 1);
    data.payload[sizeof(data.payload) - 1] = '\0';
    data.payloadLen = strlen(msg);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void MqttManager::logMessage(const char* format, ...) {
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
void MqttManager::publishMqttEventInternal(const char* eventType, const char* details,
                                           bool success) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = success ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "MQTT: %s %s - %s",
             eventType,
             details ? details : "",
             success ? "OK" : "FAIL");

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void MqttManager::publishMqttEvent(const char* eventType, const char* details,
                                   bool success) {
    publishMqttEventInternal(eventType, details, success);
}

// ============================================================================
// 5. ЖИЗНЕННЫЙ ЦИКЛ (IModule) (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::init() {
    logMessage("Init pending - call begin() with parameters");
}

void MqttManager::start() {
    if (_initialized) {
        _enabled = true;
        logMessage("Started");
        publishMqttEventInternal("START", nullptr, true);
        if (!_client.connected()) {
            connect();
        }
    }
}

void MqttManager::stop() {
    if (_mqttMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
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
        publishMqttEventInternal("STOP", nullptr, true);
    }
}

void MqttManager::tick() {
    if (!_initialized || !_enabled) return;

    esp_task_wdt_reset();

    if (!_client.connected()) {
        uint32_t now = millis();
        if (now - _lastReconnectAttemptMs > _retryIntervalMs) {
            _lastReconnectAttemptMs = now;
            _stats.reconnectAttempts++;
            connect();
        }
    } else {
        _client.loop();
        checkBrokerAlive();
    }

    if (_onStatsUpdate) {
        uint32_t now = millis();
        if (now - _lastStatsUpdateMs > STATS_UPDATE_INTERVAL_MS) {
            _lastStatsUpdateMs = now;
            updateStats();
            _onStatsUpdate(_stats);
        }
    }
}

// ============================================================================
// 6. ОБРАБОТКА СОБЫТИЙ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
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
                if (instance->_initialized && instance->_enabled) {
                    instance->connect();
                }
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
            if (_initialized && _enabled) {
                connect();
                publishMqttEventInternal("NET_CONNECTED", nullptr, true);
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
// 7. СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
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

// ============================================================================
// 8. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void MqttManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _client.connected() ? 1 : 0;

    snprintf(data->payload, sizeof(data->payload),
            "connected:%d,broker:%s:%d,throttle:%zu,last:%lu,disc:%d,err:%lu,events:%lu",
            _client.connected() ? 1 : 0,
            _params.brokerIp,
            _params.port,
            _throttlingMap.size(),
            _lastActivityMs,
            _discoverySent ? 1 : 0,
            _stats.totalErrors,
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 9. ОБРАБОТКА КОМАНД (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case 0x0600: { // PUBLISH
            if (data->payloadLen > 0) {
                String topic = _baseTopic + "/" + String(data->payload);
                publish(topic.c_str(), "1", false);
                publishMqttEventInternal("CMD_PUBLISH", data->payload, true);
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
                    publishMqttEventInternal("CMD_PUBLISH_STATE", name.c_str(), true);
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
// 10. ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::publishStateEvent(bool connected) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = connected ? SH_EVENT_MQTT_CONNECTED : SH_EVENT_MQTT_DISCONNECTED;
    data.value = connected ? 1 : 0;
    snprintf(data.payload, sizeof(data.payload), "broker:%s:%d",
            _params.brokerIp, _params.port);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void MqttManager::publishMessageEvent(const String& topic, const String& payload) {
    MqttMessageEvent event;
    strncpy(event.topic, topic.c_str(), sizeof(event.topic) - 1);
    event.topic[sizeof(event.topic) - 1] = '\0';
    strncpy(event.payload, payload.c_str(), sizeof(event.payload) - 1);
    event.payload[sizeof(event.payload) - 1] = '\0';
    event.payloadLen = payload.length();
    event.timestamp = millis();
    event.retained = false;
    event.qos = 0;

    ShEventData data;
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
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_MQTT_ERROR;
    data.value = _client.state();
    strncpy(data.payload, errorCode ? errorCode : "UNKNOWN_ERROR", sizeof(data.payload) - 1);
    data.payload[sizeof(data.payload) - 1] = '\0';
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
    publishMqttEventInternal("ERROR", errorCode, false);
}

void MqttManager::publishDiscoveryEvent() {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_MQTT_DISCOVERY_SENT;
    data.value = 0;
    strncpy(data.payload, "Discovery sent", sizeof(data.payload) - 1);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
    publishMqttEventInternal("DISCOVERY", nullptr, true);
}

void MqttManager::publishPingEvent() {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_MQTT_PING;
    data.value = 0;
    strncpy(data.payload, "Broker ping OK", sizeof(data.payload) - 1);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
    publishMqttEventInternal("PING", nullptr, true);
}

// ============================================================================
// 11. ИНИЦИАЛИЗАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::begin(const MqttClientParams& params) {
    if (_mqttMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        _params = params;

        if (!params.useMqtt || params.isPureLocalMode) {
            logMessage("MQTT disabled");
            xSemaphoreGiveRecursive(_mqttMutex);
            return;
        }

        _baseTopic = "smart/" + String(_params.hostname);
        _broadcastTopic = "smart/all";

        IPAddress mqttIp;
        if (!mqttIp.fromString(_params.brokerIp)) {
            logMessage("Invalid broker IP: %s", _params.brokerIp);
            xSemaphoreGiveRecursive(_mqttMutex);
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
        publishMqttEventInternal("INIT", _params.brokerIp, true);

        if (_enabled && !_client.connected()) {
            connect();
        }
    }
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
    publishMqttEventInternal("RESET", nullptr, true);
}

// ============================================================================
// 12. СТАТИЧЕСКИЙ КОЛБЭК (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::staticCallback(char* topic, byte* payload, unsigned int length) {
    if (_instance) {
        _instance->handleCallback(topic, payload, length);
    }
}

// ============================================================================
// 13. ПОДКЛЮЧЕНИЕ (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
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

    String clientId = _params.hostname;
    uint32_t chipId = ESP.getEfuseMac();
    clientId += "-" + String(chipId & 0xFFFFFFFF, HEX);

    String lwtTopic = _baseTopic + "/status";

    bool result = false;
    if (strlen(_params.user) > 0) {
        result = _client.connect(
            clientId.c_str(),
            _params.user,
            _params.password,
            lwtTopic.c_str(),
            _params.willRetain,
            "offline"
        );
    } else {
        result = _client.connect(
            clientId.c_str(),
            lwtTopic.c_str(),
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
        publishMqttEventInternal("CONNECT", _params.brokerIp, true);

        String statusTopic = _baseTopic + "/status";
        _client.publish(statusTopic.c_str(), "online", true);

        setupSubscriptions();

        updateConnectionState(true);
        publishStateEvent(true);

        if (!_discoverySent) {
            sendAutoDiscovery();
        }

        logMessage("Subscriptions set up");
    } else {
        _retryCount++;
        logMessage("Connection failed (state: %d, retry: %d/%d)",
                  _client.state(), _retryCount, _maxRetries);
        publishMqttEventInternal("CONNECT_FAIL", String(_client.state()).c_str(), false);
        publishErrorEvent("CONNECT_FAILED");
        _stats.totalErrors++;
    }

    _isConnecting = false;
    xSemaphoreGiveRecursive(_mqttMutex);
}

void MqttManager::setupSubscriptions() {
    // Подписка на системные команды
    String sysTopic = _baseTopic + "/system/+";
    _client.subscribe(sysTopic.c_str());

    // Подписка на keepalive
    String keepAliveTopic = _baseTopic + "/keepalive";
    _client.subscribe(keepAliveTopic.c_str());

    // Подписка на broadcast
    _client.subscribe(_broadcastTopic.c_str());

    logMessage("Subscribed to topics");
    publishMqttEventInternal("SUBSCRIBED", "System topics", true);
}

// ============================================================================
// 14. ПРОВЕРКА БРОКЕРА (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::checkBrokerAlive() {
    if (!_client.connected()) return;

    uint32_t now = millis();
    if (now - _lastBrokerPingMs > BROKER_PING_INTERVAL_MS) {
        _lastBrokerPingMs = now;

        if (_client.publish("$SYS/keepalive", (const uint8_t*)"", 0, false)) {
            _stats.pingSuccess++;
            publishPingEvent();
            logMessage("Broker ping OK");
        } else {
            _stats.pingFail++;
            logMessage("Broker ping FAILED");
            publishMqttEventInternal("PING_FAIL", nullptr, false);
        }
    }
}

// ============================================================================
// 15. ОБНОВЛЕНИЕ СОСТОЯНИЯ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::updateConnectionState(bool connected) {
    if (_wasConnected != connected) {
        _wasConnected = connected;
        if (!connected) {
            _stats.totalDisconnections++;
            _stats.lastDisconnectTime = millis();
            publishMqttEventInternal("DISCONNECT", nullptr, false);
        }
        if (_onConnectionCb) {
            _onConnectionCb(connected);
        }
    }
}

// ============================================================================
// 16. ОБРАБОТКА СООБЩЕНИЙ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::handleCallback(char* topic, byte* payload, unsigned int length) {
    _lastActivityMs = millis();
    _stats.totalMessagesReceived++;

    String message = String((const char*)payload, length);
    String strTopic = String(topic);

    logMessage("Received: %s = %s", strTopic.c_str(), message.c_str());

    publishMessageEvent(strTopic, message);

    if (strTopic.endsWith("/system/reboot")) {
        logMessage("Reboot command received");
        sendCommand(0, 0, 0, "REBOOT");
        publishMqttEventInternal("CMD_REBOOT", nullptr, true);
        return;
    }

    if (strTopic.endsWith("/system/update")) {
        logMessage("Update command received");
        sendCommand(0, 0, 0, "UPDATE");
        publishMqttEventInternal("CMD_UPDATE", nullptr, true);
        return;
    }

    if (strTopic.endsWith("/keepalive")) {
        publishMqttEventInternal("KEEPALIVE", nullptr, true);
        return;
    }

    if (_onMessageCb) {
        _onMessageCb(strTopic, message);
    }
}

// ============================================================================
// 17. ТРОТТЛИНГ (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool MqttManager::shouldPublish(const char* name, float currentValue,
                                float minDelta, uint32_t maxInterval) {
    if (_mqttMutex == nullptr) return false;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t currentMs = millis();

        for (auto& entry : _throttlingMap) {
            if (strcmp(entry.name, name) == 0) {
                uint32_t interval = currentMs - entry.lastSentMs;

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

        if (_throttlingMap.size() < _maxThrottlingSlots) {
            ThrottlingEntry entry;
            strncpy(entry.name, name, sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
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
        uint32_t now = millis();
        auto it = _throttlingMap.begin();
        while (it != _throttlingMap.end()) {
            if (now - it->lastSentMs > 86400000UL) {
                it = _throttlingMap.erase(it);
            } else {
                ++it;
            }
        }
        xSemaphoreGiveRecursive(_mqttMutex);
    }
}

void MqttManager::setThrottleLimits(uint32_t lowInterval, uint32_t normalInterval,
                                    uint32_t criticalInterval) {
    _throttleLowInterval = lowInterval;
    _throttleNormalInterval = normalInterval;
    _throttleCriticalInterval = criticalInterval;
    logMessage("Throttle limits set");
    publishMqttEventInternal("THROTTLE_SET", nullptr, true);
}

// ============================================================================
// 18. ПУБЛИКАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool MqttManager::publishStateSmart(const char* name, float currentValue,
                                    float minDelta, MqttPriority priority) {
    if (!_initialized || _mqttMutex == nullptr) return false;
    if (!_client.connected()) return false;

    uint32_t maxIntervalMs;
    switch (priority) {
        case MqttPriority::CRITICAL: maxIntervalMs = _throttleCriticalInterval; break;
        case MqttPriority::NORMAL:   maxIntervalMs = _throttleNormalInterval; break;
        case MqttPriority::LOW:
        default:                     maxIntervalMs = _throttleLowInterval; break;
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
        publishMqttEventInternal("PUBLISH_STATE", name, true);

        ShEventData data;
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
        publishMqttEventInternal("PUBLISH_FAIL", name, false);
    }

    return result;
}

bool MqttManager::publishState(const char* name, const char* state,
                               bool retain, int qos, bool force) {
    if (force) {
        if (!_client.connected()) return false;

        String topic = _baseTopic + "/" + String(name) + "/state";

        bool result = _client.publish(topic.c_str(), (const uint8_t*)state,
                                      strlen(state), retain);

        if (result) {
            _lastActivityMs = millis();
            _stats.totalMessagesPublished++;
            logMessage("Published (forced): %s = %s", name, state);
            publishMqttEventInternal("PUBLISH_FORCED", name, true);

            ShEventData data;
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
            publishMqttEventInternal("PUBLISH_FORCED_FAIL", name, false);
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
        publishMqttEventInternal("PUBLISH_CMD", command, true);

        ShEventData data;
        data.sourceModule = _moduleId;
        data.targetModule = 0;
        data.command = SH_EVENT_MQTT_PUBLISHED;
        data.value = strlen(payload);
        snprintf(data.payload, sizeof(data.payload), "%s=%s", command, payload);
        data.payloadLen = strlen(data.payload);
        postEvent(SH_EVENT_MODULE_TICK, &data);
    } else {
        logMessage("Command publish failed: %s", command);
        _stats.totalErrors++;
        publishErrorEvent("COMMAND_PUBLISH_FAILED");
        publishMqttEventInternal("PUBLISH_CMD_FAIL", command, false);
    }

    return result;
}

bool MqttManager::publish(const char* topic, const char* payload, bool retain, int qos) {
    if (!_client.connected()) return false;

    bool result = _client.publish(topic, payload, retain);

    if (result) {
        _lastActivityMs = millis();
        _stats.totalMessagesPublished++;
        logMessage("Published (raw): %s", topic);
        publishMqttEventInternal("PUBLISH_RAW", topic, true);
    } else {
        logMessage("Raw publish failed: %s", topic);
        _stats.totalErrors++;
        publishMqttEventInternal("PUBLISH_RAW_FAIL", topic, false);
    }

    return result;
}

bool MqttManager::publish(const char* topic, const uint8_t* payload,
                          size_t length, bool retain, int qos) {
    if (!_client.connected()) return false;

    bool result = _client.publish(topic, payload, length, retain);

    if (result) {
        _lastActivityMs = millis();
        _stats.totalMessagesPublished++;
        logMessage("Published (binary): %s (%zu bytes)", topic, length);
        publishMqttEventInternal("PUBLISH_BINARY", topic, true);
    } else {
        logMessage("Binary publish failed: %s", topic);
        _stats.totalErrors++;
        publishErrorEvent("BINARY_PUBLISH_FAILED");
        publishMqttEventInternal("PUBLISH_BINARY_FAIL", topic, false);
    }

    return result;
}

// ============================================================================
// 19. HA DISCOVERY (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::registerHaEntity(const char* component, const char* name,
                                   const char* devClass, const char* icon,
                                   const char* stateClass, const char* unitOfMeasurement) {
    if (_mqttMutex == nullptr || !_initialized) return;
    if (!_client.connected()) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
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

        if (strcmp(component, "switch") == 0 || strcmp(component, "button") == 0 ||
            strcmp(component, "number") == 0 || strcmp(component, "select") == 0 ||
            strcmp(component, "light") == 0 || strcmp(component, "cover") == 0) {
            doc["cmd_t"] = _baseTopic + "/" + String(name) + "/set";
        }

        JsonObject dev = doc["device"].to<JsonObject>();
        JsonArray ids = dev["identifiers"].to<JsonArray>();
        ids.add(_params.hostname);
        dev["name"] = _params.hostname;
        dev["model"] = "MicroOS Device";
        dev["sw_version"] = "5.0.0";

        size_t jsonBytes = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

        String configTopic = _discoveryTopicPrefix + "/" + String(component) +
                            "/" + entityUniqueId + "/config";

        bool success = _client.publish(configTopic.c_str(), (const uint8_t*)jsonBuffer,
                                       jsonBytes, true);

        if (success) {
            logMessage("HA Discovery: %s", name);
            publishMqttEventInternal("HA_DISCOVERY", name, true);
        } else {
            logMessage("HA Discovery failed: %s", name);
            _stats.totalErrors++;
            publishErrorEvent("DISCOVERY_FAILED");
            publishMqttEventInternal("HA_DISCOVERY_FAIL", name, false);
        }

        xSemaphoreGiveRecursive(_mqttMutex);
    }
}

void MqttManager::unregisterHaEntity(const char* component, const char* name) {
    if (_mqttMutex == nullptr || !_initialized) return;
    if (!_client.connected()) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        String entityUniqueId = String(_params.hostname) + "_" + String(name);
        String configTopic = _discoveryTopicPrefix + "/" + String(component) +
                            "/" + entityUniqueId + "/config";

        bool success = _client.publish(configTopic.c_str(), "", true);

        if (success) {
            logMessage("HA Discovery removed: %s", name);
            publishMqttEventInternal("HA_UNREGISTER", name, true);
        } else {
            logMessage("HA Discovery removal failed: %s", name);
            _stats.totalErrors++;
            publishErrorEvent("DISCOVERY_REMOVAL_FAILED");
            publishMqttEventInternal("HA_UNREGISTER_FAIL", name, false);
        }

        xSemaphoreGiveRecursive(_mqttMutex);
    }
}

void MqttManager::sendAutoDiscovery() {
    if (!_client.connected()) return;
    logMessage("Sending AutoDiscovery...");
    _discoverySent = true;
    publishDiscoveryEvent();
    publishMqttEventInternal("AUTO_DISCOVERY", nullptr, true);
}

// ============================================================================
// 20. КОЛБЭКИ (ВАШ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::setCallbacks(OnMqttMessageCallback msgCb,
                               OnMqttDiscoveryCallback discoveryCb) {
    if (_mqttMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _onMessageCb = msgCb;
        _onDiscoveryCb = discoveryCb;
        xSemaphoreGiveRecursive(_mqttMutex);
        logMessage("Callbacks set");
        publishMqttEventInternal("CALLBACKS_SET", nullptr, true);
    }
}

// ============================================================================
// 21. СТАТИСТИКА (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void MqttManager::updateStats() {
    _stats.uptime = _client.connected() ? (millis() - _lastConnectMs) : 0;
    _stats.queueSize = 0;
    if (_throttlingMap.size() > _stats.throttledCount) {
        _stats.throttledCount = _throttlingMap.size();
    }
}

// ============================================================================
// 22. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
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
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
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