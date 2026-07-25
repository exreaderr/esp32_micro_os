// ============================================================================
// OTAManager.h - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление обновлениями прошивки через ОТА (HTTP, Web).
// Все события обновления публикуются в шину событий.
//
// ИЗМЕНЕНИЯ v4.2.1:
// - Удалена глобальная переменная OTA
// - Добавлен синглтон
// - Рекурсивный мьютекс вместо обычного
// - Добавлены недостающие структуры
// - Исправлена генерация JSON
// - Добавлена проверка свободного места
// - Добавлен Rollback механизм
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.1
// - Добавлен метод publishOtaEvent() для публикации через новую шину
// - Добавлен счетчик _totalEventsPublished для диагностики
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>

#include "core/IModule.h"
#include "core/ShEventData.h" // НОВОЕ: для констант событий

// ============================================================================
// 1. СОБЫТИЯ ОТА MANAGER (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
enum OtaEvents : int32_t {
    SH_EVENT_OTA_START = SH_EVENT_USER_BASE + 0x0700,
    SH_EVENT_OTA_PROGRESS = SH_EVENT_USER_BASE + 0x0701,
    SH_EVENT_OTA_SUCCESS = SH_EVENT_USER_BASE + 0x0702,
    SH_EVENT_OTA_FAILED = SH_EVENT_USER_BASE + 0x0703,
    SH_EVENT_OTA_CANCELLED = SH_EVENT_USER_BASE + 0x0704,
    SH_EVENT_OTA_UPDATE_FOUND = SH_EVENT_USER_BASE + 0x0705,
    SH_EVENT_OTA_WEB_START = SH_EVENT_USER_BASE + 0x0706,
    SH_EVENT_OTA_WEB_PROGRESS = SH_EVENT_USER_BASE + 0x0707,
    SH_EVENT_OTA_WEB_COMPLETE = SH_EVENT_USER_BASE + 0x0708,
    SH_EVENT_OTA_ERROR = SH_EVENT_USER_BASE + 0x0709,
    SH_EVENT_OTA_ROLLBACK = SH_EVENT_USER_BASE + 0x070A,
    SH_EVENT_OTA_ROLLBACK_SUCCESS = SH_EVENT_USER_BASE + 0x070B,
    SH_EVENT_OTA_ROLLBACK_FAILED = SH_EVENT_USER_BASE + 0x070C
};

// ============================================================================
// 2. СТРУКТУРЫ ДАННЫХ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
enum class OTAStatus : uint8_t {
    IDLE = 0,
    CHECKING = 1,
    DOWNLOADING_FW = 2,
    DOWNLOADING_FS = 3,
    VERIFYING = 4,
    SUCCESS = 5,
    FAILED = 6,
    UPLOADING = 7,
    CANCELLED = 8,
    REBOOTING = 9,
    ROLLBACK = 10,
    ROLLBACK_SUCCESS = 11,
    ROLLBACK_FAILED = 12
};

struct UpdateInfo {
    char version[32] = "";
    char fw_md5[33] = "";
    char fs_md5[33] = "";
    char changelog[256] = "";
    char fw_url[128] = "";
    char fs_url[128] = "";
    uint32_t size = 0;
    uint32_t fw_size = 0;
    uint32_t fs_size = 0;
    bool available = false;
    bool forceUpdate = false;
    uint32_t minRam = 0;
};

struct OTAStats {
    uint32_t totalChecks = 0;
    uint32_t updatesFound = 0;
    uint32_t updatesSuccess = 0;
    uint32_t updatesFailed = 0;
    uint32_t webUploads = 0;
    uint32_t lastCheckTime = 0;
    uint32_t lastUpdateTime = 0;
    uint32_t lastUpdateDuration = 0;
    char lastVersion[32] = "";
    char lastError[128] = "";
    float avgDownloadSpeed = 0.0f;
    uint32_t totalDownloaded = 0;
    uint32_t rollbackCount = 0;
    uint32_t rollbackSuccess = 0;
};

struct UpdateHistory {
    char version[32];
    uint32_t timestamp;
    bool success;
    uint32_t duration;
    char error[128];
};

struct OtaConfigParams {
    char serverIp[16] = "";
    char hostname[32] = "smart-device";
    bool checkOnBoot = true;
    bool autoCheck = true;
    bool rebootAfterUpdate = true;
    bool verifyMD5 = true;
    bool enableRollback = true;
    uint32_t checkIntervalMs = 86400000;
    uint32_t timeoutMs = 30000;
    uint32_t maxRetries = 3;
    uint32_t retryDelayMs = 5000;
    uint16_t port = 8123;
};

// ============================================================================
// 3. ОСНОВНОЙ КЛАСС (РАСШИРЕН)
// ============================================================================
class OTAManager : public IModule {
public:
    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    typedef std::function<void(OTAStatus status, int progress, const char* message)> OnOTAProgressCallback;
    typedef std::function<void(bool success, const char* version)> OnOTACompleteCallback;
    typedef std::function<void(const UpdateInfo& info)> OnUpdateFoundCallback;
    typedef std::function<void(uint32_t downloaded, uint32_t total)> OnDownloadProgressCallback;
    typedef std::function<void(const OTAStats& stats)> OnStatsUpdateCallback;

    // === СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    static OTAManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    OTAManager();
    ~OTAManager();

    OTAManager(const OTAManager&) = delete;
    OTAManager& operator=(const OTAManager&) = delete;

    // === IModule (ВАШ, ДОПОЛНЕН) ===
    const char* getName() const override { return "OTAManager"; }
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_OTA; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized && _status == OTAStatus::IDLE; }

    // === СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ЖИЗНЕННЫЙ ЦИКЛ (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    void begin(const OtaConfigParams& params, const char* customBaseUrl = "");
    void end();
    void reset();

    // === ПРОВЕРКА ОБНОВЛЕНИЙ (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    bool checkForUpdates(UpdateInfo& info);
    bool isUpdateAvailable();

    // === ВЫПОЛНЕНИЕ ОТА (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    bool performOTA(const UpdateInfo& info);
    bool performOTA();
    void cancelOTA();
    bool rollback();

    // === WEB-ОТА (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    bool beginWebOTA(uint32_t size, int command = U_FLASH);
    bool writeWebOTA(const uint8_t* data, size_t len);
    bool endWebOTA(bool success = true);

    // === ГЕТТЕРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    bool isWebOTAInProgress() const { return _webOTAInProgress; }
    bool isOtaInProgress() const { return _otaInProgress; }
    size_t getWebOTAProgress() const { return _webOTAProgress; }
    size_t getWebOTATotal() const { return _webOTATotal; }
    OTAStatus getOtaStatus() const { return _status; }
    const char* getStatusString() const;
    int getProgress() const { return _progress; }
    const char* getLastError() const { return _lastError; }
    const char* getCurrentVersion() const { return _currentVersion; }
    OTAStats getOtaStats() const { return _stats; }
    std::vector<UpdateHistory> getHistory(size_t count = 10) const;

    // === НАСТРОЙКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setBaseUrl(const char* url);
    void setCurrentVersion(const char* version);
    void setVerifyMD5(bool enable) { _verifyMD5 = enable; }
    void setTimeout(uint32_t ms) { _timeoutMs = ms; }
    void setMaxRetries(uint32_t count) { _maxRetries = count; }

    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setProgressCallback(OnOTAProgressCallback cb) { _onProgress = cb; }
    void setCompleteCallback(OnOTACompleteCallback cb) { _onComplete = cb; }
    void setOnUpdateFound(OnUpdateFoundCallback cb) { _onUpdateFound = cb; }
    void setOnDownloadProgress(OnDownloadProgressCallback cb) { _onDownloadProgress = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }

    // === НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0) ===
    void publishOtaEvent(const char* eventType, const char* details, bool success);

    // === ДИАГНОСТИКА (ВАША, РАСШИРЕНА) ===
    void streamDiagnosticInfo(Stream& stream) const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void checkAutoUpdate();
    void updateStatus(OTAStatus status, int progress = 0, const char* msg = "");
    void updateStatsRecord(bool success, const char* version);
    void saveVersion(const char* version);
    void addHistory(const UpdateHistory& entry);
    void saveHistory();
    void loadHistory();
    bool downloadWithProgress(const String& url, const String& md5, uint32_t& downloaded);
    bool downloadManifest(JsonDocument& doc);
    bool verifyFirmware(const String& md5);
    uint32_t getFreeSketchSpace() const;
    bool hasEnoughSpace(const UpdateInfo& info);

    // === ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ) ===
    void publishProgressEvent(int progress, const char* message);
    void publishCompleteEvent(bool success, const char* version, const char* error = nullptr);
    void publishUpdateFoundEvent(const UpdateInfo& info);
    void publishErrorEvent(const char* errorCode);
    void publishWebProgressEvent(uint32_t downloaded, uint32_t total);
    void publishRollbackEvent(bool success);

    // === НОВЫЙ МЕТОД: ВНУТРЕННЯЯ ПУБЛИКАЦИЯ СОБЫТИЙ ===
    void publishOtaEventInternal(const char* eventType, const char* details, bool success);

    // === ЛОГИРОВАНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);

    // === ОБРАБОТЧИКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            uint32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ДАННЫЕ (ВАШИ, РАСШИРЕНЫ) ===
    OtaConfigParams _params;
    String _baseUrl;
    char _currentVersion[32] = "4.0.0";
    uint32_t _moduleId = MODULE_ID_OTA;
    bool _verifyMD5 = true;
    uint32_t _lastCheckMs = 0;
    uint32_t _timeoutMs = 30000;
    uint32_t _maxRetries = 3;
    uint32_t _retryCount = 0;

    // НОВОЕ: счетчик опубликованных событий
    uint32_t _totalEventsPublished = 0;

    // Состояние
    SemaphoreHandle_t _otaMutex = nullptr;
    volatile OTAStatus _status = OTAStatus::IDLE;
    volatile int _progress = 0;
    volatile bool _webOTAInProgress = false;
    volatile bool _otaInProgress = false;
    bool _initialized = false;
    bool _autoCheckEnabled = true;
    uint32_t _webOTAProgress = 0;
    uint32_t _webOTATotal = 0;
    uint32_t _otaStartTime = 0;
    char _lastError[128] = "";

    // Версия для отката
    char _rollbackVersion[32] = "";
    bool _rollbackAvailable = false;

    // Статистика
    OTAStats _stats;
    std::vector<UpdateHistory> _history;

    // Колбэки
    OnOTAProgressCallback _onProgress = nullptr;
    OnOTACompleteCallback _onComplete = nullptr;
    OnUpdateFoundCallback _onUpdateFound = nullptr;
    OnDownloadProgressCallback _onDownloadProgress = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;

    // Константы
    static constexpr size_t MAX_HISTORY = 10;
    static constexpr uint32_t OTA_CHUNK_SIZE = 1024;
    static constexpr const char* VERSION_FILE = "/version.json";
    static constexpr const char* HISTORY_FILE = "/ota_history.json";
    static constexpr const char* ROLLBACK_FILE = "/rollback.bin";
    static constexpr uint32_t MIN_FREE_SPACE = 262144;
};