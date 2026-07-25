// ============================================================================
// MqttManager.h - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление MQTT-подключением и обменом данными.
// Все события подключения и получения команд публикуются в шину событий.
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
// - Добавлен метод publishMqttEvent() для публикации через новую шину
// - Добавлен счетчик _totalEventsPublished для диагностики
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <ETH.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>

#include "core/IModule.h"
#include "core/ShEventData.h" // НОВОЕ: для констант событий

// ============================================================================
// 1. СОБЫТИЯ MQTT MANAGER (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
enum MqttEvents : int32_t {
    SH_EVENT_MQTT_CONNECTED = SH_EVENT_USER_BASE + 0x0600,
    SH_EVENT_MQTT_DISCONNECTED = SH_EVENT_USER_BASE + 0x0601,
    SH_EVENT_MQTT_MESSAGE_RECEIVED = SH_EVENT_USER_BASE + 0x0602,
    SH_EVENT_MQTT_PUBLISHED = SH_EVENT_USER_BASE + 0x0603,
    SH_EVENT_MQTT_ERROR = SH_EVENT_USER_BASE + 0x0604,
    SH_EVENT_MQTT_DISCOVERY_SENT = SH_EVENT_USER_BASE + 0x0605,
    SH_EVENT_MQTT_SUBSCRIBED = SH_EVENT_USER_BASE + 0x0606,
    SH_EVENT_MQTT_UNSUBSCRIBED = SH_EVENT_USER_BASE + 0x0607,
    SH_EVENT_MQTT_PING = SH_EVENT_USER_BASE + 0x0608
};

// ============================================================================
// 2. СТРУКТУРЫ ДАННЫХ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
struct MqttMessageEvent {
    char topic[64];
    char payload[256];
    uint16_t payloadLen;
    uint32_t timestamp;
    bool retained;
    int qos;
};

struct MqttStateEvent {
    bool connected;
    char brokerIp[16];
    uint16_t port;
    char clientId[32];
    uint32_t uptime;
};

enum class MqttPriority : uint8_t {
    PRIORITY_LOW = 0,
    PRIORITY_NORMAL = 1,
    PRIORITY_CRITICAL = 2
};

struct MqttClientParams {
    char brokerIp[16] = "";
    uint16_t port = 1883;
    char user[32] = "";
    char password[64] = "";
    char hostname[32] = "smart-device";
    bool useMqtt = false;
    bool isPureLocalMode = false;
    uint16_t keepAlive = 60;
    uint16_t socketTimeout = 10;
    bool cleanSession = true;
    bool willRetain = true;
    int willQos = 1;
};

struct MqttStats {
    uint32_t totalConnections = 0;
    uint32_t totalDisconnections = 0;
    uint32_t totalMessagesReceived = 0;
    uint32_t totalMessagesPublished = 0;
    uint32_t totalErrors = 0;
    uint32_t reconnectAttempts = 0;
    uint32_t lastConnectTime = 0;
    uint32_t lastDisconnectTime = 0;
    uint32_t uptime = 0;
    uint32_t pingSuccess = 0;
    uint32_t pingFail = 0;
    size_t throttledCount = 0;
    size_t queueSize = 0;
};

// ============================================================================
// 3. ОСНОВНОЙ КЛАСС (РАСШИРЕН)
// ============================================================================
class MqttManager : public IModule {
public:
    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    typedef std::function<void(const String& topic, const String& payload)> OnMqttMessageCallback;
    typedef std::function<void()> OnMqttDiscoveryCallback;
    typedef std::function<void(bool connected)> OnMqttConnectionCallback;
    typedef std::function<void(const MqttStats& stats)> OnStatsUpdateCallback;

    // === СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    static MqttManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    MqttManager();
    ~MqttManager();

    MqttManager(const MqttManager&) = delete;
    MqttManager& operator=(const MqttManager&) = delete;

    // === IModule (ВАШ, ДОПОЛНЕН) ===
    const char* getName() const override { return "MqttManager"; }
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_MQTT; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override {
        return _initialized && _client.connected();
    }

    // === СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ЖИЗНЕННЫЙ ЦИКЛ (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    void begin(const MqttClientParams& params);
    void end();
    void reset();

    // === ПУБЛИКАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    bool publishStateSmart(const char* name, float currentValue,
                          float minDelta, MqttPriority priority);
    bool publishState(const char* name, const char* state,
                     bool retain = false, int qos = 0);
    bool publishState(const char* name, const char* state,
                     bool retain, int qos, bool force);
    bool publishCommand(const char* command, const char* payload,
                       bool retain = false);
    bool publish(const char* topic, const char* payload,
                bool retain = false, int qos = 0);
    bool publish(const char* topic, const uint8_t* payload,
                size_t length, bool retain = false, int qos = 0);

    // === HA DISCOVERY (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    void registerHaEntity(const char* component, const char* name,
                         const char* devClass = "", const char* icon = "",
                         const char* stateClass = "", const char* unitOfMeasurement = "");
    void unregisterHaEntity(const char* component, const char* name);
    void sendAutoDiscovery();

    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setCallbacks(OnMqttMessageCallback msgCb, OnMqttDiscoveryCallback discoveryCb);
    void setConnectionCallback(OnMqttConnectionCallback cb) { _onConnectionCb = cb; }
    void setStatsCallback(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }

    // === СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    bool isConnected() const { return _client.connected(); }
    const char* getBaseTopic() const { return _baseTopic.c_str(); }
    const char* getBroadcastTopic() const { return _broadcastTopic.c_str(); }
    uint32_t getLastActivity() const { return _lastActivityMs; }
    const MqttClientParams& getParams() const { return _params; }
    MqttStats getStats() const { return _stats; }

    // === УПРАВЛЕНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setThrottleLimits(uint32_t lowInterval, uint32_t normalInterval, uint32_t criticalInterval);
    void setMaxQueueSize(size_t size) { _maxQueueSize = size; }
    void setRetryInterval(uint32_t ms) { _retryIntervalMs = ms; }
    void setMaxRetries(uint8_t count) { _maxRetries = count; }

    // === НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0) ===
    void publishMqttEvent(const char* eventType, const char* details, bool success);

    // === ДИАГНОСТИКА (ВАША, РАСШИРЕНА) ===
    void streamDiagnosticInfo(Stream& stream) const;

    // === ОБРАБОТКА (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    void handleCallback(char* topic, byte* payload, unsigned int length);

private:
    // === ВНУТРЕННИЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void connect();
    void disconnect();
    void setupSubscriptions();
    void checkBrokerAlive();
    void updateConnectionState(bool connected);
    void processReceivedMessage(const String& topic, const String& payload);
    void cleanupThrottling();
    bool shouldPublish(const char* name, float currentValue, float minDelta, uint32_t maxInterval);
    void updateStats();

    // === ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ) ===
    void publishStateEvent(bool connected);
    void publishMessageEvent(const String& topic, const String& payload);
    void publishErrorEvent(const char* errorCode);
    void publishDiscoveryEvent();
    void publishPingEvent();

    // === НОВЫЙ МЕТОД: ВНУТРЕННЯЯ ПУБЛИКАЦИЯ СОБЫТИЙ ===
    void publishMqttEventInternal(const char* eventType, const char* details, bool success);

    // === ОБРАБОТЧИКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);
    static void staticCallback(char* topic, byte* payload, unsigned int length);

    // === ЛОГИРОВАНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);

    // === СТРУКТУРА ТРОТТЛИНГА (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    struct ThrottlingEntry {
        char name[32];
        float lastValue;
        uint32_t lastSentMs;
        uint32_t lastIntervalMs;
    };

    // === ДАННЫЕ (ВАШИ, РАСШИРЕНЫ) ===
    WiFiClient _ethClient;
    PubSubClient _client;
    MqttClientParams _params;
    uint32_t _moduleId = MODULE_ID_MQTT;

    // Состояние
    volatile bool _wasConnected = false;
    volatile bool _discoverySent = false;
    bool _initialized = false;
    bool _enabled = false;
    bool _isConnecting = false;

    // Таймеры
    uint32_t _lastReconnectAttemptMs = 0;
    uint32_t _lastBrokerPingMs = 0;
    uint32_t _lastConnectMs = 0;
    uint32_t _lastActivityMs = 0;
    uint32_t _lastStatsUpdateMs = 0;

    // Настройки
    uint32_t _retryIntervalMs = 5000;
    uint8_t _maxRetries = 5;
    uint8_t _retryCount = 0;
    size_t _maxQueueSize = 100;

    // Троттлинг
    std::vector<ThrottlingEntry> _throttlingMap;
    size_t _maxThrottlingSlots = 30;
    uint32_t _throttleLowInterval = 3600000;
    uint32_t _throttleNormalInterval = 300000;
    uint32_t _throttleCriticalInterval = 30000;

    // Топики
    String _baseTopic;
    String _broadcastTopic;
    String _discoveryTopicPrefix = "homeassistant";

    // НОВОЕ: счетчик опубликованных событий
    uint32_t _totalEventsPublished = 0;

    // Колбэки
    OnMqttMessageCallback _onMessageCb = nullptr;
    OnMqttDiscoveryCallback _onDiscoveryCb = nullptr;
    OnMqttConnectionCallback _onConnectionCb = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;

    // Статистика
    MqttStats _stats;

    // Рекурсивный мьютекс
    SemaphoreHandle_t _mqttMutex = nullptr;

    // Для статического колбэка
    static MqttManager* _instance;

    // Константы
    static constexpr uint32_t MQTT_BUFFER_SIZE = 2048;
    static constexpr uint32_t KEEPALIVE_INTERVAL = 60;
    static constexpr uint32_t BROKER_PING_INTERVAL_MS = 30000;
    static constexpr uint32_t STATS_UPDATE_INTERVAL_MS = 60000;
};