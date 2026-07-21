// ============================================================================
// ConfigManager.h - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление конфигурацией через события.
// Все изменения конфигурации публикуются в шину событий.
// Другие модули подписываются на изменения и реагируют автоматически.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - Исправлены критические ошибки (updateStatus, applyNetworkSettings)
// - Добавлена защита от гонок данных в calculateConfigCRC
// - Исправлено логирование (используется правильное событие)
// - Добавлены методы для работы с конфигурацией без String (снижение фрагментации)
// - Добавлена проверка _initialized во всех публичных методах
// - Добавлена защита от рекурсивных вызовов applyNetworkSettings
// - Улучшена документация всех методов
// ============================================================================
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/IModule.h"

// ============================================================================
// 1. КОНСТАНТЫ
// ============================================================================
#define CONFIG_VERSION_CURRENT 2
#define CONFIG_BACKUP_NAMESPACE "syscfg"

// События конфигурации
#define CONFIG_EVENT_CHANGED 0x1000
#define CONFIG_EVENT_LOADED  0x1001
#define CONFIG_EVENT_SAVED   0x1002
#define CONFIG_EVENT_RESET   0x1003
#define CONFIG_EVENT_CORRUPTED 0x2001
#define CONFIG_EVENT_NETWORK_APPLY 0x2000

// ============================================================================
// 2. СТРУКТУРА КОНФИГУРАЦИИ (БЕЗ String!)
// ============================================================================
/**
 * @brief Структура системной конфигурации
 *
 * Все строки хранятся в фиксированных буферах для
 * предотвращения фрагментации кучи.
 */
struct SystemConfig {
    // Версия
    uint8_t config_ver = CONFIG_VERSION_CURRENT;

    // Network (фиксированные буферы)
    bool net_dhcp = true;
    char cfg_ip[16] = "192.168.1.200";
    char cfg_mask[16] = "255.255.255.0";
    char cfg_gateway[16] = "192.168.1.1";
    char cfg_dns[16] = "192.168.1.1";

    // MQTT
    bool use_mqtt = false;
    char mqtt_ip[16] = "";
    char mqtt_user[32] = "";
    char mqtt_password[64] = "";

    // System
    char hostname[32] = "smart-device";
    char web_password_hash[65] = "";  // SHA256 = 64 байта + '\0'
    bool is_pure_local_mode = false;

    // Расширяемая секция (хранится как JSON строка)
    char extra_data[512] = "{}";

    // Дополнительные поля
    uint32_t version_flags = 0;      // Битовые флаги версии
    uint32_t last_modified = 0;      // Unix время последнего изменения
    uint32_t config_crc = 0;         // CRC всей конфигурации
};

// ============================================================================
// 3. СТРУКТУРЫ ДАННЫХ ДЛЯ СОБЫТИЙ
// ============================================================================
/**
 * @brief Событие изменения конфигурации
 */
struct ConfigChangeEvent {
    uint32_t timestamp;
    char key[64];
    char oldValue[128];
    char newValue[128];
    bool isCritical;
};

/**
 * @brief Статус конфигуратора
 */
struct ConfigStatus {
    bool isLoaded = false;
    bool isDirty = false;
    bool isSafeMode = false;
    bool isFactoryReset = false;
    bool isNetworkApplied = false;
    uint32_t crc = 0;
    uint32_t lastSaveTime = 0;
    uint32_t lastLoadTime = 0;
    size_t fileSize = 0;
    uint8_t currentVersion = CONFIG_VERSION_CURRENT;
    char errorMessage[128] = "";
    uint32_t loadCount = 0;
    uint32_t saveCount = 0;
    uint32_t lastModified = 0;
};

// ============================================================================
// 4. ОСНОВНОЙ КЛАСС
// ============================================================================
/**
 * @brief Менеджер конфигурации
 *
 * Синглтон. Обеспечивает:
 * - Загрузку/сохранение конфигурации в LittleFS
 * - NVS бэкап
 * - Атомарную запись с ротацией
 * - Публикацию изменений через события
 * - SafeMode при критических ошибках
 * - Миграцию версий
 * - Валидацию данных
 */
class ConfigManager : public IModule {
public:
    // === ТИПЫ ДЛЯ РАСШИРЕНИЙ ===
    typedef std::function<void(JsonVariant extra)> OnLoadExtraCallback;
    typedef std::function<void(JsonVariant extra)> OnSaveExtraCallback;
    typedef std::function<void(const char* key, const char* oldVal, const char* newVal)> OnConfigChangeCallback;
    typedef std::function<void(bool isSafeMode)> OnSafeModeChangeCallback;

    // === СИНГЛТОН ===
    static ConfigManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    ConfigManager();
    ~ConfigManager();

    // Запрещаем копирование
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "ConfigManager"; }
    const char* getVersion() const override { return "4.2.2"; }
    uint32_t getModuleId() const override { return MODULE_ID_CONFIG; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _loaded && _initialized; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;
    void setSafeMode(bool active);
    String getDiagnosticsString() const;

    // === ПУБЛИЧНЫЕ МЕТОДЫ ===
    bool load();
    bool save();
    void forceSave() { save(); }
    void scheduleSave() { markDirty(); }
    void resetToDefaults();
    bool reload() { return load(); }

    // === ИМПОРТ / ЭКСПОРТ ===
    bool exportToJson(String& output) const;
    bool exportToJson(char* buffer, size_t bufferSize) const;
    String exportToJson() const;
    bool importFromJson(const String& json);
    bool saveFromJsonStream(Stream& stream);
    void serializeToStream(Stream& stream);

    // === ГЕТТЕРЫ (возвращают const char*) ===
    const SystemConfig& getConfig() const { return _sys; }
    const char* getHostname() const { return _sys.hostname; }
    const char* getMqttIp() const { return _sys.mqtt_ip; }
    const char* getMqttUser() const { return _sys.mqtt_user; }
    const char* getMqttPassword() const { return _sys.mqtt_password; }
    bool getUseMqtt() const { return _sys.use_mqtt; }
    const char* getWebPasswordHash() const { return _sys.web_password_hash; }
    bool getPureLocalMode() const { return _sys.is_pure_local_mode; }
    bool getNetworkDhcp() const { return _sys.net_dhcp; }
    const char* getStaticIp() const { return _sys.cfg_ip; }
    const char* getStaticMask() const { return _sys.cfg_mask; }
    const char* getStaticGateway() const { return _sys.cfg_gateway; }
    const char* getStaticDns() const { return _sys.cfg_dns; }
    const char* getExtraData() const { return _sys.extra_data; }
    uint32_t getConfigCRC() const { return _lastSavedCRC; }

    // === СТАТУС КОНФИГУРАЦИИ ===
    ConfigStatus getConfigStatus() const;
    bool getConfigStatusString(char* buffer, size_t bufferSize) const;
    String getConfigStatusString() const;
    bool isDirty() const { return _isDirty; }
    bool isLoaded() const { return _loaded; }
    bool isSafeMode() const { return _safeModeActive; }

    // === СЕТТЕРЫ (с отправкой событий) ===
    void setHostname(const char* name);
    void setHostname(const String& name);
    void setMqttIp(const char* ip);
    void setMqttIp(const String& ip);
    void setMqttUser(const char* user);
    void setMqttUser(const String& user);
    void setMqttPassword(const char* password);
    void setMqttPassword(const String& password);
    void setUseMqtt(bool use);
    void setWebPasswordHash(const char* hash);
    void setWebPasswordHash(const String& hash);
    void setPureLocalMode(bool local);
    void setNetworkDhcp(bool dhcp);
    void setStaticIp(const char* ip, const char* mask, const char* gateway, const char* dns);
    void setStaticIp(const String& ip, const String& mask, const String& gateway, const String& dns);
    void setStaticIp(const char* ip);
    void setStaticIp(const String& ip);
    void setExtraData(const char* json);
    void setExtraData(const String& json);

    // === РЕГИСТРАЦИЯ КОЛБЭКОВ ===
    void setExtraCallbacks(OnLoadExtraCallback loadcb, OnSaveExtraCallback savecb);
    void setConfigChange(OnConfigChangeCallback cb) { _onConfigChange = cb; }
    void setSafeModeCallback(OnSafeModeChangeCallback cb) { _onSafeModeChange = cb; }

    // === СЕТЕВЫЕ НАСТРОЙКИ ===
    void applyNetworkSettings();
    void refreshNetwork();

    // === ВАЛИДАЦИЯ ===
    bool isValidIp(const char* ip) const;
    bool isValidIp(const String& ip) const;
    bool isValidHostname(const char* hostname) const;
    bool isConfigValid() const;

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    bool loadFromFile();
    bool saveToFile();
    void loadFromNvsBackup();
    void saveToNvsBackup();
    bool validateSystemConfig(const JsonDocument& doc);
    void migrateConfig(JsonDocument& doc, uint8_t oldVersion);
    void markDirty();
    uint32_t calculateConfigCRC();
    void updateStatus(const char* error = nullptr);
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    bool isFileExists(const char* path) const;
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    bool ensureLittleFS();

    // === ОБРАБОТЧИКИ СОБЫТИЙ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ОТПРАВКА СОБЫТИЙ ===
    void publishConfigChange(const char* key, const char* oldValue,
                            const char* newValue, bool critical = false);
    void publishConfigLoaded();
    void publishConfigSaved();
    void publishConfigReset();
    void publishConfigCorrupted(uint32_t crc);

    // === ДАННЫЕ ===
    SystemConfig _sys;
    Preferences _prefs;
    SemaphoreHandle_t _mutex = nullptr;  // Рекурсивный мьютекс

    bool _safeModeActive = false;
    bool _factoryResetTriggered = false;
    bool _isDirty = false;
    bool _networkApplied = false;
    bool _loaded = false;
    bool _initialized = false;
    bool _applyNetworkInProgress = false;  // Защита от рекурсии

    esp_event_handler_instance_t _sysHandlerInstance = nullptr;
    esp_event_handler_instance_t _appHandlerInstance = nullptr;

    uint32_t _dirtyTimestamp = 0;
    uint32_t _lastSavedCRC = 0;
    uint32_t _lastLoadTime = 0;
    uint32_t _lastSaveTime = 0;
    uint32_t _loadCount = 0;
    uint32_t _saveCount = 0;

    ConfigStatus _status;
    char _lastError[128] = "";

    // === КОЛБЭКИ ===
    OnLoadExtraCallback _onLoadExtra = nullptr;
    OnSaveExtraCallback _onSaveExtra = nullptr;
    OnConfigChangeCallback _onConfigChange = nullptr;
    OnSafeModeChangeCallback _onSafeModeChange = nullptr;

    // === КОНСТАНТЫ ===
    static constexpr uint32_t FLUSH_DELAY_MS = 10000;
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 2000;
    static constexpr uint32_t MAX_JSON_SIZE = 4096;
    static constexpr size_t MAX_HOSTNAME_LEN = 63;
    static constexpr uint32_t CRC_MUTEX_TIMEOUT_MS = 500;

    // === ПУТИ ===
    static constexpr const char* CONFIG_PATH = "/config.json";
    static constexpr const char* BACKUP_PATH = "/config.bak";
    static constexpr const char* TEMP_PATH = "/config.tmp";
};
