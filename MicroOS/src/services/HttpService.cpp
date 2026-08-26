// ============================================================================
// HttpService.cpp — реализация веб-сервера и разделённого веб-UI
// ============================================================================
#include "HttpService.h"
#include "ConfigService.h"
#include "NetworkManager.h"
#include "AuthService.h"
#include "LogService.h"
#include "AuditService.h"
#include "TelemetryService.h"
#include "TimeService.h"
#include "StorageService.h"
#include "UpdateService.h"
#include "HealthMonitor.h"
#include "../core/Events.h"
#include "../core/Kernel.h"
#include <esp_random.h>
#include <new>          // placement nothrow: heap-буфер JSON в init()
#include <Update.h>   // U_FLASH / U_SPIFFS для OTA-приёма (залежь №3)

// Страница админки (PROGMEM) и её обработчик — в конце файла.

HttpService& HttpService::getInstance() {
    static HttpService instance;
    return instance;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void HttpService::init() {
    // JSON-буфер — ОДИН блок heap на всю жизнь (урок outbox 5.1.0:
    // фиксированные бюджеты BSS не жонглируем, но один статический heap-блок
    // легален). nothrow: провал → fallback 2 КБ, сервер всё равно поднимется.
    _jsonBuf = new (std::nothrow) char[HTTP_JSON_BUF];
    if (_jsonBuf) _jsonBuf[0] = '\0';

    // Объявляем нестандартный заголовок ДО begin: иначе WebServer его
    // не соберёт и checkAdmin() всегда будет видеть пустой токен.
    const char* headerKeys[] = { "X-Auth-Token" };
    _server.collectHeaders(headerKeys, 1);
    _initialized = true;
    log(LogLevel::Info, "init: web server ready (port 80, json buf %u %s)",
        (unsigned)jsonBufSize(), _jsonBuf ? "heap" : "FALLBACK");
}

void HttpService::start()  { _started = true; }
void HttpService::stop()   { serverStop(); _started = false; }

void HttpService::tick() {
    // Стейт-машина: сервер жив только при реальной сети
    bool net = NetworkService::getInstance().isConnected();
    if (!net) _netUpSinceMs = 0;
    else if (_netUpSinceMs == 0) _netUpSinceMs = millis();
    // 5.8.0, HTTP-гард: пауза после подъёма сети (см. HTTP_WEB_DELAY_MS) —
    // в бут-шторм не добавляем lwIP-сокеты веб-клиентов. Safe Mode тоже
    // ждёт: 10 с до recovery-консоли — приемлемая цена живого ядра.
    bool want = net && (uint32_t)(millis() - _netUpSinceMs) >= HTTP_WEB_DELAY_MS;
    if (want != _serverUp) {
        if (want) serverStart(); else serverStop();
    }
    if (_serverUp) {
        _server.handleClient();
    }

    // Отложенный reboot (ack уже отправлен клиенту)
    if (_restartAtMs != 0 && (int32_t)(millis() - _restartAtMs) >= 0) {
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), "web");
        postEvent(SH_EVENT_SHUTDOWN, &d);
        delay(100);
        ESP.restart();
    }
}

void HttpService::serverStart() {
    registerRoutes();
    _server.begin();
    _serverUp = true;
    log(LogLevel::Info, "HTTP server started%s",
        Kernel::getInstance().isSafeMode() ? " (SAFE MODE recovery)" : "");
}

void HttpService::serverStop() {
    if (!_serverUp) return;
    _server.stop();   // close() клиентов — WebServer закрывает сам
    _serverUp = false;
}

// ============================================================================
// МАРШРУТЫ
// ============================================================================
void HttpService::registerRoutes() {
    // Публичная часть
    _server.on("/",               HTTP_GET,  [this]() { handleRoot(); });
    _server.on("/api/system",     HTTP_GET,  [this]() { handleApiSystem(); });
    _server.on("/api/auth",       HTTP_POST, [this]() { handleApiAuth(); });
    _server.on("/api/setup",      HTTP_POST, [this]() { handleApiSetup(); });
    // Админская часть (каждый обработчик начинается с requireAdmin)
    _server.on("/admin",          HTTP_GET,  [this]() { handleAdmin(); });
    _server.on("/api/logout",     HTTP_POST, [this]() { handleApiLogout(); });
    _server.on("/api/telemetry",  HTTP_GET,  [this]() { handleApiTelemetry(); });
    _server.on("/api/config",     HTTP_GET,  [this]() { handleApiConfigGet(); });
    _server.on("/api/config",     HTTP_POST, [this]() { handleApiConfigSet(); });
    // NVS-бэкап конфигурации (5.0.9): переживает перепрошивку FS
    _server.on("/api/config/backup",  HTTP_GET,  [this]() { handleApiConfigBackupInfo(); });
    _server.on("/api/config/backup",  HTTP_POST, [this]() { handleApiConfigBackup(); });
    _server.on("/api/config/restore", HTTP_POST, [this]() { handleApiConfigRestore(); });
    _server.on("/api/logs",       HTTP_GET,  [this]() { handleApiLogs(); });
    _server.on("/api/audit",      HTTP_GET,  [this]() { handleApiAudit(); });
    _server.on("/api/reboot",     HTTP_POST, [this]() { handleApiReboot(); });
    // OTA (залежь №3): состояние, проверка манифеста, приём образа
    _server.on("/api/ota/info",   HTTP_GET,  [this]() { handleApiOtaInfo(); });
    _server.on("/api/ota/check",  HTTP_POST, [this]() { handleApiOtaCheck(); });
    _server.on("/api/ota/update", HTTP_POST, [this]() { handleApiOtaUpdate(); });
    _server.on("/api/ota/upload", HTTP_POST,
               [this]() { handleApiOtaUploadDone(); },
               [this]() { handleApiOtaUploadChunk(); });
    _server.on("/api/health",     HTTP_GET,  [this]() { handleApiHealth(); });
    _server.on("/api/auth/change",HTTP_POST, [this]() { handleApiAuthChange(); });
    _server.on("/api/time/sync",  HTTP_POST, [this]() { handleApiTimeSync(); });
    _server.on("/api/time/set",   HTTP_POST, [this]() { handleApiTimeSet(); });
    // Профильный API — префиксный маршрут (WebServer: onNotFound не годится,
    // используем on с префиксом через UriGlob? — нет: проверяем в notFound)
    _server.onNotFound([this]() {
        if (_server.uri().startsWith("/api/dev/")) handleApiDev();
        else if (_server.uri().startsWith("/web/")) handleWebFile();
        else handleNotFound();
    });
}

// ============================================================================
// СЕССИИ АДМИНИСТРАТОРА (токены, RAM-only)
// ============================================================================
bool HttpService::sessionValid(const char* tok) {
    if (tok == nullptr || strlen(tok) != HTTP_TOKEN_LEN - 1) return false;
    uint32_t now = millis();
    for (uint8_t i = 0; i < HTTP_TOKEN_SLOTS; ++i) {
        if (_sessions[i].token[0] != '\0' &&
            strcmp(_sessions[i].token, tok) == 0 &&
            (int32_t)(_sessions[i].expiresMs - now) > 0) {
            _sessions[i].expiresMs = now + HTTP_SESSION_MS;   // скольжение
            return true;
        }
    }
    return false;
}

bool HttpService::checkAdmin() {
    String tok;
    if (_server.hasHeader("X-Auth-Token")) {
        tok = _server.header("X-Auth-Token");
    } else if (_server.hasArg("token")) {
        tok = _server.arg("token");   // для <a href> выгрузок (аудит)
    }
    return sessionValid(tok.c_str());
}

bool HttpService::isAdminToken(const char* token) {
    return sessionValid(token);
}

const char* HttpService::uiArgTrampoline(void* ctx, const char* name) {
    HttpService* self = static_cast<HttpService*>(ctx);
    if (!self->_server.hasArg(name)) return nullptr;
    String v = self->_server.arg(name);
    char* slot = self->_argRing[self->_argRingPos];
    self->_argRingPos = (self->_argRingPos + 1) % HTTP_ARG_RING;
    safeStrCopy(slot, sizeof(self->_argRing[0]), v.c_str());
    return slot;
}

const char* HttpService::issueToken() {
    // Свободный/просроченный слот; если нет — самый старый (LRU)
    uint32_t now = millis();
    Session* slot = &_sessions[0];
    for (uint8_t i = 0; i < HTTP_TOKEN_SLOTS; ++i) {
        if (_sessions[i].token[0] == '\0' ||
            (int32_t)(_sessions[i].expiresMs - now) <= 0) {
            slot = &_sessions[i];
            break;
        }
        if ((int32_t)(_sessions[i].expiresMs - slot->expiresMs) < 0) {
            slot = &_sessions[i];
        }
    }
    snprintf(slot->token, sizeof(slot->token), "%08lx",
             (unsigned long)esp_random());
    slot->expiresMs = now + HTTP_SESSION_MS;
    return slot->token;
}

void HttpService::dropToken(const char* token) {
    for (uint8_t i = 0; i < HTTP_TOKEN_SLOTS; ++i) {
        if (strcmp(_sessions[i].token, token) == 0) {
            _sessions[i].token[0] = '\0';
            return;
        }
    }
}

// ============================================================================
// УТИЛИТЫ ОТВЕТОВ
// ============================================================================
void HttpService::sendJson(int code, const char* json) {
    // 5.8.0, HTTP-гард: закрытие соединения — явно (на ядре 3.3.11
    // WebServer и сам шлёт Connection: close; дублируем на случай ядер
    // с keep-alive: сокет — ~4 КБ lwIP, урок просадки heap 14.08).
    _server.sendHeader("Connection", "close");
    _server.send(code, "application/json; charset=utf-8", json);
}

// 5.8.0, «Скачать журнал»: потоковая отдача (см. HttpService.h)
bool HttpService::streamFileDownload(fs::File& f, const char* downloadName) {
    if (!f) return false;
    _server.sendHeader("Cache-Control", "no-store");
    _server.sendHeader("Content-Disposition",
                       String("attachment; filename=\"") +
                       (downloadName ? downloadName : "download.bin") + "\"");
    // streamFile: файл уходит кусками, целиком в heap НЕ поднимается
    size_t sent = _server.streamFile(f, "application/x-ndjson");
    return sent > 0;
}

bool HttpService::requireAdmin() {
    if (checkAdmin()) return true;
    sendJson(401, "{\"error\":\"auth_required\"}");
    return false;
}

// ============================================================================
// ПУБЛИЧНАЯ ЧАСТЬ
// ============================================================================
void HttpService::handleRoot() {
    NetworkService& net = NetworkService::getInstance();
    Kernel& k = Kernel::getInstance();

    // Уровень деградации — человеческим языком
    const char* levelRu;
    switch (net.degradationLevel()) {
        case DegradationLevel::Full:       levelRu = "Полный"; break;
        case DegradationLevel::LocalNet:   levelRu = "Локальная сеть"; break;
        default:                           levelRu = "Автономный"; break;
    }

    char ip[16];
    net.ipString(ip, sizeof(ip));

    // Время (если достоверно)
    char timeBuf[24] = "—";
    struct tm t;
    if (TimeService::getInstance().getLocalTime(t)) {
        snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    }

    // Секция профиля (инжекция) или пометка об её отсутствии
    char profile[2048];
    if (_ui != nullptr) {
        _ui->renderPublicHtml(profile, sizeof(profile));
    } else {
        safeStrCopy(profile, sizeof(profile),
                    k.isSafeMode()
                    ? "<p class='bad'><b>SAFE MODE</b>: профиль не загружен. "
                      "Диагностика и восстановление — в /admin.</p>"
                    : "<p>Профиль не предоставляет публичный интерфейс.</p>");
    }

    const char* title = (_ui != nullptr) ? _ui->uiTitle() : "МикроОС";

    snprintf(_pageBuf, sizeof(_pageBuf),
        "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"refresh\" content=\"15\">"
        "<title>%s</title><style>"
        "body{font-family:system-ui,Arial;background:#10151c;color:#dde;margin:0}"
        "header{background:#1b2530;padding:12px 16px;font-weight:600}"
        ".card{background:#1b2530;border-radius:10px;padding:14px 16px;margin:12px 16px}"
        "td{padding:3px 12px 3px 0;color:#aeb}.bad{color:#f88}"
        "a{color:#6af}footer{color:#678;text-align:center;padding:14px;font-size:13px}"
        "</style></head><body>"
        "<header>%s</header>"
        "<div class=\"card\"><table>"
        "<tr><td>Устройство</td><td>%s</td></tr>"
        "<tr><td>ID</td><td>%s</td></tr>"
        "<tr><td>IP</td><td>%s</td></tr>"
        "<tr><td>Режим сети</td><td>%s</td></tr>"
        "<tr><td>Время</td><td>%s</td></tr>"
        "<tr><td>Аптайм</td><td>%lu с</td></tr>"
        "</table></div>"
        "<div class=\"card\">%s</div>"
        "<footer>МикроОС 5.0 · <a href=\"/admin\">администрирование</a></footer>"
        "</body></html>",
        title, title,
        net.hostname(), net.deviceId(), ip, levelRu, timeBuf,
        (unsigned long)(millis() / 1000),
        profile);

    _server.send(200, "text/html; charset=utf-8", _pageBuf);
}

void HttpService::handleApiSystem() {
    NetworkService& net = NetworkService::getInstance();
    char ip[16];
    net.ipString(ip, sizeof(ip));
    snprintf(jsonBuf(), jsonBufSize(),
        "{\"id\":\"%s\",\"host\":\"%s\",\"ip\":\"%s\","
        "\"level\":%u,\"uptime\":%lu,\"setup\":%d,\"safe\":%d,"
        "\"version\":\"5.0.0\"}",
        net.deviceId(), net.hostname(), ip,
        (unsigned)net.degradationLevel(),
        (unsigned long)(millis() / 1000),
        AuthService::getInstance().isProvisioned() ? 0 : 1,
        Kernel::getInstance().isSafeMode() ? 1 : 0);
    sendJson(200, jsonBuf());
}

// ============================================================================
// АУТЕНТИФИКАЦИЯ (C1/C3)
// ============================================================================
void HttpService::handleApiAuth() {
    AuthService& auth = AuthService::getInstance();

    // Блокировка rate-limiter'а — честный ответ о сроке
    if (auth.isRateLimited("admin")) {
        snprintf(jsonBuf(), jsonBufSize(),
                 "{\"error\":\"locked\",\"sec\":%lu}",
                 (unsigned long)auth.rateLimitRemainingSec("admin"));
        sendJson(401, jsonBuf());
        return;
    }
    if (!_server.hasArg("pin") ||
        !auth.verifyAdminPin(_server.arg("pin").c_str(), "web")) {
        snprintf(jsonBuf(), jsonBufSize(),
                 "{\"error\":\"bad_pin\",\"locked\":%d}",
                 auth.isRateLimited("admin") ? 1 : 0);
        sendJson(401, jsonBuf());
        return;
    }
    snprintf(jsonBuf(), jsonBufSize(), "{\"token\":\"%s\"}", issueToken());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiSetup() {
    AuthService& auth = AuthService::getInstance();
    // C1: мастер жив только до первого ПИНа — дальше дверь закрыта
    if (auth.isProvisioned()) {
        sendJson(403, "{\"error\":\"already_provisioned\"}");
        return;
    }
    if (!_server.hasArg("pin") ||
        !auth.setAdminPin(_server.arg("pin").c_str())) {
        sendJson(400, "{\"error\":\"invalid_pin_format\"}");
        return;
    }
    // Provisioning завершён — сразу выдаём сессию владельцу
    snprintf(jsonBuf(), jsonBufSize(),
             "{\"token\":\"%s\",\"provisioned\":1}", issueToken());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiLogout() {
    if (_server.hasHeader("X-Auth-Token")) {
        String tok = _server.header("X-Auth-Token");
        dropToken(tok.c_str());
    }
    sendJson(200, "{\"ok\":1}");
}

// ============================================================================
// АДМИНСКАЯ ЧАСТЬ: API
// ============================================================================
void HttpService::handleApiTelemetry() {
    if (!requireAdmin()) return;
    TelemetryService::getInstance().toJson(jsonBuf(), jsonBufSize());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiConfigGet() {
    if (!requireAdmin()) return;
    // SECRET-поля исключены самим ConfigService — устройство их не отдаёт
    ConfigService::getInstance().toJson(jsonBuf(), jsonBufSize());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiConfigSet() {
    if (!requireAdmin()) return;
    if (!_server.hasArg("key") || !_server.hasArg("value")) {
        sendJson(400, "{\"error\":\"bad_request\"}");
        return;
    }
    bool ok = ConfigService::getInstance().set(
        _server.arg("key").c_str(), _server.arg("value").c_str());
    snprintf(jsonBuf(), jsonBufSize(), "{\"%s\":\"%s\"}",
             _server.arg("key").c_str(), ok ? "ok" : "rejected");
    sendJson(ok ? 200 : 400, jsonBuf());
}

// ============================================================================
// NVS-БЭКАП КОНФИГУРАЦИИ (5.0.9): «Вжжух, вжжух — и готово» после перепрошивки
// ============================================================================
void HttpService::handleApiConfigBackupInfo() {
    if (!requireAdmin()) return;
    ConfigService::getInstance().backupInfoJson(jsonBuf(), jsonBufSize());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiConfigBackup() {
    if (!requireAdmin()) return;
    size_t size = 0;
    bool ok = ConfigService::getInstance().backupToNvs(
        (uint32_t)TimeService::getInstance().getUnixTime(),
        UpdateService::getInstance().firmwareVersion(), &size);
    if (!ok) {
        sendJson(500, "{\"error\":\"backup_failed\"}");
        return;
    }
    snprintf(jsonBuf(), jsonBufSize(), "{\"ok\":1,\"size\":%u}",
             (unsigned)size);
    sendJson(200, jsonBuf());
}

void HttpService::handleApiConfigRestore() {
    if (!requireAdmin()) return;
    // БЕЗ подтверждений и предупреждений — осознанное действие оператора
    // (явный заказ UX). Ребут обязателен: половина полей CFG_CRITICAL,
    // применение конфигурации — при загрузке.
    int applied = ConfigService::getInstance().restoreFromNvs();
    if (applied < 0) {
        sendJson(404, "{\"error\":\"backup_not_found\"}");
        return;
    }
    if (applied == 0) {
        sendJson(422, "{\"error\":\"backup_corrupt\"}");
        return;
    }
    snprintf(jsonBuf(), jsonBufSize(),
             "{\"ok\":1,\"applied\":%d,\"reboot_in_ms\":1500}", applied);
    sendJson(200, jsonBuf());
    _restartAtMs = millis() + HTTP_RESTART_DELAY_MS;
}

void HttpService::handleApiLogs() {
    if (!requireAdmin()) return;
    LogService::getInstance().tail(jsonBuf(), jsonBufSize(), 40);
    _server.send(200, "text/plain; charset=utf-8", jsonBuf());
}

void HttpService::handleApiAudit() {
    if (!requireAdmin()) return;
    File f = StorageService::getInstance().openRead(AUDIT_FILE_PATH);
    if (!f) {
        sendJson(404, "{\"error\":\"no_audit_file\"}");
        return;
    }
    _server.streamFile(f, "application/json-lines");
    f.close();
}

// ============================================================================
// СТАТИКА ПРОФИЛЯ (/web/* → LittleFS /web/*)
// ============================================================================
// Профильные веб-ресурсы (богатые страницы, иконки, JS) живут в файловой
// системе — PROGMEM дорог, а страница СКУД ~30 КБ. Контент сам по себе не
// секретен: данные и операции идут через API с авторизацией.
void HttpService::handleWebFile() {
    String uri = _server.uri();
    // Траверсал запрещён категорически
    if (uri.indexOf("..") >= 0) {
        sendJson(400, "{\"error\":\"bad_path\"}");
        return;
    }
    // MIME по расширению (минимум для панели профиля)
    const char* mime = "application/octet-stream";
    if (uri.endsWith(".html")) mime = "text/html; charset=utf-8";
    else if (uri.endsWith(".css"))  mime = "text/css; charset=utf-8";
    else if (uri.endsWith(".js"))   mime = "application/javascript";
    else if (uri.endsWith(".json")) mime = "application/json";
    else if (uri.endsWith(".png"))  mime = "image/png";
    else if (uri.endsWith(".svg"))  mime = "image/svg+xml";
    else if (uri.endsWith(".ico"))  mime = "image/x-icon";

    // Экономия FS (практика монолита v2.5): если рядом лежит <file>.gz —
    // отдаём сжатый вариант с Content-Encoding: gzip. Потребитель статики —
    // браузер, gzip поддерживают все; lock.html 37 КБ → ~10 КБ в образе.
    // ВАЖНО: заголовок Content-Encoding: gzip НЕ отправляем вручную —
    // WebServer ядра ESP32 3.x сам добавляет его в _streamFileCore() для
    // файлов *.gz (WebServer.cpp). Ручной sendHeader давал заголовок ДВАЖДЫ,
    // браузер распаковывал gzip дважды и падал с ERR_CONTENT_DECODING_FAILED.
    String gzUri = uri + ".gz";
    File fz = StorageService::getInstance().openRead(gzUri.c_str());
    if (fz) {
        _server.sendHeader("Vary", "Accept-Encoding");
        // Панель — разработческая итерация: heuristic caching браузеров
        // уже ловил нас устаревшими ответами — запрещаем кэш насовсем.
        _server.sendHeader("Cache-Control", "no-cache");
        _server.streamFile(fz, mime);
        fz.close();
        return;
    }

    File f = StorageService::getInstance().openRead(uri.c_str());
    if (!f) {
        sendJson(404, "{\"error\":\"not_found\"}");
        return;
    }
    _server.sendHeader("Cache-Control", "no-cache");
    _server.streamFile(f, mime);
    f.close();
}

void HttpService::handleApiReboot() {
    if (!requireAdmin()) return;
    sendJson(200, "{\"ok\":1,\"reboot_in_ms\":1500}");
    _restartAtMs = millis() + HTTP_RESTART_DELAY_MS;
}

// ============================================================================
// OTA (залежь №3): состояние / манифест / приём образа из панели
// ============================================================================
void HttpService::handleApiOtaInfo() {
    if (!requireAdmin()) return;
    UpdateService::getInstance().otaInfoJson(jsonBuf(), jsonBufSize());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiOtaCheck() {
    if (!requireAdmin()) return;
    // Блокирует loop до 4 с (GET version.json с HA) — кнопка админа, редкая;
    // WDT 10 с не достаёт, но телеметрия может отметить tick-overrun.
    UpdateService::getInstance().checkRemote();
    // Ответ всегда 200 с полным состоянием: «не удалось получить манифест»
    // — тоже валидный исход, панель раскрасит сама.
    UpdateService::getInstance().otaInfoJson(jsonBuf(), jsonBufSize());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiOtaUpdate() {
    if (!requireAdmin()) return;
    // Phase 4: запуск фоновой загрузки — сама работа в UpdateService::tick
    // (кооперативно), здесь только постановка в очередь и ответ «поехали».
    UpdateService& ota = UpdateService::getInstance();
    if (ota.uploadActive()) {
        sendJson(409, "{\"error\":\"upload_active\"}");
        return;
    }
    if (ota.downloadActive()) {
        sendJson(409, "{\"error\":\"already_running\"}");
        return;
    }
    // type=fw — только прошивка; без аргумента — полное (fw + ФС, если
    // в манифесте есть fs_url). Ребут после записи — сам, из tick.
    bool withFs = !_server.hasArg("type") || _server.arg("type") != "fw";
    ota.requestRemoteUpdate(withFs);
    sendJson(200, "{\"ok\":1,\"started\":1}");
}

void HttpService::handleApiOtaUploadChunk() {
    HTTPUpload& up = _server.upload();
    UpdateService& ota = UpdateService::getInstance();

    if (up.status == UPLOAD_FILE_START) {
        // Авторизация ДО первого байта; 401 отправит финиш-обработчик
        // (если ответить здесь, тело multipart задушит ответ).
        _otaAuthFail = !checkAdmin();
        _otaRxFailed = false;
        _otaRxOk = false;
        if (_otaAuthFail) return;
        // ?type=fs -> файловая система (U_SPIFFS), иначе прошивка (U_FLASH).
        // Content-Length включает multipart-обвязку — размер неизвестен.
        int cmd = (_server.arg("type") == "fs") ? U_SPIFFS : U_FLASH;
        if (!ota.uploadBegin(cmd, 0)) _otaRxFailed = true;
        log(LogLevel::Info, "OTA upload start: %s (%s)",
            up.filename.c_str(), cmd == U_SPIFFS ? "fs" : "fw");
        return;
    }
    if (_otaAuthFail || _otaRxFailed) return;   // глотаем, не пишем

    if (up.status == UPLOAD_FILE_WRITE) {
        if (!ota.uploadWrite(up.buf, up.currentSize)) _otaRxFailed = true;
    } else if (up.status == UPLOAD_FILE_END) {
        _otaRxOk = ota.uploadEnd();
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        ota.uploadAbort();
        _otaRxOk = false;
    }
}

void HttpService::handleApiOtaUploadDone() {
    if (_otaAuthFail) {
        _otaAuthFail = false;
        sendJson(401, "{\"error\":\"auth_required\"}");
        return;
    }
    if (_otaRxOk) {
        // Образ цел и записан. A/B-валидация после ребута — штатная
        // (UpdateService::init/tick): 60 с стабильной работы = VALID,
        // падение до валидации = загрузчик сам вернёт старый раздел.
        // Версия из app descriptor — панель покажет, ЧТО именно залили
        // (урок 5.0.7: рассинхрон прошивка/FS от «не того» .bin).
        snprintf(jsonBuf(), jsonBufSize(),
                 "{\"ok\":1,\"reboot_in_ms\":1500,\"ver\":\"%s\"}",
                 UpdateService::getInstance().lastUploadedVersion());
        sendJson(200, jsonBuf());
        _restartAtMs = millis() + HTTP_RESTART_DELAY_MS;
    } else {
        _otaRxFailed = false;
        // Причина — из UpdateService (bad magic / write / end): панель
        // покажет осмысленный текст вместо голого HTTP 500.
        UpdateService::getInstance().otaInfoJson(jsonBuf(), jsonBufSize());
        sendJson(500, jsonBuf());
    }
}

void HttpService::handleApiHealth() {
    if (!requireAdmin()) return;
    HealthMonitor::getInstance().reportJson(jsonBuf(), jsonBufSize());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiAuthChange() {
    // Смена пароля: действующий пароль — сама авторизация (перебор старого
    // ограничен тем же rate-limiter'ом, что и вход; setAdminPin внутри).
    if (!_server.hasArg("old") || !_server.hasArg("new")) {
        sendJson(400, "{\"error\":\"old_new_required\"}");
        return;
    }
    bool ok = AuthService::getInstance().setAdminPin(
        _server.arg("new").c_str(), _server.arg("old").c_str());
    if (!ok) {
        sendJson(403, "{\"error\":\"change_failed\"}");
        return;
    }
    // Пароль сменился — выдаём свежую сессию владельцу нового пароля
    snprintf(jsonBuf(), jsonBufSize(),
             "{\"ok\":1,\"token\":\"%s\"}", issueToken());
    sendJson(200, jsonBuf());
}

void HttpService::handleApiTimeSync() {
    if (!requireAdmin()) return;
    // Асинхронно: SNTP-ответ придёт позже, факт зафиксирует TimeService
    bool ok = TimeService::getInstance().forceNtpSync();
    snprintf(jsonBuf(), jsonBufSize(),
             "{\"ok\":%d,\"async\":1}", ok ? 1 : 0);
    sendJson(ok ? 200 : 400, jsonBuf());
}

void HttpService::handleApiTimeSet() {
    if (!requireAdmin()) return;
    // Время от браузера (unix, UTC): для изолированных сетей, где SNTP до
    // pool.ntp.org не достучаться. Точности браузера СКУД избыточна —
    // временным ключам нужны минуты, не миллисекунды. Отбраковка мусора:
    // не раньше 2026-01-01 и не дальше +1 суток от грубого здравого смысла.
    String u = _server.arg("unix");
    long sec = u.length() > 0 ? u.toInt() : 0;
    bool ok = sec > 1767225600L &&
              TimeService::getInstance().setSystemTime((time_t)sec);
    // Кнопка «Синхр. время» — осознанное действие админа: вместе с unix
    // браузер присылает свой часовой пояс (&tz=). Пишем его в sys.tz_offset,
    // иначе getLocalTime продолжит считать расписания в старом поясе
    // (урок 5.0.x: у пользователя браузер +7, в конфиге стояло +3).
    String tz = _server.arg("tz");
    if (ok && tz.length() > 0) {
        int off = tz.toInt();
        if (off >= -12 && off <= 12) {
            // set() сам валидирует диапазон и планирует сохранение (debounce)
            ConfigService::getInstance().set("sys.tz_offset",
                                             String(off).c_str());
        }
    }
    snprintf(jsonBuf(), jsonBufSize(), "{\"ok\":%d}", ok ? 1 : 0);
    sendJson(ok ? 200 : 400, jsonBuf());
}

// ============================================================================
// ПРОФИЛЬНЫЙ API (инжекция)
// ============================================================================
void HttpService::handleApiDev() {
    if (_ui == nullptr) {
        sendJson(404, "{\"error\":\"no_profile_ui\"}");
        return;
    }
    // Токен запроса доступен провайдеру (профильный ПИН — его политика)
    if (_server.hasHeader("X-Auth-Token")) {
        safeStrCopy(_currentToken, sizeof(_currentToken),
                    _server.header("X-Auth-Token").c_str());
    } else {
        _currentToken[0] = '\0';
    }

    // Контекст запроса целиком: метод, токен, аргументы (CRUD профиля)
    ShUiRequest req;
    req.method = (_server.method() == HTTP_POST) ? "POST" :
                 (_server.method() == HTTP_GET)  ? "GET"  : "OTHER";
    req.token  = _currentToken;
    req.arg    = &HttpService::uiArgTrampoline;
    req.argCtx = this;

    int status = 200;
    // 5.8.3: uri() возвращает String ПО ЗНАЧЕНИЮ — временный объект
    // разрушается на точке с запятой, и tail из временного c_str()
    // становился висячим указателем. Чтение внутри handleApi ловило
    // уже затёртый heap (16-байтная корзина /api/dev/dlog сносилась
    // первой → 404 unknown_dev_api при живых dlog/channels).
    // Держим именованную копию, пока работает handleApi.
    String uri = _server.uri();
    const char* tail = uri.c_str() + strlen("/api/dev/");
    bool handled = _ui->handleApi(tail, req, jsonBuf(), jsonBufSize(),
                                  status);
    if (!handled) {
        sendJson(404, "{\"error\":\"unknown_dev_api\"}");
        return;
    }
    // 5.8.0: status==0 — провайдер уже отправил ответ сам (потоковая
    // отдача файла через streamFileDownload). JSON поверх потока — порча
    // обоих, поэтому молчим.
    if (status == 0) return;
    sendJson(status, jsonBuf());
}

void HttpService::handleNotFound() {
    snprintf(jsonBuf(), jsonBufSize(), "{\"error\":\"not_found\",\"uri\":\"%s\"}",
             _server.uri().c_str());
    sendJson(404, jsonBuf());
}

// ============================================================================
// СТРАНИЦА АДМИНИСТРИРОВАНИЯ (PROGMEM, статика)
// ============================================================================
// Модель безопасности: HTML отдаётся любому, но это пустая оболочка —
// ВСЕ данные идут через /api/* с проверкой токена. Без токена страница
// показывает форму входа (или мастер provisioning'а при setup=1).
// JS — ванильный, без внешних зависимостей (устройство может жить в
// изолированном сегменте: никаких CDN).
// ============================================================================
static const char ADMIN_PAGE[] PROGMEM = R"ADM(<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>МикроОС 5.0 — администрирование</title>
<style>
body{font-family:system-ui,Arial;background:#10151c;color:#dde;margin:0}
header{background:#1b2530;padding:12px 16px;font-weight:600;display:flex;justify-content:space-between}
.card{background:#1b2530;border-radius:10px;padding:14px 16px;margin:12px 16px}
button{background:#2d6cdf;color:#fff;border:0;border-radius:6px;padding:8px 14px;cursor:pointer}
button.sec{background:#37424e}
nav{display:flex;gap:6px;padding:10px 16px;flex-wrap:wrap}
nav button{background:#26313d}nav button.on{background:#2d6cdf}
main{padding:0 16px 30px}
input{background:#0d1218;border:1px solid #37424e;color:#dde;border-radius:6px;padding:7px 9px}
table{border-collapse:collapse}td{padding:3px 12px 3px 0;color:#aeb}
pre{background:#0d1218;padding:10px;border-radius:8px;overflow:auto;font-size:12px;max-height:55vh}
.g{color:#8fd;font-weight:600;margin:14px 0 4px}
.r{display:flex;gap:8px;align-items:center;margin:4px 0;flex-wrap:wrap}
.r label{flex:1;min-width:180px}
.ok{color:#8d8}.bad{color:#f88}.dim{color:#789;font-size:12px}
a{color:#6af}
</style></head><body>
<header><span>МикроОС 5.0</span><span id="hn">…</span></header>
<div id="a"></div>
<script>
const A=document.getElementById('a');
let T=localStorage.mos_t||'';
async function api(m,u,b){
  const o={method:m,headers:{}};
  if(T)o.headers['X-Auth-Token']=T;
  if(b){o.headers['Content-Type']='application/x-www-form-urlencoded';o.body=b}
  const r=await fetch(u,o);
  if(r.status==401&&u!='/api/auth'){T='';localStorage.removeItem('mos_t');login();throw 401}
  return r}
const H=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
async function boot(){
  const s=await(await fetch('/api/system')).json();
  document.getElementById('hn').textContent=s.host+' · '+s.ip+(s.safe?' · SAFE MODE':'');
  if(s.setup)return setup();
  if(T){try{await api('GET','/api/telemetry');return tabs()}catch(e){return}}
  login()}
function setup(){
  A.innerHTML=`<div class=card><h3>Первичная настройка</h3>
  <p>Устройство новое: пароль администратора не задан (backdoor отсутствует по построению).
  Задайте пароль владельца — 4–32 символа (буквы, цифры, знаки, без пробелов):</p>
  <div class=r><input id=p type=password maxlength=32>
  <button onclick=doSetup()>Задать пароль</button></div><p id=m class=bad></p></div>`}
async function doSetup(){
  const p=document.getElementById('p').value;
  const r=await fetch('/api/setup',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pin='+p});
  const j=await r.json();
  if(j.token){T=j.token;localStorage.mos_t=T;tabs()}
  else document.getElementById('m').textContent='Пароль: 4–32 печатных символа без пробелов'}
function login(){
  A.innerHTML=`<div class=card><h3>Вход администратора</h3>
  <div class=r><input id=p type=password maxlength=32 placeholder="Пароль">
  <button onclick=doLogin()>Войти</button></div><p id=m class=bad></p></div>`;
  document.getElementById('p').addEventListener('keydown',e=>{if(e.key=='Enter')doLogin()})}
async function doLogin(){
  const p=document.getElementById('p').value;
  const r=await fetch('/api/auth',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pin='+p});
  const j=await r.json();
  if(j.token){T=j.token;localStorage.mos_t=T;tabs()}
  else document.getElementById('m').textContent=
    j.error=='locked'?('Перебор заблокирован, осталось '+j.sec+' с'):'Неверный пароль'}
function tabs(){
  A.innerHTML=`<nav>
  <button id=t0 onclick=tab(0)>Система</button>
  <button id=t1 onclick=tab(1)>Настройки</button>
  <button id=t2 onclick=tab(2)>Журнал</button>
  <button id=t3 onclick=tab(3)>Аудит</button>
  <button id=t4 onclick=tab(4)>Обновление</button>
  <button class=sec onclick=out()>Выход</button></nav><main id=c></main>`;
  tab(0)}
async function out(){await api('POST','/api/logout');T='';localStorage.removeItem('mos_t');login()}
async function tab(i){
  for(let k=0;k<5;k++)document.getElementById('t'+k).className=k==i?'on':'';
  const c=document.getElementById('c');c.innerHTML='<div class=card>Загрузка…</div>';
  await [sys_,cfg,log_,aud,ota][i](c)}
async function sys_(c){
  const j=await(await api('GET','/api/telemetry')).json();
  c.innerHTML=`<div class=card><h3>Состояние системы</h3><table>
  <tr><td>Аптайм</td><td>${j.uptime} с</td></tr>
  <tr><td>Heap</td><td>${j.heap} (мин ${j.heap_min})</td></tr>
  <tr><td>Шина: потеряно / пик очереди</td><td>${j.bus_dropped} / ${j.bus_hwm}</td></tr>
  <tr><td>Температура CPU</td><td>${j.cpu_t} °C</td></tr>
  <tr><td>Режим сети</td><td>${['Полный','Локальная сеть','Автономный'][j.degradation]}</td></tr>
  <tr><td>RTT шлюза</td><td>${j.gw_rtt} мс</td></tr>
  <tr><td>Bootloop-счётчик</td><td>${j.bootloop}</td></tr>
  <tr><td>Потери очереди аудита</td><td>${j.audit_lost}</td></tr></table>
  <p><button class=sec onclick=reb()>Перезагрузить</button></p></div>`}
async function reb(){if(confirm('Перезагрузить устройство?'))await api('POST','/api/reboot')}
async function cfg(c){
  const j=await(await api('GET','/api/config')).json();
  const ks=Object.keys(j).sort((a,b)=>j[a].group.localeCompare(j[b].group,'ru')||a.localeCompare(b));
  let g='',h='';
  for(const k of ks){const f=j[k];
    if(f.group!=g){g=f.group;h+=`<div class=g>${H(g)}</div>`}
    let w=f.secret
      ?`<input type=password id="f_${k}" autocomplete=new-password placeholder="${
        f.set?'•••••••• — задан, введите новый':'не задан'}">`
      :f.t==3
      ?`<input type=checkbox id="f_${k}"${(f.value=='true'||f.value=='1')?' checked':''}${f.ro?' disabled':''}>`
      :`<input id="f_${k}" value="${H(f.value)}"${f.ro?' disabled':''}${
        f.t<=1?` type=number min=${f.min} max=${f.max}`:f.t==2?' type=number step=any':''}>`;
    h+=`<div class=r><label title="${k}">${H(f.label)}</label>${w}${
      f.ro?'':`<button onclick=sv('${k}',${f.t},${f.secret?1:0})>✓</button>`}</div>`}
  c.innerHTML=`<div class=card><h3>Настройки</h3>${h}<p id=cm></p>
  <p class=dim>Параметры сети и безопасности применяются после перезагрузки.</p></div>`}
async function sv(k,t,s){
  const el=document.getElementById('f_'+k);
  const v=t==3?(el.checked?'true':'false'):el.value;
  if(s&&!v){document.getElementById('cm').innerHTML=
    '<span class=dim>Пусто — секрет не изменён (введите значение для замены)</span>';return}
  const r=await api('POST','/api/config','key='+encodeURIComponent(k)+'&value='+encodeURIComponent(v));
  document.getElementById('cm').innerHTML=r.ok
    ?'<span class=ok>Сохранено: '+H(k)+'</span>'
    :'<span class=bad>Отклонено: '+H(k)+'</span>'}
async function log_(c){
  const t=await(await api('GET','/api/logs')).text();
  c.innerHTML=`<div class=card><h3>Журнал (последние записи)</h3><pre>${H(t)}</pre>
  <button onclick=tab(2)>Обновить</button></div>`}
async function aud(c){
  c.innerHTML=`<div class=card><h3>Аудит</h3>
  <p>Юридически значимые события (вход, доступ, настройки, OTA) — JSON-lines,
  переживает перезагрузку, ротация 3 поколения.</p>
  <p><a href="/api/audit?token=${T}">Скачать /audit.log</a></p></div>`}
async function ota(c){
  let j={};try{j=await(await api('GET','/api/ota/info')).json()}catch(e){}
  const rmt=j.remote||{};
  c.innerHTML=`<div class=card><h3>Обновление прошивки</h3><table>
  <tr><td>Текущая версия</td><td>${H(j.fw||'?')}${j.build?' · сборка '+H(j.build):''}${
    j.pending_verify===1?' · ⏳ идёт валидация':''}</td></tr>
  <tr><td>На сервере HA</td><td>${rmt.checked===1
    ?(rmt.version?H(rmt.version):'манифест недоступен'):'ещё не проверялась'}</td></tr>
  <tr><td>Манифест</td><td>${H(rmt.url||'')}</td></tr></table>
  <div class=r style="margin-top:8px"><button class=sec onclick=otaChk()>Проверить обновление</button>
  <button onclick=otaGo()>Обновить с сервера HA (прошивка + ФС)</button></div><p id=om></p>
  <p class=dim>Манифест: http://&lt;mqtt.host&gt;:8123/&lt;sys.ota_path&gt;/&lt;host&gt;/version.json.
  A/B-разделы: новая прошивка стартует в режиме проверки; при сбое —
  автоматический откат на предыдущую (A1). Ручная загрузка образов — через
  профильную панель (карточка OTA на вкладке «Система»).</p></div>`}
async function otaChk(){
  await api('POST','/api/ota/check','');
  ota(document.getElementById('c'))}
async function otaGo(){
  if(!confirm('Скачать прошивку (+ ФС) с сервера HA и обновиться? Устройство перезагрузится.'))return;
  const r=await api('POST','/api/ota/update','');
  const j=await r.json().catch(()=>({}));
  document.getElementById('om').innerHTML=(r.ok&&j.started)
    ?'<span class=ok>Загрузка образов запущена, устройство перезагрузится</span>'
    :'<span class=bad>Не запустилось: '+H(j.error||('HTTP '+r.status))+'</span>'}
boot();
</script></body></html>
)ADM";

// ============================================================================
// ОБРАБОТЧИК СТРАНИЦЫ АДМИНКИ
// ============================================================================
void HttpService::handleAdmin() {
    // Статика из PROGMEM; врата — на стороне API (токен), см. комментарий выше.
    _server.send_P(200, "text/html; charset=utf-8", ADMIN_PAGE);
}
