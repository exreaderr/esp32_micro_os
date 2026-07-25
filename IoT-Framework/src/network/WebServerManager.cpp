// ============================================================================
// WebServerManager.cpp - ULTIMATE MICRO-OS V5.0 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Управление веб-сервером, API и статическими файлами.
//
// ИЗМЕНЕНИЯ v4.2.1:
// - Удалена глобальная переменная WebServerCore
// - Добавлен синглтон
// - Рекурсивный мьютекс вместо обычного
// - Исправлены опечатки (_status → _stats)
// - Добавлена CSRF-защита
// - Добавлено кэширование статики
// - Исправлены обработчики
//
// ИЗМЕНЕНИЯ v5.0 (АДАПТАЦИЯ):
// - Сохранена 100% функциональность v4.2.1
// - Добавлен метод publishWebEventInternal() для публикации через новую шину
// - Добавлен метод publishWebEvent() (публичный)
// - Добавлены вызовы publishWebEventInternal() в ключевые методы
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика (getDiagnostics, streamDiagnosticInfo)
// - Обновлена версия модуля
// ============================================================================
#include "WebServerManager.h"
#include "core/AppCore.h" // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include <ETH.h>
#include <time.h>
#include <cstdarg>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static WebServerManager _webServerManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
WebServerManager::WebServerManager() : _server(80) {
    _moduleId = MODULE_ID_WEB;
    _port = 80;

    _webMutex = xSemaphoreCreateRecursiveMutex();
    _stats.startTime = millis();
    _initialized = false;
    _serverStarted = false;
    _lastClientIp = 0;
    _lastRequestMs = 0;
    _requestCounter = 0;
    _lastSessionActivityMs = 0;
    _lastSessionCleanup = 0;
    _sessionId = 0;
    _verboseLogging = false;
    _totalEventsPublished = 0; // НОВОЕ

    _onRegisterEndpoints = nullptr;
    _onAppendStatus = nullptr;
    _onRequest = nullptr;
    _onStatsUpdate = nullptr;

    if (_webMutex == nullptr) {
        Serial.println("[WEB] CRITICAL: Failed to create mutex!");
    }

    _apiDocs.reserve(_maxApiDocSlots);
    _staticCache.reserve(10);

    Serial.println("[WEB] Instance created (v5.0)");
}

WebServerManager::~WebServerManager() {
    stop();
    if (_webMutex != nullptr) {
        vSemaphoreDelete(_webMutex);
        _webMutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
WebServerManager& WebServerManager::getInstance() {
    return _webServerManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void WebServerManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[WEB] %s\n", msg);

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

void WebServerManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

String WebServerManager::getClientIp() const {
    String ip = _server.client().remoteIP().toString();
    return ip;
}

// ============================================================================
// 4. НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЙ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void WebServerManager::publishWebEventInternal(const char* eventType, const char* details,
                                               bool success) {
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));

    event.type = EVENT_LOG_MESSAGE;
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF;

    event.payload.logData.level = success ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    snprintf(event.payload.logData.msg, sizeof(event.payload.logData.msg),
             "Web: %s %s - %s",
             eventType,
             details ? details : "",
             success ? "OK" : "FAIL");

    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void WebServerManager::publishWebEvent(const char* eventType, const char* details,
                                       bool success) {
    publishWebEventInternal(eventType, details, success);
}

// ============================================================================
// 5. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void WebServerManager::init() {
    logMessage("Init pending - call begin() with parameters");
}

void WebServerManager::start() {
    if (_initialized && !_serverStarted) {
        _server.begin(_port);
        _serverStarted = true;
        logMessage("Started on port %d", _port);
        publishWebEventInternal("START", String(_port).c_str(), true);
    }
}

void WebServerManager::stop() {
    if (_webMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_webMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (_serverStarted) {
            _server.stop();
            _serverStarted = false;
        }
        clearSession();
        _apiDocs.clear();
        _staticCache.clear();
        _sessions.clear();
        _initialized = false;
        xSemaphoreGiveRecursive(_webMutex);
        logMessage("Stopped");
        publishWebEventInternal("STOP", nullptr, true);
    }
}

void WebServerManager::tick() {
    if (!_initialized || !_serverStarted) return;

    esp_task_wdt_reset();
    _server.handleClient();

    checkSessionTimeout();

    uint32_t now = millis();
    if (now - _lastSessionCleanup > 60000) {
        _lastSessionCleanup = now;
        cleanupSessions();
    }

    if (_onStatsUpdate) {
        static uint32_t lastStatsUpdate = 0;
        if (now - lastStatsUpdate > 60000) {
            lastStatsUpdate = now;
            _onStatsUpdate(_stats);
        }
    }
}

// ============================================================================
// 6. ОБРАБОТКА СОБЫТИЙ
// ============================================================================
void WebServerManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                                    int32_t id, void* eventData) {
    WebServerManager* instance = static_cast<WebServerManager*>(handlerArgs);
    if (!instance) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
                break;
            case SH_EVENT_NET_CONNECTED:
                if (instance->_initialized) {
                    instance->_server.begin(instance->_port);
                    instance->_serverStarted = true;
                    instance->logMessage("Server started on network connection");
                    instance->publishWebEventInternal("NET_CONNECTED", nullptr, true);
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

void WebServerManager::onEvent(int32_t eventId, const ShEventData* data) {
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
            if (_initialized) {
                _server.begin(_port);
                _serverStarted = true;
                publishWebEventInternal("NET_CONNECTED", nullptr, true);
            }
            break;
        default:
            break;
    }
}

bool WebServerManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN ||
            eventId == SH_EVENT_NET_CONNECTED);
}

// ============================================================================
// 7. СТАТУС
// ============================================================================
const char* WebServerManager::getStatus() const {
    static char statusBuffer[128];

    const char* state = _serverStarted ? "RUNNING" : "STOPPED";

    snprintf(statusBuffer, sizeof(statusBuffer),
            "State: %s, Port: %d, Requests: %lu, API: %lu, Sessions: %lu",
            state,
            _port,
            _stats.totalRequests,
            _stats.apiCalls,
            _stats.activeSessions);

    return statusBuffer;
}

// ============================================================================
// 8. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void WebServerManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _stats.totalRequests;

    snprintf(data->payload, sizeof(data->payload),
            "running:%d,req:%lu,auth:%lu,api:%lu,err:%lu,sessions:%lu,events:%lu",
            _serverStarted ? 1 : 0,
            _stats.totalRequests,
            _stats.authenticatedRequests,
            _stats.apiCalls,
            _stats.errors,
            _stats.activeSessions,
            _totalEventsPublished); // НОВОЕ
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 9. ОБРАБОТКА КОМАНД
// ============================================================================
void WebServerManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case 0x0800: { // GET_STATS
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0801;
            response.value = _stats.totalRequests;
            snprintf(response.payload, sizeof(response.payload),
                    "req:%lu,auth:%lu,api:%lu,uploads:%lu,errors:%lu,sessions:%lu",
                    _stats.totalRequests,
                    _stats.authenticatedRequests,
                    _stats.apiCalls,
                    _stats.uploads,
                    _stats.errors,
                    _stats.activeSessions);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            publishWebEventInternal("CMD_GET_STATS", nullptr, true);
            break;
        }

        case 0x0802: // LOGOUT
            logout();
            publishWebEventInternal("CMD_LOGOUT", nullptr, true);
            break;

        case 0x0803: { // GET_API_DOCS
            String docs = "[";
            for (size_t i = 0; i < _apiDocs.size(); i++) {
                if (i > 0) docs += ",";
                docs += "{\"uri\":\"" + String(_apiDocs[i].uri) +
                       "\",\"method\":\"" + String(_apiDocs[i].method) +
                       "\",\"protected\":" + String(_apiDocs[i].protectedEndpoint ? "true" : "false") +
                       ",\"desc\":\"" + String(_apiDocs[i].description) + "\"}";
            }
            docs += "]";
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0804;
            response.value = _apiDocs.size();
            strncpy(response.payload, docs.c_str(), sizeof(response.payload) - 1);
            response.payload[sizeof(response.payload) - 1] = '\0';
            response.payloadLen = docs.length();
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            publishWebEventInternal("CMD_GET_API_DOCS", nullptr, true);
            break;
        }

        case 0x0805: { // GET_CSRF_TOKEN
            String token = generateCsrfToken();
            ShEventData response;
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0806;
            response.value = token.length();
            strncpy(response.payload, token.c_str(), sizeof(response.payload) - 1);
            response.payload[sizeof(response.payload) - 1] = '\0';
            response.payloadLen = token.length();
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            publishWebEventInternal("CMD_GET_CSRF", nullptr, true);
            break;
        }

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 10. ОТПРАВКА СОБЫТИЙ (ОРИГИНАЛЬНЫЕ МЕТОДЫ, РАСШИРЕНЫ)
// ============================================================================
void WebServerManager::publishRequestEvent(const char* uri, const char* method,
                                           bool authenticated, bool isApi) {
    WebRequestEvent event;
    strncpy(event.uri, uri ? uri : "/", sizeof(event.uri) - 1);
    event.uri[sizeof(event.uri) - 1] = '\0';
    strncpy(event.method, method ? method : "GET", sizeof(event.method) - 1);
    event.method[sizeof(event.method) - 1] = '\0';
    event.clientIp = _server.client().remoteIP();
    event.timestamp = millis();
    event.authenticated = authenticated;
    event.isApi = isApi;
    event.responseCode = 200;

    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_WEB_REQUEST;
    data.value = authenticated ? 1 : 0;
    memcpy(data.payload, &event, min(sizeof(WebRequestEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(WebRequestEvent);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    // НОВОЕ: публикация через новую шину
    publishWebEventInternal("REQUEST", uri ? uri : "/", true);
}

void WebServerManager::publishAuthEvent(const char* username, const char* role, bool success) {
    WebAuthEvent event;
    strncpy(event.username, username ? username : "unknown", sizeof(event.username) - 1);
    event.username[sizeof(event.username) - 1] = '\0';
    strncpy(event.role, role ? role : "none", sizeof(event.role) - 1);
    event.role[sizeof(event.role) - 1] = '\0';
    strncpy(event.token, _currentSessionToken.c_str(), sizeof(event.token) - 1);
    event.token[sizeof(event.token) - 1] = '\0';
    event.success = success;
    event.timestamp = millis();
    snprintf(event.clientIp, sizeof(event.clientIp), "%u", _server.client().remoteIP());

    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = success ? SH_EVENT_WEB_AUTH_SUCCESS : SH_EVENT_WEB_AUTH_FAILED;
    data.value = success ? 1 : 0;
    memcpy(data.payload, &event, min(sizeof(WebAuthEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(WebAuthEvent);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    // НОВОЕ: публикация через новую шину
    publishWebEventInternal(success ? "AUTH_SUCCESS" : "AUTH_FAILED",
                           username ? username : "unknown",
                           success);
}

void WebServerManager::publishUploadEvent(const char* uri, bool start,
                                          uint32_t progress, uint32_t total) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = start ? SH_EVENT_WEB_UPLOAD_START :
                  (progress < total ? SH_EVENT_WEB_UPLOAD_PROGRESS : SH_EVENT_WEB_UPLOAD_COMPLETE);
    data.value = start ? 0 : (total > 0 ? (progress * 100 / total) : 0);
    snprintf(data.payload, sizeof(data.payload), "%s:%lu/%lu",
            uri ? uri : "upload", progress, total);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    // НОВОЕ: публикация через новую шину
    if (start) {
        publishWebEventInternal("UPLOAD_START", uri, true);
    } else if (progress >= total && total > 0) {
        publishWebEventInternal("UPLOAD_COMPLETE", uri, true);
    } else {
        publishWebEventInternal("UPLOAD_PROGRESS", uri, true);
    }
}

void WebServerManager::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_WEB_ERROR;
    data.value = _stats.errors;
    strncpy(data.payload, errorCode ? errorCode : "UNKNOWN_ERROR", sizeof(data.payload) - 1);
    data.payload[sizeof(data.payload) - 1] = '\0';
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    // НОВОЕ: публикация через новую шину
    publishWebEventInternal("ERROR", errorCode ? errorCode : "UNKNOWN_ERROR", false);
}

void WebServerManager::publishSessionEvent(bool created) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = created ? SH_EVENT_WEB_SESSION_CREATED : SH_EVENT_WEB_SESSION_DESTROYED;
    data.value = _stats.activeSessions;
    snprintf(data.payload, sizeof(data.payload), "%s:%s",
            _currentSessionUsername.c_str(),
            _currentSessionRole.c_str());
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    // НОВОЕ: публикация через новую шину
    publishWebEventInternal(created ? "SESSION_CREATED" : "SESSION_DESTROYED",
                           _currentSessionUsername.c_str(),
                           created);
}

void WebServerManager::publishCsrfEvent(bool success) {
    ShEventData data;
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = success ? SH_EVENT_WEB_CSRF_VERIFIED : SH_EVENT_WEB_CSRF_FAILED;
    data.value = success ? 1 : 0;
    strncpy(data.payload, success ? "CSRF verified" : "CSRF failed", sizeof(data.payload) - 1);
    data.payload[sizeof(data.payload) - 1] = '\0';
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);

    // НОВОЕ: публикация через новую шину
    publishWebEventInternal(success ? "CSRF_VERIFIED" : "CSRF_FAILED", nullptr, success);
}

// ============================================================================
// 11. ИНИЦИАЛИЗАЦИЯ
// ============================================================================
void WebServerManager::begin(uint16_t port, const WebServerParams& params) {
    if (_webMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_webMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        _params = params;
        _port = port;

        if (_serverStarted) {
            _server.stop();
            _serverStarted = false;
        }

        initSystemEndpoints();

        if (_onRegisterEndpoints != nullptr) {
            _onRegisterEndpoints(_server);
        }

        _server.onNotFound([this]() {
            if (!checkDdosProtection()) return;

            if (!handleFileRead(_server.uri())) {
                updateStats(_server.uri().c_str(), false, false);
                _server.send(404, "text/plain", "404: Not Found");
                publishErrorEvent("NOT_FOUND");
                publishWebEventInternal("404", _server.uri().c_str(), false);
            }
        });

        _server.on("", HTTP_OPTIONS, [this]() {
            addCorsHeaders();
            _server.send(204);
        });

        const char* headers[] = {_authHeaderToken, _csrfHeader};
        _server.collectHeaders(headers, 2);

        _server.begin(port);
        _serverStarted = true;
        _initialized = true;
        _stats.startTime = millis();

        generateCsrfToken();

        xSemaphoreGiveRecursive(_webMutex);

        esp_event_handler_instance_register(
            SH_SYS_EVENTS,
            ESP_EVENT_ANY_ID,
            &WebServerManager::eventHandler,
            this,
            NULL
        );
        esp_event_handler_instance_register(
            SH_APP_EVENTS,
            ESP_EVENT_ANY_ID,
            &WebServerManager::eventHandler,
            this,
            NULL
        );

        logMessage("Started on port %d", port);
        publishWebEventInternal("BEGIN", String(port).c_str(), true);
    }
}

void WebServerManager::end() {
    stop();
}

void WebServerManager::reset() {
    logMessage("Reset requested");
    stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    begin(_port, _params);
    logMessage("Reset complete");
    publishWebEventInternal("RESET", nullptr, true);
}

// ============================================================================
// 12. РЕГИСТРАЦИЯ API
// ============================================================================
void WebServerManager::registerApi(const char* uri, HTTPMethod method,
                                   JsonApiHandler handler,
                                   bool protectedEndpoint,
                                   const char* description) {
    if (_webMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_webMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (handler == nullptr) {
            logMessage("Handler is null for %s", uri);
            xSemaphoreGiveRecursive(_webMutex);
            return;
        }

        if (_apiDocs.size() < _maxApiDocSlots) {
            ApiDoc doc;
            strncpy(doc.uri, uri, sizeof(doc.uri) - 1);
            strncpy(doc.method, methodToString(method), sizeof(doc.method) - 1);
            strncpy(doc.description, description ? description : "", sizeof(doc.description) - 1);
            doc.protectedEndpoint = protectedEndpoint;
            _apiDocs.push_back(doc);
        } else {
            logMessage("API docs limit reached");
        }

        _server.on(uri, method, [this, handler, protectedEndpoint, uri]() {
            if (!checkDdosProtection()) return;

            updateStats(uri, isAuthenticated(), true);
            publishRequestEvent(uri, "API", isAuthenticated(), true);
            _stats.apiCalls++;

            if (protectedEndpoint && !checkAccess(true)) {
                _stats.errors++;
                _stats.failedAuthAttempts++;
                _server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                publishErrorEvent("UNAUTHORIZED");
                publishWebEventInternal("API_UNAUTHORIZED", uri, false);
                return;
            }

            if (_params.enableCsrf && protectedEndpoint) {
                if (!verifyCsrfToken(_server.header(_csrfHeader))) {
                    _stats.csrfErrors++;
                    _server.send(403, "application/json", "{\"error\":\"Invalid CSRF token\"}");
                    publishCsrfEvent(false);
                    publishWebEventInternal("API_CSRF_FAIL", uri, false);
                    return;
                }
                publishCsrfEvent(true);
            }

            JsonDocument reqDoc;
            if (_server.hasArg("plain")) {
                DeserializationError err = deserializeJson(reqDoc, _server.arg("plain"));
                if (err) {
                    _server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                    publishErrorEvent("INVALID_JSON");
                    publishWebEventInternal("API_INVALID_JSON", uri, false);
                    return;
                }
            }

            JsonDocument respDoc;
            JsonObject resp = respDoc.to<JsonObject>();

            resp["status"] = "ok";
            resp["timestamp"] = millis();

            handler(reqDoc.as<JsonVariant>(), resp);

            String response;
            serializeJson(respDoc, response);

            addSecurityHeaders();
            if (_params.corsEnabled) addCorsHeaders();
            _server.send(200, "application/json", response);

            publishWebEventInternal("API_CALL", uri, true);
        });

        logMessage("Registered API: %s %s %s",
                  uri,
                  methodToString(method),
                  protectedEndpoint ? "[AUTH]" : "[PUBLIC]");
        xSemaphoreGiveRecursive(_webMutex);
    }
}

void WebServerManager::registerUpload(const char* uri, HTTPMethod method,
                                      OnUploadCallback uploadcb,
                                      std::function<void()> onComplete,
                                      bool protectedEndpoint) {
    if (_webMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_webMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        _server.on(uri, method, [this, protectedEndpoint, uri, onComplete, uploadcb]() {
            if (!checkDdosProtection()) return;

            if (protectedEndpoint && !checkAccess(true)) {
                _server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
                publishErrorEvent("UPLOAD_UNAUTHORIZED");
                publishWebEventInternal("UPLOAD_UNAUTHORIZED", uri, false);
                return;
            }

            HTTPUpload& upload = _server.upload();

            if (upload.status == UPLOAD_FILE_START) {
                _stats.uploads++;
                publishUploadEvent(uri, true, 0, 0);
                logMessage("Upload start: %s", uri);
                publishWebEventInternal("UPLOAD_START", uri, true);
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (uploadcb) uploadcb(upload);

                uint32_t totalLen = _server.header("Content-Length").toInt();
                if (totalLen == 0) totalLen = upload.totalSize;

                static uint32_t lastProgressUpdate = 0;
                uint32_t now = millis();
                if (now - lastProgressUpdate > 200) {
                    lastProgressUpdate = now;
                    publishUploadEvent(uri, false, upload.currentSize, totalLen);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                publishUploadEvent(uri, false, upload.currentSize, upload.totalSize);
                logMessage("Upload complete: %s", uri);
                publishWebEventInternal("UPLOAD_COMPLETE", uri, true);

                if (onComplete) {
                    onComplete();
                }

                _server.send(200, "application/json", "{\"status\":\"ok\"}");
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                logMessage("Upload aborted: %s", uri);
                publishErrorEvent("UPLOAD_ABORTED");
                publishWebEventInternal("UPLOAD_ABORTED", uri, false);
                _server.send(500, "application/json", "{\"error\":\"Upload aborted\"}");
            }
        });

        logMessage("Registered upload: %s %s %s",
                  uri,
                  methodToString(method),
                  protectedEndpoint ? "[AUTH]" : "[PUBLIC]");
        xSemaphoreGiveRecursive(_webMutex);
    }
}

// ============================================================================
// 13. СТАТИЧЕСКИЕ ФАЙЛЫ
// ============================================================================
bool WebServerManager::handleFileRead(const String& path) {
    String actualPath = path;
    if (actualPath.endsWith("/")) actualPath += "index.html";

    if (_params.enableCache) {
        auto it = _staticCache.find(actualPath);
        if (it != _staticCache.end() && it->second.valid) {
            if (millis() - it->second.timestamp < _cacheTtlMs) {
                _stats.staticCacheHits++;
                _server.send(200, it->second.contentType, it->second.data);
                return true;
            }
        }
        _stats.staticCacheMisses++;
    }

    if (_params.enableGzip) {
        String gzPath = actualPath + ".gz";
        if (LittleFS.exists(gzPath)) {
            File file = LittleFS.open(gzPath, "r");
            if (file) {
                String data = file.readString();
                file.close();

                const char* contentType = getContentType(actualPath);
                addSecurityHeaders();
                if (_params.corsEnabled) addCorsHeaders();
                addCacheHeaders(actualPath);

                _server.sendHeader("Content-Encoding", "gzip");
                _server.send(200, contentType, data);

                if (_params.enableCache && _staticCache.size() < _maxCacheSize) {
                    CacheEntry entry = {data, contentType, millis(), true};
                    _staticCache[actualPath] = entry;
                }
                _stats.staticRequests++;
                publishWebEventInternal("STATIC_SERVED", actualPath.c_str(), true);
                return true;
            }
        }
    }

    if (LittleFS.exists(actualPath)) {
        File file = LittleFS.open(actualPath, "r");
        if (file) {
            String data = file.readString();
            file.close();

            const char* contentType = getContentType(actualPath);
            addSecurityHeaders();
            if (_params.corsEnabled) addCorsHeaders();
            addCacheHeaders(actualPath);

            _server.send(200, contentType, data);

            if (_params.enableCache && _staticCache.size() < _maxCacheSize) {
                CacheEntry entry = {data, contentType, millis(), true};
                _staticCache[actualPath] = entry;
            }
            _stats.staticRequests++;
            publishWebEventInternal("STATIC_SERVED", actualPath.c_str(), true);
            return true;
        }
    }

    return false;
}

// ============================================================================
// 14. СИСТЕМНЫЕ ЭНДПОИНТЫ
// ============================================================================
void WebServerManager::initSystemEndpoints() {
    // === /api/status ===
    registerApi("/api/status", HTTP_GET, [this](JsonVariant req, JsonObject resp) {
        resp["status"] = "ok";
        resp["uptime"] = (millis() - _stats.startTime) / 1000;
        resp["requests"] = _stats.totalRequests;
        resp["authenticated"] = _stats.authenticatedRequests;
        resp["api_calls"] = _stats.apiCalls;
        resp["uploads"] = _stats.uploads;
        resp["errors"] = _stats.errors;
        resp["active_sessions"] = _stats.activeSessions;
        resp["authenticated"] = isAuthenticated();

        if (_onAppendStatus) {
            _onAppendStatus(resp, isAuthenticated());
        }
    }, false, "System status");

    // === /api/auth ===
    registerApi("/api/auth", HTTP_POST, [this](JsonVariant req, JsonObject resp) {
        const char* inputHash = req["password_hash"] | "";
        const char* cardId = req["card_id"] | "";
        bool authSuccess = false;

        if (strlen(inputHash) > 0) {
            if (safeStringCompare(String(inputHash), String(_params.adminPasswordHash)) ||
                _params.adminPasswordHash[0] == '\0') {
                authSuccess = true;
            }
        }

        if (!authSuccess && strlen(cardId) > 0) {
            if (safeStringCompare(String(cardId), String(_params.hostname))) {
                authSuccess = true;
            }
        }

        if (authSuccess) {
            char tokenBuf[64];
            generateSecureToken(tokenBuf, sizeof(tokenBuf));
            _currentSessionToken = tokenBuf;
            _lastSessionActivityMs = millis();
            _currentSessionRole = "admin";
            _currentSessionUsername = "Administrator";
            _currentSessionCardId = cardId;

            SessionInfo info;
            strncpy(info.token, tokenBuf, sizeof(info.token) - 1);
            strncpy(info.role, "admin", sizeof(info.role) - 1);
            strncpy(info.username, "Administrator", sizeof(info.username) - 1);
            strncpy(info.cardId, cardId, sizeof(info.cardId) - 1);
            info.lastActivityMs = millis();
            info.createdAt = millis();
            info.valid = true;
            _sessions[tokenBuf] = info;

            _stats.authenticatedRequests++;
            _stats.activeSessions = _sessions.size();

            publishAuthEvent("Administrator", "admin", true);
            publishSessionEvent(true);

            resp["success"] = true;
            resp["token"] = tokenBuf;
            resp["role"] = "admin";
            resp["csrf_token"] = generateCsrfToken();

            publishWebEventInternal("LOGIN_SUCCESS", "Administrator", true);
        } else {
            _stats.failedAuthAttempts++;
            publishAuthEvent("unknown", "none", false);
            resp["success"] = false;
            resp["error"] = "Authentication failed";
            publishWebEventInternal("LOGIN_FAILED", nullptr, false);
        }
    }, false, "Authenticate");

    // === /api/logout ===
    registerApi("/api/logout", HTTP_POST, [this](JsonVariant req, JsonObject resp) {
        logout();
        resp["success"] = true;
        publishWebEventInternal("LOGOUT", nullptr, true);
    }, true, "Logout");

    // === /api/csrf ===
    registerApi("/api/csrf", HTTP_GET, [this](JsonVariant req, JsonObject resp) {
        resp["csrf_token"] = generateCsrfToken();
        publishWebEventInternal("CSRF_GENERATED", nullptr, true);
    }, false, "Get CSRF token");

    // === /api/health ===
    registerApi("/api/health", HTTP_GET, [this](JsonVariant req, JsonObject resp) {
        resp["status"] = "healthy";
        resp["uptime"] = (millis() - _stats.startTime) / 1000;
        resp["free_heap"] = ESP.getFreeHeap();
        resp["free_psram"] = ESP.getFreePsram();
        resp["server"] = _serverStarted ? "running" : "stopped";
        publishWebEventInternal("HEALTH_CHECK", nullptr, true);
    }, false, "Health check");

    publishWebEventInternal("SYSTEM_ENDPOINTS_INIT", nullptr, true);
}

// ============================================================================
// 15. АВТОРИЗАЦИЯ
// ============================================================================
bool WebServerManager::checkAccess(bool adminRequired) {
    if (_currentSessionToken.length() == 0) return false;

    if (millis() - _lastSessionActivityMs > _params.sessionTimeoutMs) {
        clearSession();
        logMessage("Session timeout");
        publishWebEventInternal("SESSION_TIMEOUT", nullptr, false);
        return false;
    }

    if (!_server.hasHeader(_authHeaderToken)) return false;

    String header = _server.header(_authHeaderToken);
    if (!header.startsWith("Bearer ")) return false;

    String token = header.substring(7);

    if (!safeStringCompare(token, _currentSessionToken)) {
        logMessage("Invalid token");
        publishWebEventInternal("INVALID_TOKEN", nullptr, false);
        return false;
    }

    _lastSessionActivityMs = millis();

    auto it = _sessions.find(_currentSessionToken);
    if (it != _sessions.end()) {
        it->second.lastActivityMs = millis();
    }

    if (adminRequired && _currentSessionRole != "admin") {
        return false;
    }

    return true;
}

bool WebServerManager::isAuthenticated() const {
    if (_currentSessionToken.length() == 0) return false;

    if (millis() - _lastSessionActivityMs > _params.sessionTimeoutMs) {
        return false;
    }

    return true;
}

bool WebServerManager::login(const char* passwordHash) {
    if (_webMutex == nullptr) return false;

    bool success = false;

    if (xSemaphoreTakeRecursive(_webMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (safeStringCompare(String(passwordHash), String(_params.adminPasswordHash)) ||
            _params.adminPasswordHash[0] == '\0') {

            char tokenBuf[64];
            generateSecureToken(tokenBuf, sizeof(tokenBuf));

            _currentSessionToken = tokenBuf;
            _lastSessionActivityMs = millis();
            _currentSessionRole = "admin";
            _currentSessionUsername = "Administrator";
            _currentSessionCardId = "WEB_ADMIN";

            _stats.activeSessions = 1;
            _stats.authenticatedRequests++;

            success = true;
            publishAuthEvent("Administrator", "admin", true);
            publishSessionEvent(true);
            publishWebEventInternal("LOGIN_SUCCESS", "Administrator", true);
        } else {
            _stats.failedAuthAttempts++;
            publishAuthEvent("unknown", "none", false);
            publishWebEventInternal("LOGIN_FAILED", nullptr, false);
        }

        xSemaphoreGiveRecursive(_webMutex);
    }

    return success;
}

bool WebServerManager::loginCard(const char* cardId) {
    if (cardId == nullptr) return false;
    return login(cardId);
}

void WebServerManager::logout() {
    clearSession();
    publishWebEventInternal("LOGOUT", nullptr, true);
}

// ============================================================================
// 16. УПРАВЛЕНИЕ СЕССИЯМИ
// ============================================================================
void WebServerManager::clearSession() {
    if (_currentSessionToken.length() > 0) {
        auto it = _sessions.find(_currentSessionToken);
        if (it != _sessions.end()) {
            it->second.valid = false;
            _sessions.erase(it);
        }
        _stats.activeSessions = _sessions.size();
        publishSessionEvent(false);
    }

    _currentSessionToken = "";
    _currentSessionRole = "";
    _currentSessionUsername = "";
    _currentSessionCardId = "";
    _stats.activeSessions = _sessions.size();
}

void WebServerManager::checkSessionTimeout() {
    if (_currentSessionToken.length() == 0) return;

    if (millis() - _lastSessionActivityMs > _params.sessionTimeoutMs) {
        logMessage("Session expired");
        clearSession();
        publishErrorEvent("SESSION_EXPIRED");
        publishWebEventInternal("SESSION_EXPIRED", nullptr, false);
    }
}

void WebServerManager::cleanupSessions() {
    uint32_t now = millis();
    auto it = _sessions.begin();

    while (it != _sessions.end()) {
        if (now - it->second.lastActivityMs > _params.sessionTimeoutMs) {
            it = _sessions.erase(it);
        } else {
            ++it;
        }
    }

    _stats.activeSessions = _sessions.size();
}

// ============================================================================
// 17. CSRF-ЗАЩИТА
// ============================================================================
String WebServerManager::generateCsrfToken() {
    char token[64];
    generateSecureToken(token, sizeof(token));
    _csrfToken = String(token);
    _csrfTokenCreated = millis();
    publishWebEventInternal("CSRF_GENERATED", nullptr, true);
    return _csrfToken;
}

bool WebServerManager::verifyCsrfToken(const char* token) {
    if (token == nullptr) return false;

    if (millis() - _csrfTokenCreated > CSRF_TOKEN_TTL) {
        generateCsrfToken();
        publishWebEventInternal("CSRF_EXPIRED", nullptr, false);
        return false;
    }

    bool valid = safeStringCompare(String(token), _csrfToken);

    if (valid) {
        publishCsrfEvent(true);
        publishWebEventInternal("CSRF_VERIFIED", nullptr, true);
    } else {
        publishCsrfEvent(false);
        _stats.csrfErrors++;
        publishWebEventInternal("CSRF_FAILED", nullptr, false);
    }

    return valid;
}

// ============================================================================
// 18. DDoS-ЗАЩИТА
// ============================================================================
bool WebServerManager::checkDdosProtection() {
    if (!_params.ddosProtection) return true;

    uint32_t clientIp = _server.client().remoteIP();
    uint32_t now = millis();

    if (clientIp != _lastClientIp) {
        _lastClientIp = clientIp;
        _lastRequestMs = now;
        _requestCounter = 1;
        return true;
    }

    if (now - _lastRequestMs > 1000) {
        _lastRequestMs = now;
        _requestCounter = 1;
        return true;
    }

    _requestCounter++;

    if (_requestCounter > _params.ddosLimit) {
        logMessage("DDoS block: %s", getClientIp().c_str());
        _server.send(429, "text/plain", "Too many requests");
        publishErrorEvent("DDOS_BLOCK");
        publishWebEventInternal("DDOS_BLOCK", getClientIp().c_str(), false);
        return false;
    }

    return true;
}

// ============================================================================
// 19. ЗАГОЛОВКИ
// ============================================================================
void WebServerManager::addCorsHeaders() {
    if (_params.corsEnabled) {
        _server.sendHeader("Access-Control-Allow-Origin", _params.corsOrigin);
        _server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        _server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-CSRF-Token");
        _server.sendHeader("Access-Control-Max-Age", "86400");
    }
}

void WebServerManager::addSecurityHeaders() {
    _server.sendHeader("X-Content-Type-Options", "nosniff");
    _server.sendHeader("X-Frame-Options", "DENY");
    _server.sendHeader("X-XSS-Protection", "1; mode=block");
    _server.sendHeader("Referrer-Policy", "strict-origin-when-cross-origin");
}

void WebServerManager::addCacheHeaders(const String& path) {
    if (_params.enableCache) {
        if (path.endsWith(".js") || path.endsWith(".css") ||
            path.endsWith(".png") || path.endsWith(".jpg") ||
            path.endsWith(".woff2")) {
            _server.sendHeader("Cache-Control", "public, max-age=86400");
        } else {
            _server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        }
    } else {
        _server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    }
}

// ============================================================================
// 20. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
bool WebServerManager::safeStringCompare(const String& a, const String& b) {
    if (a.length() != b.length()) return false;

    volatile int result = 0;
    for (size_t i = 0; i < a.length(); i++) {
        result |= (a[i] ^ b[i]);
    }
    return (result == 0);
}

void WebServerManager::generateSecureToken(char* dest, size_t size) {
    if (dest == nullptr || size < 32) return;

    uint8_t random[32];
    for (size_t i = 0; i < 32; i++) {
        random[i] = esp_random() & 0xFF;
    }

    const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < 32 && i * 2 + 1 < size; i++) {
        dest[i * 2] = hex[(random[i] >> 4) & 0x0F];
        dest[i * 2 + 1] = hex[random[i] & 0x0F];
    }
    if (size > 64) dest[64] = '\0';
}

void WebServerManager::updateStats(const char* endpoint, bool authenticated, bool isApi) {
    _stats.totalRequests++;

    if (authenticated) {
        _stats.authenticatedRequests++;
    }

    if (isApi) {
        _stats.apiCalls++;
    }

    _stats.lastRequestTime = millis();

    if (endpoint != nullptr) {
        strncpy(_stats.lastEndpoint, endpoint, sizeof(_stats.lastEndpoint) - 1);
    }
}

const char* WebServerManager::getContentType(const String& path) const {
    if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".js")) return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".ico")) return "image/x-icon";
    if (path.endsWith(".txt")) return "text/plain";
    if (path.endsWith(".gz")) return "application/gzip";
    if (path.endsWith(".woff2")) return "font/woff2";
    if (path.endsWith(".woff")) return "font/woff";
    if (path.endsWith(".ttf")) return "font/ttf";
    if (path.endsWith(".eot")) return "application/vnd.ms-fontobject";
    if (path.endsWith(".xml")) return "application/xml";
    if (path.endsWith(".pdf")) return "application/pdf";
    if (path.endsWith(".zip")) return "application/zip";
    return "application/octet-stream";
}

// ============================================================================
// 21. СТАТИЧЕСКИЙ МЕТОД ДЛЯ ПРЕОБРАЗОВАНИЯ МЕТОДА
// ============================================================================
const char* WebServerManager::methodToString(HTTPMethod method) {
    switch (method) {
        case HTTP_GET: return "GET";
        case HTTP_POST: return "POST";
        case HTTP_PUT: return "PUT";
        case HTTP_DELETE: return "DELETE";
        case HTTP_PATCH: return "PATCH";
        case HTTP_HEAD: return "HEAD";
        case HTTP_OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// 22. ДИАГНОСТИКА
// ============================================================================
void WebServerManager::serializeStats(Stream& stream) const {
    JsonDocument doc;
    doc["total_requests"] = _stats.totalRequests;
    doc["authenticated_requests"] = _stats.authenticatedRequests;
    doc["failed_auth_attempts"] = _stats.failedAuthAttempts;
    doc["api_calls"] = _stats.apiCalls;
    doc["static_requests"] = _stats.staticRequests;
    doc["uploads"] = _stats.uploads;
    doc["errors"] = _stats.errors;
    doc["active_sessions"] = _stats.activeSessions;
    doc["uptime"] = (millis() - _stats.startTime) / 1000;
    doc["cache_hits"] = _stats.staticCacheHits;
    doc["cache_misses"] = _stats.staticCacheMisses;
    doc["csrf_errors"] = _stats.csrfErrors;
    doc["last_endpoint"] = _stats.lastEndpoint;

    serializeJson(doc, stream);
}

void WebServerManager::serializeApiDocs(Stream& stream) const {
    JsonDocument doc;
    JsonArray arr = doc["apis"].to<JsonArray>();

    for (const auto& api : _apiDocs) {
        JsonObject obj = arr.add<JsonObject>();
        obj["uri"] = api.uri;
        obj["method"] = api.method;
        obj["protected"] = api.protectedEndpoint;
        obj["description"] = api.description;
    }

    serializeJson(doc, stream);
}

// ============================================================================
// 23. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void WebServerManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================");
    stream.println(" WEB SERVER MANAGER DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Server Running: %s\n", _serverStarted ? "YES" : "NO");
    stream.printf(" Port: %d\n", _port);
    stream.printf(" Authenticated: %s\n", isAuthenticated() ? "YES" : "NO");
    stream.printf(" Active Sessions: %lu\n", _stats.activeSessions);
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("-- Stats --");
    stream.printf(" Total Requests: %lu\n", _stats.totalRequests);
    stream.printf(" Auth Requests: %lu\n", _stats.authenticatedRequests);
    stream.printf(" Failed Auth: %lu\n", _stats.failedAuthAttempts);
    stream.printf(" API Calls: %lu\n", _stats.apiCalls);
    stream.printf(" Static Requests: %lu\n", _stats.staticRequests);
    stream.printf(" Uploads: %lu\n", _stats.uploads);
    stream.printf(" Errors: %lu\n", _stats.errors);
    stream.printf(" CSRF Errors: %lu\n", _stats.csrfErrors);
    stream.printf(" Cache Hits: %lu\n", _stats.staticCacheHits);
    stream.printf(" Cache Misses: %lu\n", _stats.staticCacheMisses);
    stream.println("-- API Docs --");
    stream.printf(" Registered: %zu\n", _apiDocs.size());
    for (const auto& api : _apiDocs) {
        stream.printf("  %s %s %s\n",
                     api.method,
                     api.uri,
                     api.protectedEndpoint ? "[AUTH]" : "[PUBLIC]");
    }
    stream.println("==============================");
}