// ============================================================================
// WebServerManager.h - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление веб-сервером, API и статическими файлами.
// Все события API-запросов и аутентификации публикуются в шину событий.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНЫ синтаксические ошибки в handleCommand
// - ИСПРАВЛЕНА ошибка в login
// - ИСПРАВЛЕНА ошибка в getContentType
// - ДОБАВЛЕН метод logout
// - ДОБАВЛЕНА полная потокобезопасность
// - ДОБАВЛЕНА обработка всех системных событий
// - УЛУЧШЕНА работа с сессиями
// - ДОБАВЛЕНА защита от CSRF
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
#include <cstring>

// Только IModule, без AppCore!
#include "core/IModule.h"

// ============================================================================
// 1. КОНСТАНТЫ
// ============================================================================
#define WEB_SERVER_PORT 80
#define WEB_SESSION_TIMEOUT_MS 900000  // 15 минут
#define WEB_UPLOAD_TIMEOUT_MS 30000    // 30 секунд
#define WEB_MAX_UPLOAD_SIZE 1048576    // 1 MB
#define WEB_CSRF_TOKEN_TTL 3600000     // 1 час
#define WEB_MAX_API_DOCS 32
#define WEB_MAX_CACHE_SIZE 20
#define WEB_CACHE_TTL_MS 60000         // 1 минута
#define WEB_DDOS_LIMIT 5
#define WEB_DDOS_WINDOW_MS 10000       // 10 секунд

// ============================================================================
// 2. СОБЫТИЯ WEB SERVER MANAGER
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
// 3. СТРУКТУРЫ ДАННЫХ
// ============================================================================
/**
 * @brief Структура события веб-запроса
 */
struct WebRequestEvent {
    char uri[64];
    char method[8];
    uint32_t clientIp;
    uint32_t timestamp;
    bool authenticated;
    bool isApi;
    uint16_t responseCode;
};

/**
 * @brief Структура события аутентификации
 */
struct WebAuthEvent {
    char username[32];
    char role[16];
    char token[64];
    bool success;
    uint32_t timestamp;
    char clientIp[16];
};

/**
 * @brief Статистика веб-сервера
 */
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
    uint32_t ddosBlocks = 0;
};

/**
 * @brief Параметры веб-сервера
 */
struct WebServerParams {
    char adminPasswordHash[65] = "";
    char hostname[32] = "smart-device";
    bool corsEnabled = false;
    char corsOrigin[64] = "*";
    bool ddosProtection = true;
    uint8_t ddosLimit = WEB_DDOS_LIMIT;
    uint32_t sessionTimeoutMs = WEB_SESSION_TIMEOUT_MS;
    uint32_t uploadTimeoutMs = WEB_UPLOAD_TIMEOUT_MS;
    uint32_t maxUploadSize = WEB_MAX_UPLOAD_SIZE;
    bool enableCache = true;
    bool enableGzip = true;
    bool enableCsrf = true;
    bool forceHttps = false;
    uint16_t port = WEB_SERVER_PORT;
};

/**
 * @brief Информация о сессии
 */
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
// 4. ОСНОВНОЙ КЛАСС
// ============================================================================
/**
 * @brief Менеджер веб-сервера
 *
 * Синглтон. Обеспечивает:
 * - Веб-сервер с REST API
 * - Аутентификацию через токены
 * - CSRF защиту
 * - Статические файлы с кэшированием
 * - DDoS защиту
 * - Home Assistant готовность
 * - Полную потокобезопасность
 */
class WebServerManager : public IModule {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(JsonVariant req, JsonObject resp)> JsonApiHandler;
    typedef std::function<void(WebServer& server)> OnRegisterEndpointsCallback;
    typedef std::function<void(JsonObject& doc, bool isAdmin)> OnAppendStatusCallback;
    typedef std::function<void(HTTPUpload& upload)> OnUploadCallback;
    typedef std::function<void(const char* uri, const char* method)> OnRequestCallback;
    typedef std::function<void(const WebStats& stats)> OnStatsUpdateCallback;

    // === СИНГЛТОН ===
    static WebServerManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    WebServerManager();
    ~WebServerManager();

    // Запрещаем копирование
    WebServerManager(const WebServerManager&) = delete;
    WebServerManager& operator=(const WebServerManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "WebServerManager"; }
    const char* getVersion() const override { return "4.2.2"; }
    uint32_t getModuleId() const override { return MODULE_ID_WEB; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized && _serverStarted; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ЖИЗНЕННЫЙ ЦИКЛ ===
    void begin(uint16_t port, const WebServerParams& params);
    void end();
    void reset();

    // === РЕГИСТРАЦИЯ API ===
    void registerApi(const char* uri, HTTPMethod method, JsonApiHandler handler,
                    bool protectedEndpoint = true, const char* description = "");
    void registerUpload(const char* uri, HTTPMethod method, OnUploadCallback uploadcb,
                       std::function<void()> onComplete = nullptr,
                       bool protectedEndpoint = true);

    // === РЕГИСТРАЦИЯ КОЛБЭКОВ ===
    void setOnRegisterEndpoints(OnRegisterEndpointsCallback cb) { _onRegisterEndpoints = cb; }
    void setOnAppendStatus(OnAppendStatusCallback cb) { _onAppendStatus = cb; }
    void setOnRequest(OnRequestCallback cb) { _onRequest = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }

    // === АУТЕНТИФИКАЦИЯ ===
    bool login(const char* passwordHash);
    bool loginCard(const char* cardId);
    void logout();
    bool isAuthenticated() const { return _currentSessionToken.length() > 0; }
    const char* getSessionToken() const { return _currentSessionToken.c_str(); }
    const char* getSessionRole() const { return _currentSessionRole.c_str(); }

    // === CSRF ===
    String generateCsrfToken();
    bool verifyCsrfToken(const char* token);

    // === СТАТИСТИКА ===
    void serializeStats(Stream& stream) const;
    void serializeApiDocs(Stream& stream) const;
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

    // === ДИАГНОСТИКА ===
    void setVerboseLogging(bool enable) { _verboseLogging = enable; }

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    void initSystemEndpoints();
    bool handleFileRead(const String& path);
    bool checkAccess(bool adminRequired = false);
    bool checkDdosProtection();
    void clearSession();
    void checkSessionTimeout();
    void cleanupSessions();
    void updateStats(const char* endpoint, bool authenticated, bool isApi = true);
    void addCorsHeaders();
    void addSecurityHeaders();
    void addCacheHeaders(const String& path);
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    void generateSecureToken(char* dest, size_t size);
    bool safeStringCompare(const char* a, const char* b);
    const char* getContentType(const String& path) const;
    static const char* methodToString(HTTPMethod method);

    // === ОТПРАВКА СОБЫТИЙ ===
    void publishRequestEvent(const char* uri, const char* method, bool authenticated, bool isApi);
    void publishAuthEvent(const char* username, const char* role, bool success);
    void publishUploadEvent(const char* uri, bool start, uint32_t progress = 0, uint32_t total = 0);
    void publishErrorEvent(const char* errorCode);
    void publishSessionEvent(bool created);
    void publishCsrfEvent(bool success);

    // === ОБРАБОТЧИКИ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === СТРУКТУРА API ДОКУМЕНТАЦИИ ===
    struct ApiDoc {
        char uri[64];
        char method[8];
        char description[128];
        bool protectedEndpoint;
    };

    // === ДАННЫЕ ===
    WebServer _server;
    WebServerParams _params;
    WebStats _stats;
    uint32_t _moduleId = MODULE_ID_WEB;
    uint16_t _port = WEB_SERVER_PORT;
    bool _initialized = false;
    bool _serverStarted = false;
    bool _verboseLogging = false;
    bool _initInProgress = false;

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
    size_t _maxCacheSize = WEB_MAX_CACHE_SIZE;
    uint32_t _cacheTtlMs = WEB_CACHE_TTL_MS;

    // Колбэки
    OnRegisterEndpointsCallback _onRegisterEndpoints = nullptr;
    OnAppendStatusCallback _onAppendStatus = nullptr;
    OnRequestCallback _onRequest = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;

    // API документация
    std::vector<ApiDoc> _apiDocs;
    size_t _maxApiDocSlots = WEB_MAX_API_DOCS;

    // Мьютекс
    SemaphoreHandle_t _webMutex = nullptr;  // Рекурсивный!

    // Константы
    static constexpr const char* _authHeaderToken = "Authorization";
    static constexpr const char* _csrfHeader = "X-CSRF-Token";
    static constexpr const char* _sessionCookie = "SESSION";
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
};

// #endif // WEBSERVERMANAGER_H