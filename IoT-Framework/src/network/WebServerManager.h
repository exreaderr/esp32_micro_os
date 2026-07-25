// ============================================================================
// WebServerManager.h - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление веб-сервером, API и статическими файлами.
// Все события API-запросов и аутентификации публикуются в шину событий.
//
// ИЗМЕНЕНИЯ v4.2.1:
// - Удалена глобальная переменная WebServerCore
// - Добавлен синглтон
// - Рекурсивный мьютекс вместо обычного
// - Исправлены опечатки (_status → _stats, _maxApiDocsSlots)
// - Добавлена CSRF-защита
// - Добавлено кэширование статики
// - Улучшена обработка загрузок
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.1
// - Добавлен метод publishWebEvent() для публикации через новую шину
// - Добавлен счетчик _totalEventsPublished для диагностики
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <functional>
#include <vector>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>

#include "core/IModule.h"
#include "core/ShEventData.h" // НОВОЕ: для констант событий

// ============================================================================
// 1. СОБЫТИЯ WEB SERVER MANAGER (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
enum WebEvents : int32_t {
    SH_EVENT_WEB_REQUEST = SH_EVENT_USER_BASE + 0x0800,
    SH_EVENT_WEB_AUTH_SUCCESS = SH_EVENT_USER_BASE + 0x0801,
    SH_EVENT_WEB_AUTH_FAILED = SH_EVENT_USER_BASE + 0x0802,
    SH_EVENT_WEB_API_CALL = SH_EVENT_USER_BASE + 0x0803,
    SH_EVENT_WEB_UPLOAD_START = SH_EVENT_USER_BASE + 0x0804,
    SH_EVENT_WEB_UPLOAD_PROGRESS = SH_EVENT_USER_BASE + 0x0805,
    SH_EVENT_WEB_UPLOAD_COMPLETE = SH_EVENT_USER_BASE + 0x0806,
    SH_EVENT_WEB_ERROR = SH_EVENT_USER_BASE + 0x0807,
    SH_EVENT_WEB_SESSION_CREATED = SH_EVENT_USER_BASE + 0x0808,
    SH_EVENT_WEB_SESSION_DESTROYED = SH_EVENT_USER_BASE + 0x0809,
    SH_EVENT_WEB_STATIC_SERVED = SH_EVENT_USER_BASE + 0x080A,
    SH_EVENT_WEB_CSRF_VERIFIED = SH_EVENT_USER_BASE + 0x080B,
    SH_EVENT_WEB_CSRF_FAILED = SH_EVENT_USER_BASE + 0x080C
};

// ============================================================================
// 2. СТРУКТУРЫ ДАННЫХ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
struct WebRequestEvent {
    char uri[64];
    char method[8];
    uint32_t clientIp;
    uint32_t timestamp;
    bool authenticated;
    bool isApi;
    uint16_t responseCode;
};

struct WebAuthEvent {
    char username[32];
    char role[16];
    char token[64];
    bool success;
    uint32_t timestamp;
    char clientIp[16];
};

struct WebStats {
    uint32_t totalRequests = 0;
    uint32_t authenticatedRequests = 0;
    uint32_t failedAuthAttempts = 0;
    uint32_t apiCalls = 0;
    uint32_t staticRequests = 0;
    uint32_t uploads = 0;
    uint32_t errors = 0;
    uint32_t startTime = 0;
    uint32_t lastRequestTime = 0;
    char lastEndpoint[32] = "none";
    uint32_t activeSessions = 0;
    uint32_t csrfErrors = 0;
    uint32_t staticCacheHits = 0;
    uint32_t staticCacheMisses = 0;
};

struct WebServerParams {
    char adminPasswordHash[65] = "";
    char hostname[32] = "smart-device";
    bool corsEnabled = false;
    char corsOrigin[64] = "";
    bool ddosProtection = true;
    uint8_t ddosLimit = 5;
    uint32_t sessionTimeoutMs = 900000;
    uint32_t uploadTimeoutMs = 30000;
    uint32_t maxUploadSize = 1048576;
    bool enableCache = true;
    bool enableGzip = true;
    bool enableCsrf = true;
    bool forceHttps = false;
    uint16_t port = 80;
};

struct SessionInfo {
    char token[64];
    char role[16];
    char username[32];
    char cardId[9];
    uint32_t lastActivityMs;
    uint32_t createdAt;
    bool valid;
};

// ============================================================================
// 3. ОСНОВНОЙ КЛАСС (РАСШИРЕН)
// ============================================================================
class WebServerManager : public IModule {
public:
    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    typedef std::function<void(JsonVariant req, JsonObject resp)> JsonApiHandler;
    typedef std::function<void(WebServer& server)> OnRegisterEndpointsCallback;
    typedef std::function<void(JsonObject& doc, bool isAdmin)> OnAppendStatusCallback;
    typedef std::function<void(HTTPUpload& upload)> OnUploadCallback;
    typedef std::function<void(const char* uri, const char* method)> OnRequestCallback;
    typedef std::function<void(const WebStats& stats)> OnStatusUpdateCallback;

    // === СИНГЛТОН (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    static WebServerManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    WebServerManager();
    ~WebServerManager();

    WebServerManager(const WebServerManager&) = delete;
    WebServerManager& operator=(const WebServerManager&) = delete;

    // === IModule (ВАШ, ДОПОЛНЕН) ===
    const char* getName() const override { return "WebServerManager"; }
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_WEB; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized && _serverStarted; }

    // === СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ЖИЗНЕННЫЙ ЦИКЛ (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    void begin(uint16_t port, const WebServerParams& params);
    void end();
    void reset();

    // === РЕГИСТРАЦИЯ API (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    void registerApi(const char* uri, HTTPMethod method, JsonApiHandler handler,
                    bool protectedEndpoint = true, const char* description = "");
    void registerUpload(const char* uri, HTTPMethod method, OnUploadCallback uploadcb,
                       std::function<void()> onComplete = nullptr,
                       bool protectedEndpoint = true);

    // === УПРАВЛЕНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    bool isAuthenticated() const;
    bool login(const char* passwordHash);
    bool loginCard(const char* cardId);
    void logout();
    String generateCsrfToken();

    // === ГЕТТЕРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    bool isServerRunning() const { return _serverStarted; }
    uint16_t getPort() const { return _port; }
    const WebStats& getStats() const { return _stats; }
    const char* getCurrentUser() const { return _currentSessionUsername.c_str(); }
    const char* getCurrentRole() const { return _currentSessionRole.c_str(); }
    const char* getSessionToken() const { return _currentSessionToken.c_str(); }

    // === НАСТРОЙКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setOnRegisterEndpoints(OnRegisterEndpointsCallback cb) { _onRegisterEndpoints = cb; }
    void setOnAppendStatus(OnAppendStatusCallback cb) { _onAppendStatus = cb; }
    void setOnRequest(OnRequestCallback cb) { _onRequest = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }
    void setVerboseLogging(bool enable) { _verboseLogging = enable; }

    // === НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0) ===
    void publishWebEvent(const char* eventType, const char* details, bool success);

    // === ДИАГНОСТИКА (ВАША, РАСШИРЕНА) ===
    void streamDiagnosticInfo(Stream& stream) const;
    void serializeStats(Stream& stream) const;
    void serializeApiDocs(Stream& stream) const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void initSystemEndpoints();
    bool handleFileRead(const String& path);
    void updateStats(const char* endpoint, bool authenticated, bool isApi = true);
    void addCorsHeaders();
    void addSecurityHeaders();
    void addCacheHeaders(const String& path);
    bool checkDdosProtection();
    bool checkAccess(bool adminRequired = false);
    bool verifyCsrfToken(const char* token);
    void checkSessionTimeout();
    void cleanupSessions();
    void clearSession();
    String getClientIp() const;
    bool safeStringCompare(const String& a, const String& b);
    void generateSecureToken(char* dest, size_t size);
    const char* getContentType(const String& path) const;
    static const char* methodToString(HTTPMethod method);

    // === НОВЫЙ МЕТОД: ВНУТРЕННЯЯ ПУБЛИКАЦИЯ СОБЫТИЙ ===
    void publishWebEventInternal(const char* eventType, const char* details, bool success);

    // === ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ) ===
    void publishRequestEvent(const char* uri, const char* method, bool authenticated, bool isApi);
    void publishAuthEvent(const char* username, const char* role, bool success);
    void publishUploadEvent(const char* uri, bool start, uint32_t progress = 0, uint32_t total = 0);
    void publishErrorEvent(const char* errorCode);
    void publishSessionEvent(bool created);
    void publishCsrfEvent(bool success);

    // === ЛОГИРОВАНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);

    // === ОБРАБОТЧИКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === СТРУКТУРА API ДОКУМЕНТАЦИИ (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    struct ApiDoc {
        char uri[64];
        char method[8];
        char description[128];
        bool protectedEndpoint;
    };

    // === ДАННЫЕ (ВАШИ, РАСШИРЕНЫ) ===
    WebServer _server;
    WebServerParams _params;
    WebStats _stats;
    uint32_t _moduleId = MODULE_ID_WEB;
    uint16_t _port = 80;
    bool _initialized = false;
    bool _serverStarted = false;
    bool _verboseLogging = false;

    // НОВОЕ: счетчик опубликованных событий
    uint32_t _totalEventsPublished = 0;

    // Сессии
    String _currentSessionToken = "";
    String _currentSessionRole = "";
    String _currentSessionUsername = "";
    String _currentSessionCardId = "";
    uint32_t _lastSessionActivityMs = 0;
    uint32_t _sessionId = 0;
    std::map<String, SessionInfo> _sessions;
    uint32_t _lastSessionCleanup = 0;

    // CSRF
    String _csrfToken = "";
    uint32_t _csrfTokenCreated = 0;
    static constexpr uint32_t CSRF_TOKEN_TTL = 3600000;

    // DDoS
    uint32_t _lastClientIp = 0;
    uint32_t _lastRequestMs = 0;
    uint8_t _requestCounter = 0;

    // Статическое кэширование
    struct CacheEntry {
        String data;
        String contentType;
        uint32_t timestamp;
        bool valid;
    };
    std::map<String, CacheEntry> _staticCache;
    size_t _maxCacheSize = 20;
    uint32_t _cacheTtlMs = 60000;

    // Колбэки
    OnRegisterEndpointsCallback _onRegisterEndpoints = nullptr;
    OnAppendStatusCallback _onAppendStatus = nullptr;
    OnRequestCallback _onRequest = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;

    // API документация
    std::vector<ApiDoc> _apiDocs;
    size_t _maxApiDocSlots = 32;

    // Мьютекс
    SemaphoreHandle_t _webMutex = nullptr;

    // Константы
    static constexpr const char* _authHeaderToken = "Authorization";
    static constexpr const char* _csrfHeader = "X-CSRF-Token";
    static constexpr const char* _sessionCookie = "SESSION";
};