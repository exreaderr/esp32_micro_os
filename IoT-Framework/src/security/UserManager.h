// ============================================================================
// UserManager.h - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Управление пользователями СКУД (карты, PIN-коды, права доступа).
// Режимы: LIGHT (все устройства, кроме СКУД) и FULL (СКУД)
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - УДАЛЕНА глобальная переменная USERS (используется синглтон)
// - ИСПРАВЛЕНА отправка событий (используется postEvent вместо Core.postEvent)
// - ИСПРАВЛЕН мьютекс (рекурсивный вместо обычного)
// - ИСПРАВЛЕНА работа с логами (используется LogManager)
// - ИСПРАВЛЕН portMAX_DELAY в конструкторе
// - ДОБАВЛЕНЫ недостающие константы
// - ДОБАВЛЕНА полная потокобезопасность
// - УЛУЧШЕНА работа с PIN-кодами
// ============================================================================
#pragma once

#ifndef USER_MANAGER_FULL
#define USER_MANAGER_FULL 1
#endif

#include <Arduino.h>
#include <vector>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

// Только IModule, без AppCore!
#include "core/IModule.h"

// ============================================================================
// 1. КОНСТАНТЫ
// ============================================================================
#define USER_MANAGER_MAX_USERS 100
#define USER_MANAGER_MAX_SESSIONS 10
#define USER_MANAGER_MAX_CHANGE_LOG 30
#define USER_MANAGER_MAX_AUTH_ATTEMPTS 5
#define USER_MANAGER_AUTH_BLOCK_TIME_MS 30000
#define USER_MANAGER_FLUSH_DELAY_MS 10000
#define USER_MANAGER_SESSION_TIMEOUT_MS 3600000
#define USER_MANAGER_NVS_NAMESPACE "usermgr"

// ============================================================================
// 2. ТИПЫ КЛЮЧЕЙ
// ============================================================================
enum KeyType : uint8_t {
    KEY_MASTER = 0,      // Мастер-ключ (администратор)
    KEY_PERMANENT = 1,   // Постоянный ключ
    KEY_TEMPORARY = 2,   // Временный ключ
    KEY_ONETIME = 3,     // Одноразовый ключ
    KEY_BLOCKED = 4,     // Заблокированный
    KEY_GUEST = 5        // Гостевой ключ
};

// ============================================================================
// 3. РЕЖИМЫ РАБОТЫ
// ============================================================================
enum class UserManagerMode : uint8_t {
    MODE_LIGHT = 0,      // Только Admin (все устройства, кроме СКУД)
    MODE_FULL = 1        // Полная база пользователей (СКУД)
};

// ============================================================================
// 4. СОБЫТИЯ USER MANAGER
// ============================================================================
enum UserEvents : int32_t {
    SH_EVENT_USER_ADDED = SH_EVENT_USER_BASE + 0x0900,
    SH_EVENT_USER_UPDATED = SH_EVENT_USER_BASE + 0x0901,
    SH_EVENT_USER_REMOVED = SH_EVENT_USER_BASE + 0x0902,
    SH_EVENT_USER_BLOCKED = SH_EVENT_USER_BASE + 0x0903,
    SH_EVENT_USER_UNBLOCKED = SH_EVENT_USER_BASE + 0x0904,
    SH_EVENT_USER_EXPIRED = SH_EVENT_USER_BASE + 0x0905,
    SH_EVENT_USER_USED = SH_EVENT_USER_BASE + 0x0906,
    SH_EVENT_USER_PIN_VERIFIED = SH_EVENT_USER_BASE + 0x0907,
    SH_EVENT_USER_PIN_FAILED = SH_EVENT_USER_BASE + 0x0908,
    SH_EVENT_USER_MASTER_CREATED = SH_EVENT_USER_BASE + 0x0909,
    SH_EVENT_USER_MASTER_DELETED = SH_EVENT_USER_BASE + 0x090A,
    SH_EVENT_USER_DB_CLEARED = SH_EVENT_USER_BASE + 0x090B,
    SH_EVENT_USER_LOGIN = SH_EVENT_USER_BASE + 0x090C,
    SH_EVENT_USER_ERROR = SH_EVENT_USER_BASE + 0x090D,
    SH_EVENT_ADMIN_LOGIN = SH_EVENT_USER_BASE + 0x090E,
    SH_EVENT_ADMIN_LOGOUT = SH_EVENT_USER_BASE + 0x090F,
    SH_EVENT_ADMIN_PASSWORD_CHANGED = SH_EVENT_USER_BASE + 0x0910,
    SH_EVENT_USER_SESSION_CREATED = SH_EVENT_USER_BASE + 0x0911,
    SH_EVENT_USER_SESSION_DESTROYED = SH_EVENT_USER_BASE + 0x0912,
    SH_EVENT_AUTH_FAILED = SH_EVENT_USER_BASE + 0x0913
};

// ============================================================================
// 5. СТРУКТУРЫ ДАННЫХ
// ============================================================================
#ifdef USER_MANAGER_FULL
/**
 * @brief Профиль пользователя (FULL режим)
 */
struct UserProfile {
    char id[9];           // 8 символов UID + '\0'
    char name[65];        // 64 символа + '\0'
    KeyType type;
    uint8_t track;        // 1-99 голосовой трек
    uint32_t expiry;      // Unix timestamp для временных ключей
    bool hasPassword;
    uint32_t createdAt;
    uint32_t lastUsed;
    uint32_t useCount;
    bool isActive;
    uint8_t accessLevel;
    char notes[32];
    uint8_t pinHash[32];  // SHA256 хеш PIN-кода
};

/**
 * @brief Статистика пользователей (FULL режим)
 */
struct UserStats {
    size_t total = 0;
    size_t master = 0;
    size_t permanent = 0;
    size_t temporary = 0;
    size_t oneTime = 0;
    size_t guest = 0;
    size_t blocked = 0;
    size_t withPin = 0;
    size_t expired = 0;
    size_t active = 0;
    size_t inactive = 0;
    uint32_t totalUses = 0;
    uint32_t mostUsedCount = 0;
    char mostUsedId[9] = {0};
};

/**
 * @brief Фильтр пользователей (FULL режим)
 */
struct UserFilter {
    KeyType type = KEY_PERMANENT;
    bool hasPin = false;
    bool isActive = true;
    bool expired = false;
    uint32_t minUses = 0;
    uint32_t maxUses = UINT32_MAX;
    char nameContains[32] = {0};
    uint32_t createdAfter = 0;
    uint32_t createdBefore = UINT32_MAX;
    uint32_t usedAfter = 0;
    uint32_t usedBefore = UINT32_MAX;
};

/**
 * @brief Журнал изменений (FULL режим)
 */
struct UserChangeLog {
    char cardId[9];
    char action[12];
    uint32_t timestamp;
    char details[32];
};
#endif // USER_MANAGER_FULL

/**
 * @brief Профиль администратора (LIGHT режим)
 */
struct AdminProfile {
    char passwordHash[65];
    char cardId[9];
    bool isActive;
    uint32_t createdAt;
    uint32_t lastUsed;
    uint32_t useCount;
    uint8_t track;
    uint8_t pinHash[32];
    bool hasPin;
};

/**
 * @brief Статистика администратора (LIGHT режим)
 */
struct AdminStats {
    uint32_t loginCount = 0;
    uint32_t failedAttempts = 0;
    uint32_t lastLoginTime = 0;
    uint32_t lastLogoutTime = 0;
    bool hasCardId = false;
    bool hasPassword = false;
};

/**
 * @brief Сессия пользователя
 */
struct UserSession {
    char token[64];
    char userId[9];
    char role[16];
    uint32_t createdAt;
    uint32_t lastActivityMs;
    bool valid;
};

/**
 * @brief Структура события аутентификации
 */
struct AuthEventData {
    char username[32];
    char role[16];
    bool success;
    uint32_t timestamp;
    char clientIp[16];
    uint32_t sessionId;
};

/**
 * @brief Структура события пользователя
 */
struct UserEventData {
    char cardId[9];
    char name[65];
    uint8_t type;
    uint32_t timestamp;
    bool success;
    uint32_t useCount;
    char details[32];
};

/**
 * @brief Структура события проверки PIN
 */
struct PinVerificationEvent {
    char cardId[9];
    bool success;
    uint32_t timestamp;
    char clientIp[16];
};

// ============================================================================
// 6. ОСНОВНОЙ КЛАСС
// ============================================================================
/**
 * @brief Менеджер пользователей
 *
 * Синглтон. Обеспечивает:
 * - Управление пользователями (FULL режим)
 * - Управление администратором (LIGHT режим)
 * - Аутентификацию по паролю и карте
 * - Сессии
 * - PIN-коды
 * - Мастер-ключ
 * - Полную потокобезопасность
 */
class UserManager : public IModule {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(bool authenticated, const char* role)> OnAuthCallback;
    typedef std::function<void(const char* userId, const char* name)> OnUserChangeCallback;
    typedef std::function<void(const UserStats& stats)> OnStatsUpdateCallback;
    typedef std::function<void(const char* userId, bool success)> OnPinVerificationCallback;

#ifdef USER_MANAGER_FULL
    typedef std::function<void(const UserProfile& user)> OnUserAddedCallback;
    typedef std::function<void(const UserProfile& oldUser, const UserProfile& newUser)> OnUserUpdatedCallback;
    typedef std::function<void(const char* cardId)> OnUserRemovedCallback;
    typedef std::function<void(const size_t count)> OnUserCountChangedCallback;
    typedef std::function<void(const char* cardId, uint32_t useCount)> OnUserUsedCallback;
#endif

    // === СИНГЛТОН ===
    static UserManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    UserManager();
    ~UserManager();

    // Запрещаем копирование
    UserManager(const UserManager&) = delete;
    UserManager& operator=(const UserManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "UserManager"; }
    const char* getVersion() const override { return "4.2.2"; }
    uint32_t getModuleId() const override { return MODULE_ID_USER; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized; }

    // === СТАТУС ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ИНИЦИАЛИЗАЦИЯ ===
    void begin(UserManagerMode mode = UserManagerMode::MODE_LIGHT);
    void end();
    void reset();

    // === РЕЖИМ ===
    UserManagerMode getMode() const { return _mode; }
    bool isFullMode() const { return _mode == UserManagerMode::MODE_FULL; }
    bool isLightMode() const { return _mode == UserManagerMode::MODE_LIGHT; }

    // === АДМИНИСТРИРОВАНИЕ ===
    bool loginAdmin(const char* passwordHash);
    bool loginAdmin(const char* cardId, const char* pinHash = nullptr);
    void logoutAdmin();
    bool setAdminPassword(const char* passwordHash);
    bool setAdminCard(const char* cardId);
    bool verifyAdminPassword(const char* passwordHash) const;
    bool verifyAdminCard(const char* cardId) const;
    bool hasAdmin() const;
    void getAdminId(char* dest, size_t size) const;
    bool isAdminAuthenticated() const { return _adminAuthenticated; }
    uint32_t getSessionId() const { return _sessionId; }

    // === СЕССИИ ===
    bool createSession(const char* userId, const char* role, char* token, size_t tokenSize);
    bool validateSession(const char* token);
    bool destroySession(const char* token);
    void cleanupSessions();

    // === ПОЛНОСТЬЮ FULL РЕЖИМ (СКУД) ===
#ifdef USER_MANAGER_FULL
    // CRUD операции
    bool addUser(const UserProfile& user, const char* pinHash = "");
    bool addUser(const char* cardId, const char* name, KeyType type = KEY_PERMANENT,
                uint8_t track = 1, uint32_t expiry = 0, const char* pinHash = "");
    bool updateUser(const char* cardId, const UserProfile& user, const char* pinHash = "");
    bool removeUser(const char* cardId);
    bool removeUserSilent(const char* cardId);
    bool blockUser(const char* cardId);
    bool unblockUser(const char* cardId);
    void clearAll();

    // Поиск
    int findIndex(const char* cardId) const;
    UserProfile* find(const char* cardId);
    const UserProfile* find(const char* cardId) const;
    bool exists(const char* cardId) const;
    bool isBlocked(const char* cardId) const;
    std::vector<UserProfile> findWithFilter(const UserFilter& filter) const;
    std::vector<UserProfile> findMostUsed(size_t limit = 10) const;

    // Использование
    bool recordUse(const char* cardId);

    // PIN-коды
    bool setPin(const char* cardId, const char* pinHash);
    bool removePin(const char* cardId);
    bool verifyPin(const char* cardId, const char* pinHash) const;
    bool hasPin(const char* cardId) const;
    bool isPinUnique(const char* pinHash, const char* excludeCard = "") const;
    bool isPinRequired(const char* cardId) const;

    // Мастер-ключ
    bool hasMasterKey() const;
    void getMasterKeyId(char* dest, size_t size) const;
    bool createMasterKey(const char* cardId);
    bool deleteMasterKey();
    bool isMasterKey(const char* cardId) const;

    // Управление
    void cleanupExpired(uint32_t currentTime);
    size_t cleanupExpiredCount(uint32_t currentTime);

    // Экспорт / Импорт
    void exportToStream(Stream& stream) const;
    bool importFromStream(Stream& stream);
    bool backupToNVS();
    bool restoreFromNVS();

    // Геттеры FULL
    const std::vector<UserProfile>& getAll() const { return _users; }
    size_t count() const { return _users.size(); }
    UserStats getUserStats() const;
    std::vector<UserChangeLog> getChangeLog(size_t limit = 30) const;

    // Колбэки FULL
    void setOnUserAdded(OnUserAddedCallback cb) { _onUserAdded = cb; }
    void setOnUserUpdated(OnUserUpdatedCallback cb) { _onUserUpdated = cb; }
    void setOnUserRemoved(OnUserRemovedCallback cb) { _onUserRemoved = cb; }
    void setOnUserCountChanged(OnUserCountChangedCallback cb) { _onUserCountChanged = cb; }
    void setOnUserUsed(OnUserUsedCallback cb) { _onUserUsed = cb; }
    void setOnPinVerification(OnPinVerificationCallback cb) { _onPinVerification = cb; }
    void setOnUserChange(OnUserChangeCallback cb) { _onUserChange = cb; }
#endif

    // === ОБЩИЕ КОЛБЭКИ ===
    void setOnAuth(OnAuthCallback cb) { _onAuth = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    bool isValidCardId(const char* cardId) const;
    bool isValidPasswordHash(const char* hash) const;
    bool isValidName(const char* name) const;
    bool isValidPinHash(const char* hash) const;
    void normalizeCardId(char* dest, const char* src) const;
    void generateSessionId();
    void clearPinCache();
    void saveToFile();
    bool loadFromFile();
    void markDirty();
    void handleBackgroundSave();
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    bool isInitializedAndReady() const;

#ifdef USER_MANAGER_FULL
    void updateUserStats();
    void addChangeLog(const char* cardId, const char* action, const char* details);
    const char* keyTypeToString(KeyType type) const;
    KeyType stringToKeyType(const char* typeStr) const;
#endif

    // === ОТПРАВКА СОБЫТИЙ ===
    void publishAuthEvent(const char* username, const char* role, bool success);
    void publishAdminEvent(int32_t eventId, bool success);
    void publishUserEvent(const char* cardId, const char* name, uint8_t type,
                         int32_t eventId, bool success, const char* details = nullptr);
    void publishPinEvent(const char* cardId, bool success);
    void publishErrorEvent(const char* errorCode);
    void publishStatsEvent();
    void publishSessionEvent(const char* token, bool created);

    // === ОБРАБОТЧИКИ ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ДАННЫЕ ===
    UserManagerMode _mode = UserManagerMode::MODE_LIGHT;

#ifdef USER_MANAGER_FULL
    // FULL режим: полная база
    std::vector<UserProfile> _users;
    std::vector<UserChangeLog> _changeLog;
    UserStats _stats;
    size_t MAX_USERS = USER_MANAGER_MAX_USERS;
    static constexpr size_t MAX_CHANGE_LOG = USER_MANAGER_MAX_CHANGE_LOG;
    static constexpr const char* _dbPath = "/users.json";
    static constexpr const char* _backupPath = "/users.bak";

    // Колбэки FULL
    OnUserAddedCallback _onUserAdded = nullptr;
    OnUserUpdatedCallback _onUserUpdated = nullptr;
    OnUserRemovedCallback _onUserRemoved = nullptr;
    OnUserCountChangedCallback _onUserCountChanged = nullptr;
    OnPinVerificationCallback _onPinVerification = nullptr;
    OnUserChangeCallback _onUserChange = nullptr;
    OnUserUsedCallback _onUserUsed = nullptr;
#endif

    // LIGHT режим: только Admin
    AdminProfile _admin;
    AdminStats _adminStats;

    // Сессии
    std::vector<UserSession> _sessions;
    size_t MAX_SESSIONS = USER_MANAGER_MAX_SESSIONS;
    uint32_t _sessionTimeoutMs = USER_MANAGER_SESSION_TIMEOUT_MS;

    // Константы для NVS
    static constexpr const char* _nvsNamespace = USER_MANAGER_NVS_NAMESPACE;
    static constexpr const char* _adminKey = "admin_hash";
    static constexpr const char* _adminCardKey = "admin_card";
    static constexpr const char* _adminTrackKey = "admin_track";
    static constexpr const char* _adminCreatedKey = "admin_created";
    static constexpr const char* _adminPinKey = "admin_pin";

    Preferences _pref;
    uint32_t _moduleId = MODULE_ID_USER;

    // Состояние
    bool _isDirty = false;
    bool _initialized = false;
    bool _adminAuthenticated = false;
    bool _initInProgress = false;
    uint32_t _dirtyTimestamp = 0;
    uint32_t _lastExpiryCheck = 0;
    uint32_t _lastStatsUpdate = 0;
    uint32_t _lastSessionCleanup = 0;
    uint32_t _sessionId = 0;
    uint32_t _lastAuthAttemptTime = 0;
    uint8_t _authAttempts = 0;

    // PIN кэш
    mutable char _pinCacheCardId[9] = {0};
    mutable char _pinCacheHash[65] = {0};
    mutable uint32_t _pinCacheTime = 0;

    // Колбэки
    OnAuthCallback _onAuth = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;

    // Мьютекс (РЕКУРСИВНЫЙ!)
    SemaphoreHandle_t _userMutex = nullptr;

    // Константы
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
    static constexpr uint32_t EXPIRY_CHECK_INTERVAL_MS = 600000;  // 10 минут
    static constexpr uint32_t STATS_UPDATE_INTERVAL_MS = 60000;   // 1 минута
    static constexpr uint32_t SESSION_CLEANUP_INTERVAL_MS = 60000; // 1 минута
};

// #endif // USERMANAGER_H