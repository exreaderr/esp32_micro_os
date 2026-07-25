// ============================================================================
// UserManager.cpp - РЕАЛИЗАЦИЯ БАЗОВОГО МЕНЕДЖЕРА ПОЛЬЗОВАТЕЛЕЙ v5.0
// ============================================================================
// Описание: Реализация легкой версии UserManager.
//           Полностью сохраняет логику USER_MANAGER_LIGHT из предыдущей версии.
// ============================================================================
#include "UserManager.h"
#include "core/AppCore.h"
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>
#include <esp_random.h>

// ============================================================================
// 1. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void UserManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void UserManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    AppCore::getInstance().logInfo("[Users] %s", msg);
}

void UserManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

bool UserManager::isValidPasswordHash(const char* hash) const {
    if (hash == nullptr) return false;
    size_t len = strlen(hash);
    if (len < 8 || len > 128) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit(hash[i])) return false;
    }
    return true;
}

bool UserManager::isValidCardId(const char* cardId) const {
    if (cardId == nullptr) return false;
    size_t len = strlen(cardId);
    if (len < 4 || len > 16) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit(cardId[i])) return false;
    }
    return true;
}

bool UserManager::isAuthBlocked() const {
    if (_authAttempts >= _maxAuthAttempts) {
        if (millis() - _lastAuthAttemptTime < _authBlockTimeMs) {
            return true;
        }
    }
    return false;
}

void UserManager::generateSecureToken(char* dest, size_t size) {
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

// ============================================================================
// 2. ИНИЦИАЛИЗАЦИЯ
// ============================================================================
bool UserManager::init(JsonVariantConst config) {
    if (_initialized) {
        AppCore::getInstance().logWarning("[Users] Already initialized");
        return true;
    }

    AppCore::getInstance().logInfo("[Users] Initializing User Manager (LIGHT mode)...");

    // --- 1. Чтение конфигурации ---
    if (config.containsKey("enable_guest")) {
        _enableGuest = config["enable_guest"].as<bool>();
    }

    if (config.containsKey("session_timeout")) {
        _sessionTimeoutMs = config["session_timeout"].as<uint32_t>();
        if (_sessionTimeoutMs < 60000) _sessionTimeoutMs = 60000; // минимум 1 минута
    }

    if (config.containsKey("max_auth_attempts")) {
        _maxAuthAttempts = config["max_auth_attempts"].as<uint32_t>();
        if (_maxAuthAttempts < 1) _maxAuthAttempts = 1;
    }

    if (config.containsKey("auth_block_time")) {
        _authBlockTimeMs = config["auth_block_time"].as<uint32_t>();
        if (_authBlockTimeMs < 60000) _authBlockTimeMs = 60000;
    }

    // --- 2. Очистка данных ---
    memset(&_admin, 0, sizeof(_admin));
    memset(&_adminStats, 0, sizeof(_adminStats));
    _adminAuthenticated = false;
    _authAttempts = 0;
    _lastAuthAttemptTime = 0;
    _sessionId = 0;
    _totalLogins = 0;
    _totalFailedLogins = 0;
    _totalPasswordChanges = 0;

    // --- 3. Загрузка из NVS ---
    loadAdminFromNVS();

    _initialized = true;
    _ready = true;

    AppCore::getInstance().logInfo("[Users] User Manager initialized");
    AppCore::getInstance().logDebug("[Users] Admin: %s, Guest: %s",
                                    hasAdmin() ? "EXISTS" : "NOT SET",
                                    _enableGuest ? "ENABLED" : "DISABLED");

    return true;
}

// ============================================================================
// 3. ПЕРИОДИЧЕСКИЙ ЦИКЛ
// ============================================================================
void UserManager::update() {
    if (!_initialized) return;

    // Проверка блокировки аутентификации
    if (_authAttempts >= _maxAuthAttempts) {
        if (millis() - _lastAuthAttemptTime > _authBlockTimeMs) {
            _authAttempts = 0;
            logMessage("Auth block released");
        }
    }
}

// ============================================================================
// 4. ОБРАБОТЧИК СОБЫТИЙ
// ============================================================================
void UserManager::handleEvent(const ShEventData& event) {
    if (!_initialized) return;

    // --- 1. Команда на вход администратора ---
    if (event.type == EVENT_CMD_EXECUTE) {
        if (event.payload.cmdData.command == CMD_LOGIN_ADMIN) {
            const char* passwordHash = event.payload.cmdData.payload;
            bool success = loginAdmin(passwordHash);

            // Отправляем ответ
            ShEventData response;
            response.type = EVENT_CMD_RESPONSE;
            response.senderId = _id;
            response.targetModuleId = event.senderId;
            response.payload.cmdData.command = CMD_LOGIN_ADMIN;
            response.payload.cmdData.value = success ? 1 : 0;
            safeStrCopy(response.payload.cmdData.payload,
                       success ? "OK" : "FAIL",
                       sizeof(response.payload.cmdData.payload));
            AppCore::getInstance().publishEvent(response);
            return;
        }

        if (event.payload.cmdData.command == CMD_LOGOUT_ADMIN) {
            logoutAdmin();
            return;
        }

        if (event.payload.cmdData.command == CMD_SET_ADMIN_PASSWORD) {
            const char* newHash = event.payload.cmdData.payload;
            bool success = setAdminPassword(newHash);

            ShEventData response;
            response.type = EVENT_CMD_RESPONSE;
            response.senderId = _id;
            response.targetModuleId = event.senderId;
            response.payload.cmdData.command = CMD_SET_ADMIN_PASSWORD;
            response.payload.cmdData.value = success ? 1 : 0;
            AppCore::getInstance().publishEvent(response);
            return;
        }

        if (event.payload.cmdData.command == CMD_CHECK_AUTH) {
            ShEventData response;
            response.type = EVENT_CMD_RESPONSE;
            response.senderId = _id;
            response.targetModuleId = event.senderId;
            response.payload.cmdData.command = CMD_CHECK_AUTH;
            response.payload.cmdData.value = _adminAuthenticated ? 1 : 0;
            safeStrCopy(response.payload.cmdData.payload,
                       _adminAuthenticated ? "authenticated" : "anonymous",
                       sizeof(response.payload.cmdData.payload));
            AppCore::getInstance().publishEvent(response);
            return;
        }
        return;
    }

    // --- 2. Перезагрузка модуля ---
    if (event.type == EVENT_SYS_RESTART && event.targetModuleId == _id) {
        logoutAdmin();
        return;
    }
}

// ============================================================================
// 5. АУТЕНТИФИКАЦИЯ
// ============================================================================
bool UserManager::loginAdmin(const char* passwordHash) {
    if (!hasAdmin()) {
        logMessage("Admin not configured!");
        publishErrorEvent("ADMIN_NOT_CONFIGURED");
        return false;
    }

    if (isAuthBlocked()) {
        logMessage("Auth blocked - too many attempts");
        publishErrorEvent("AUTH_BLOCKED");
        return false;
    }

    bool success = verifyAdminPassword(passwordHash);

    if (success) {
        _adminAuthenticated = true;
        _sessionId = esp_random();
        _authAttempts = 0;
        _adminStats.loginCount++;
        _adminStats.lastLoginTime = millis();
        _totalLogins++;
        logMessage("Admin logged in");
        publishAuthEvent(true, "admin");
        if (_onLoginCallback) _onLoginCallback(UserRole::ADMIN);
    } else {
        _authAttempts++;
        _lastAuthAttemptTime = millis();
        _adminStats.failedAttempts++;
        _totalFailedLogins++;
        logMessage("Admin login failed (attempt %d)", _authAttempts);
        publishAuthEvent(false, "admin");
        if (_authAttempts >= _maxAuthAttempts) {
            publishErrorEvent("AUTH_BLOCKED");
        }
    }

    return success;
}

bool UserManager::loginAdmin(const char* cardId, const char* pinHash) {
    if (!hasAdmin()) {
        logMessage("Admin not configured!");
        publishErrorEvent("ADMIN_NOT_CONFIGURED");
        return false;
    }

    if (isAuthBlocked()) {
        logMessage("Auth blocked - too many attempts");
        publishErrorEvent("AUTH_BLOCKED");
        return false;
    }

    bool success = verifyAdminCard(cardId);

    // Если есть PIN, проверяем его
    if (success && pinHash != nullptr && strlen(pinHash) > 0) {
        // В LIGHT режиме PIN проверяется через пароль
        success = verifyAdminPassword(pinHash);
    }

    if (success) {
        _adminAuthenticated = true;
        _sessionId = esp_random();
        _authAttempts = 0;
        _adminStats.loginCount++;
        _adminStats.lastLoginTime = millis();
        _totalLogins++;
        logMessage("Admin logged in with card");
        publishAuthEvent(true, "admin");
        if (_onLoginCallback) _onLoginCallback(UserRole::ADMIN);
    } else {
        _authAttempts++;
        _lastAuthAttemptTime = millis();
        _adminStats.failedAttempts++;
        _totalFailedLogins++;
        logMessage("Admin card login failed (attempt %d)", _authAttempts);
        publishAuthEvent(false, "admin");
        if (_authAttempts >= _maxAuthAttempts) {
            publishErrorEvent("AUTH_BLOCKED");
        }
    }

    return success;
}

void UserManager::logoutAdmin() {
    if (_adminAuthenticated) {
        _adminAuthenticated = false;
        _adminStats.lastLogoutTime = millis();
        logMessage("Admin logged out");
        publishAuthEvent(false, "admin", "logout");
        if (_onLogoutCallback) _onLogoutCallback();
    }
}

// ============================================================================
// 6. УПРАВЛЕНИЕ АДМИНИСТРАТОРОМ
// ============================================================================
bool UserManager::setAdminPassword(const char* passwordHash) {
    if (!isValidPasswordHash(passwordHash)) {
        logMessage("Invalid password hash");
        publishErrorEvent("INVALID_PASSWORD");
        return false;
    }

    safeStrCopy(_admin.passwordHash, sizeof(_admin.passwordHash), passwordHash);
    _admin.isActive = true;
    _admin.createdAt = millis() / 1000;
    _adminStats.hasPassword = true;
    _totalPasswordChanges++;

    saveAdminToNVS();
    logMessage("Admin password set");
    return true;
}

bool UserManager::setAdminCard(const char* cardId) {
    if (!isValidCardId(cardId)) {
        logMessage("Invalid card ID: %s", cardId);
        publishErrorEvent("INVALID_CARD");
        return false;
    }

    safeStrCopy(_admin.cardId, sizeof(_admin.cardId), cardId);
    _adminStats.hasCardId = true;

    saveAdminToNVS();
    logMessage("Admin card set: %s", cardId);
    return true;
}

void UserManager::setAdminTrack(uint8_t track) {
    _admin.track = constrain(track, 1, 99);
    saveAdminToNVS();
    logMessage("Admin track set: %d", _admin.track);
}

bool UserManager::hasAdmin() const {
    return _admin.isActive && strlen(_admin.passwordHash) > 0;
}

void UserManager::getAdminId(char* dest, size_t size) const {
    if (dest == nullptr || size == 0) return;
    dest[0] = '\0';

    if (hasAdmin() && strlen(_admin.cardId) > 0) {
        safeStrCopy(dest, size - 1, _admin.cardId);
    } else if (hasAdmin()) {
        safeStrCopy(dest, size - 1, "ADMIN");
    }
}

void UserManager::clearAdminData() {
    clearAdminNVS();
    memset(&_admin, 0, sizeof(_admin));
    memset(&_adminStats, 0, sizeof(_adminStats));
    _adminAuthenticated = false;
    _authAttempts = 0;
    _lastAuthAttemptTime = 0;
    logMessage("Admin data cleared");
}

// ============================================================================
// 7. РАБОТА С NVS
// ============================================================================
void UserManager::loadAdminFromNVS() {
    _pref.begin(_nvsNamespace, true);

    String storedHash = _pref.getString(_adminKey, "");
    if (storedHash.length() > 0) {
        safeStrCopy(_admin.passwordHash, sizeof(_admin.passwordHash), storedHash.c_str());
        _admin.isActive = true;
        _admin.createdAt = _pref.getUInt(_adminCreatedKey, 0);
        _adminStats.hasPassword = true;

        String cardId = _pref.getString(_adminCardKey, "");
        if (cardId.length() > 0) {
            safeStrCopy(_admin.cardId, sizeof(_admin.cardId), cardId.c_str());
            _adminStats.hasCardId = true;
        }

        _admin.track = _pref.getUChar(_adminTrackKey, 1);
        logMessage("Admin profile loaded from NVS");
    } else {
        logMessage("No admin configured. Set admin password first!");
    }

    _pref.end();
}

void UserManager::saveAdminToNVS() {
    _pref.begin(_nvsNamespace, false);

    if (strlen(_admin.passwordHash) > 0) {
        _pref.putString(_adminKey, _admin.passwordHash);
    }
    if (strlen(_admin.cardId) > 0) {
        _pref.putString(_adminCardKey, _admin.cardId);
    }
    if (_admin.createdAt > 0) {
        _pref.putUInt(_adminCreatedKey, _admin.createdAt);
    }
    _pref.putUChar(_adminTrackKey, _admin.track);

    _pref.end();
}

void UserManager::clearAdminNVS() {
    _pref.begin(_nvsNamespace, false);
    _pref.clear();
    _pref.end();
}

// ============================================================================
// 8. ВЕРИФИКАЦИЯ
// ============================================================================
bool UserManager::verifyAdminPassword(const char* passwordHash) const {
    if (!hasAdmin()) return false;
    if (passwordHash == nullptr) return false;
    return (strcasecmp(_admin.passwordHash, passwordHash) == 0);
}

bool UserManager::verifyAdminCard(const char* cardId) const {
    if (!hasAdmin()) return false;
    if (cardId == nullptr) return false;
    if (strlen(_admin.cardId) == 0) return false;
    return (strcasecmp(_admin.cardId, cardId) == 0);
}

// ============================================================================
// 9. ПУБЛИКАЦИЯ СОБЫТИЙ
// ============================================================================
void UserManager::publishAuthEvent(bool success, const char* role, const char* details) {
    ShEventData event;
    event.type = success ? EVENT_ACCESS_GRANTED : EVENT_ACCESS_DENIED;
    event.senderId = _id;
    event.targetModuleId = 0xFF;

    event.payload.cmdData.value = success ? 1 : 0;
    snprintf(event.payload.cmdData.payload,
             sizeof(event.payload.cmdData.payload),
             "role:%s%s",
             role ? role : "unknown",
             details ? details : "");

    AppCore::getInstance().publishEvent(event);
}

void UserManager::publishErrorEvent(const char* errorCode) {
    ShEventData event;
    event.type = EVENT_MODULE_ERROR;
    event.senderId = _id;
    event.targetModuleId = 0xFF;
    safeStrCopy(event.payload.statusStr, sizeof(event.payload.statusStr), errorCode);
    AppCore::getInstance().publishEvent(event);

    if (_onErrorCallback) {
        _onErrorCallback(errorCode);
    }
}

// ============================================================================
// 10. ДИАГНОСТИКА
// ============================================================================
const char* UserManager::getStatus() const {
    static char statusBuffer[64];

    snprintf(statusBuffer, sizeof(statusBuffer),
            "Admin: %s, Auth: %s, Attempts: %d, Guest: %s",
            hasAdmin() ? "YES" : "NO",
            _adminAuthenticated ? "YES" : "NO",
            _authAttempts,
            _enableGuest ? "YES" : "NO");

    return statusBuffer;
}

void UserManager::getDiagnostics(ShEventData& diagData) const {
    diagData.type = EVENT_LOG_MESSAGE;
    diagData.senderId = _id;
    diagData.targetModuleId = 0xFF;
    diagData.payload.logData.level = LOG_LEVEL_INFO;

    snprintf(diagData.payload.logData.msg,
             sizeof(diagData.payload.logData.msg),
             "Users: Admin=%d, Auth=%d, Logins=%lu, Fail=%lu, Guest=%d, Hash=%d, Card=%d",
             hasAdmin() ? 1 : 0,
             _adminAuthenticated ? 1 : 0,
             _totalLogins,
             _totalFailedLogins,
             _enableGuest ? 1 : 0,
             _adminStats.hasPassword ? 1 : 0,
             _adminStats.hasCardId ? 1 : 0);

    diagData.payload.logData.msg[
        sizeof(diagData.payload.logData.msg) - 1] = '\0';
}