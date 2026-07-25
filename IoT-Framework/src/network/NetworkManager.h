// ============================================================================
// NetworkManager.h - РЕАКТИВНЫЙ МЕНЕДЖЕР СЕТЕВОГО СОЕДИНЕНИЯ v5.0
// ============================================================================
// Описание: Управляет сетевым интерфейсом (Ethernet/Wi-Fi) на WT32-ETH01.
//           Обеспечивает получение IP-адреса, контроль соединения и публикацию
//           событий о состоянии сети через новую событийную шину.
//
// АРХИТЕКТУРНЫЕ ПРИНЦИПЫ:
// 1. Полная изоляция: модуль НЕ знает о MQTT, Web или OTA.
// 2. Реактивность: модуль публикует события о состоянии сети.
// 3. Неблокируемость: все операции с сетью асинхронны, используют millis().
// 4. Конфигурируемость: все параметры (IP, маска, шлюз) загружаются из конфига.
// 5. Восстановление: автоматическое переподключение при обрыве связи.
// 6. Потокобезопасность: рекурсивный мьютекс для защиты данных.
// 7. Публикация событий через новую шину (v5.0).
//
// ИЗМЕНЕНИЯ v5.0:
// - Добавлена публикация событий через AppCore::publishEvent()
// - Добавлен счетчик _totalEventsPublished для диагностики
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#pragma once

#include "core/IModule.h"
#include "core/ShEventData.h" // НОВОЕ: для констант событий
#include "core/AppCore.h"     // НОВОЕ: для публикации событий
#include <ETH.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

// ============================================================================
// 1. СОСТОЯНИЯ СЕТЕВОГО ИНТЕРФЕЙСА (ДЛЯ КОНЕЧНОГО АВТОМАТА)
// ============================================================================

/**
 * @brief Состояния сетевого соединения
 */
enum class NetworkState : uint8_t {
    STATE_DISABLED = 0,       // Сеть отключена (выключена в конфиге)
    STATE_INITIALIZING = 1,   // Инициализация Ethernet
    STATE_WAITING_IP = 2,     // Ожидание IP-адреса (DHCP)
    STATE_CONNECTED = 3,      // Сеть подключена, IP получен
    STATE_DISCONNECTED = 4,   // Сеть была подключена, но потеряла связь
    STATE_ERROR = 5           // Критическая ошибка (нет PHY, сбой DHCP)
};

// ============================================================================
// 2. СТРУКТУРЫ ДАННЫХ
// ============================================================================

/**
 * @brief Статистика сетевого соединения
 */
struct NetworkStats {
    uint32_t uptime = 0;                // Время работы сети в секундах
    uint32_t reconnectAttempts = 0;     // Количество попыток переподключения
    uint32_t successReconnects = 0;     // Успешных переподключений
    uint32_t failedReconnects = 0;      // Неудачных переподключений
    uint32_t lastReconnectTime = 0;     // Время последней попытки
    uint32_t dhcpTimeoutCount = 0;      // Количество таймаутов DHCP
    uint32_t linkDownCount = 0;         // Количество обрывов линка
    uint32_t currentRetryCount = 0;     // Текущий счетчик попыток
    uint32_t maxRetryCount = 5;         // Максимальное количество попыток
    uint32_t packetsSent = 0;           // Отправлено пакетов (если доступно)
    uint32_t packetsReceived = 0;       // Получено пакетов (если доступно)
    uint32_t lastErrorCode = 0;         // Последний код ошибки
    char lastErrorMsg[64] = "";         // Последнее сообщение об ошибке
    bool isDhcpEnabled = true;          // Используется ли DHCP
    bool hasValidIP = false;            // Есть ли валидный IP
    char ipAddress[16] = "";            // Текущий IP-адрес
    char macAddress[18] = "";           // MAC-адрес
};

// ============================================================================
// 3. КЛАСС МЕНЕДЖЕРА СЕТИ
// ============================================================================

class MicroOSNetworkManager : public IModule {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(NetworkState state)> OnStateChangeCallback;
    typedef std::function<void(const char* ip, const char* mac)> OnConnectedCallback;
    typedef std::function<void(const char* reason)> OnDisconnectedCallback;
    typedef std::function<void(const NetworkStats& stats)> OnStatsUpdateCallback;

    // === СИНГЛТОН ===
    static NetworkManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    NetworkManager();
    ~NetworkManager();

    // Запрещаем копирование
    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "NetworkManager"; }
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_NETWORK; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _ready && _initialized; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === УПРАВЛЕНИЕ ===
    bool initEthernet();
    void forceReconnect();
    void setAutoReconnect(bool enable) { _autoReconnect = enable; }
    void setReconnectDelay(uint32_t ms) { _reconnectDelayMs = ms; }
    void setMaxReconnectAttempts(uint32_t attempts) { _maxReconnectAttempts = attempts; }

    // === ГЕТТЕРЫ ===
    NetworkState getState() const { return _state; }
    bool isConnected() const { return _state == NetworkState::STATE_CONNECTED; }
    IPAddress getIP() const { return ETH.localIP(); }
    IPAddress getGateway() const { return ETH.gatewayIP(); }
    IPAddress getSubnet() const { return ETH.subnetMask(); }
    IPAddress getDNS() const { return ETH.dnsIP(); }
    String getMAC() const { return ETH.macAddress(); }
    const char* getHostname() const { return _hostname; }
    uint32_t getUptime() const { return _stats.uptime; }
    const NetworkStats& getStats() const { return _stats; }

    // === КОНФИГУРАЦИЯ ===
    void setHostname(const char* name);
    void setUseDhcp(bool useDhcp);
    void setStaticIP(const char* ip, const char* gateway, const char* subnet, const char* dns);
    void setDhcpTimeout(uint32_t ms) { _dhcpTimeoutMs = ms; }
    void setCheckInterval(uint32_t ms) { _checkIntervalMs = ms; }

    // === КОЛБЭКИ ===
    void setOnStateChange(OnStateChangeCallback cb) { _onStateChange = cb; }
    void setOnConnected(OnConnectedCallback cb) { _onConnected = cb; }
    void setOnDisconnected(OnDisconnectedCallback cb) { _onDisconnected = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }

    // === НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0) ===
    void publishNetworkEvent(const char* eventType, const char* details, bool success);

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    bool initEthernetHardware();
    void checkNetworkState();
    bool hasValidIP() const;
    void updateStats();
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);

    // === НОВЫЙ МЕТОД: ВНУТРЕННЯЯ ПУБЛИКАЦИЯ СОБЫТИЙ ===
    void publishNetworkEventInternal(const char* eventType, const char* details, bool success);

    // === ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ) ===
    void publishState(NetworkState state);
    void publishError(const char* errorCode);
    void publishConnected(const char* ip, const char* mac);
    void publishDisconnected(const char* reason);

    // === ОБРАБОТЧИКИ СОБЫТИЙ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ДАННЫЕ ===
    uint8_t _id = 0x10;                     // ID модуля (Network)
    bool _initialized = false;
    bool _ready = false;

    // --- Состояние ---
    NetworkState _state = NetworkState::STATE_DISABLED;
    NetworkState _lastPublishedState = NetworkState::STATE_DISABLED;
    uint32_t _stateChangeTime = 0;
    uint32_t _connectionTime = 0;           // Время успешного подключения

    // --- Конфигурация ---
    bool _useDhcp = true;
    bool _autoReconnect = true;
    char _hostname[32] = "esp32-device";

    // --- Параметры для контроля соединения ---
    uint32_t _checkIntervalMs = 5000;
    uint32_t _dhcpTimeoutMs = 30000;
    uint32_t _reconnectDelayMs = 10000;
    uint32_t _maxReconnectAttempts = 5;

    // --- Состояние соединения ---
    uint32_t _lastCheckTime = 0;
    uint32_t _reconnectAttempts = 0;
    uint32_t _lastReconnectAttempt = 0;

    // --- Статистика ---
    NetworkStats _stats;
    uint32_t _startTime = 0;

    // --- НОВОЕ: счетчик опубликованных событий ---
    uint32_t _totalEventsPublished = 0;

    // --- Мьютекс ---
    SemaphoreHandle_t _mutex = nullptr;

    // --- Колбэки ---
    OnStateChangeCallback _onStateChange = nullptr;
    OnConnectedCallback _onConnected = nullptr;
    OnDisconnectedCallback _onDisconnected = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;

    // --- Константы ---
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
};