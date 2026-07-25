// ============================================================================
// AccessManager.h - ПОЛНОЕ УПРАВЛЕНИЕ ДОСТУПОМ ДЛЯ СКУД (FULL РЕЖИМ) v5.0
// ============================================================================
// Описание: Расширенный модуль управления доступом для контроллеров СКУД.
//           Наследует UserManager (легкая версия) и добавляет:
//           - Управление множеством пользователей (карты, PIN-коды)
//           - Временные и одноразовые ключи
//           - Мастер-ключ
//           - Журнал изменений
//           - Статистику использования
//           - Работу с JSON-базой через StorageServer
//
// АРХИТЕКТУРНЫЕ ПРИНЦИПЫ:
// 1. Полнота: все функции СКУД-контроллера из USER_MANAGER_FULL.
// 2. Наследование: использует базовую логику UserManager.
// 3. Изоляция: все операции через события.
// 4. Безопасность: хранение PIN-кодов в зашифрованном виде.
// ============================================================================
#pragma once

#include "security/UserManager.h"
#include <vector>
#include <map>
#include <functional>

// ============================================================================
// 1. ТИПЫ КЛЮЧЕЙ (ПОЛНАЯ ВЕРСИЯ)
// ============================================================================

/**
 * @brief Типы ключей/пользователей в FULL режиме
 */
enum class AccessUserType : uint8_t {
    NORMAL = 0,         // Обычный пользователь
    TEMPORARY = 1,      // Временный ключ (срок действия)
    MASTER = 2,         // Мастер-ключ (может добавлять/удалять)
    BLOCKED = 3,        // Заблокированный пользователь
    ADMIN = 4           // Администратор СКУД
};

// ============================================================================
// 2. СТРУКТУРЫ ДАННЫХ (FULL РЕЖИМ)
// ============================================================================

/**
 * @brief Профиль пользователя в FULL режиме
 */
struct AccessUserInfo {
    char id[16] = "";               // ID (обычно UID карты в HEX)
    char name[32] = "";             // Имя пользователя
    AccessUserType type = AccessUserType::NORMAL; // Тип ключа
    uint8_t track = 1;              // Трек-номер для озвучивания (1-99)
    uint32_t expiry = 0;            // Время истечения (Unix-время, 0 = бессрочно)
    uint32_t createdAt = 0;         // Время создания
    uint32_t lastUsed = 0;          // Время последнего использования
    uint32_t useCount = 0;          // Количество использований
    bool isActive = true;           // Активен ли пользователь
    uint8_t accessLevel = 0;        // Уровень доступа (0-255)
    bool hasPassword = false;       // Есть ли PIN-код
    char notes[64] = "";            // Дополнительные заметки
};

/**
 * @brief Статистика пользователей (FULL режим)
 */
struct UserStats {
    uint32_t total = 0;              // Общее количество пользователей
    uint32_t active = 0;             // Активные пользователи
    uint32_t inactive = 0;           // Неактивные пользователи
    uint32_t master = 0;             // Мастер-ключи
    uint32_t permanent = 0;          // Постоянные пользователи
    uint32_t temporary = 0;          // Временные ключи
    uint32_t oneTime = 0;            // Одноразовые ключи
    uint32_t guest = 0;              // Гостевые ключи
    uint32_t blocked = 0;            // Заблокированные пользователи
    uint32_t withPin = 0;            // С PIN-кодом
    uint32_t totalUses = 0;          // Общее количество использований
    char mostUsedId[16] = "";        // ID самого используемого ключа
    uint32_t mostUsedCount = 0;      // Количество использований самого используемого
};

/**
 * @brief Фильтр для поиска пользователей
 */
struct UserFilter {
    AccessUserType type = AccessUserType::NORMAL; // Тип ключа
    bool hasPin = false;             // Только с PIN
    bool isActive = true;            // Только активные
    bool expired = false;            // Только истекшие
    uint32_t minUses = 0;            // Минимальное количество использований
    uint32_t maxUses = 0xFFFFFFFF;   // Максимальное количество использований
    uint32_t createdAfter = 0;       // Созданы после
    uint32_t createdBefore = 0xFFFFFFFF; // Созданы до
    uint32_t usedAfter = 0;          // Использованы после
    uint32_t usedBefore = 0xFFFFFFFF; // Использованы до
    char nameContains[65] = "";      // Имя содержит подстроку
};

/**
 * @brief Запись в журнале изменений
 */
struct UserChangeLog {
    uint32_t timestamp = 0;          // Время изменения
    char cardId[16] = "";            // ID карты
    char action[16] = "";            // Действие (add, update, remove, expire, use)
    char details[65] = "";           // Детали
};

// ============================================================================
// 3. КЛАСС МЕНЕДЖЕРА ДОСТУПА (FULL РЕЖИМ)
// ============================================================================

class AccessManager : public UserManager {
public:
    // === КОЛБЭКИ ===
    using OnUserAddedCallback = std::function<void(const AccessUserInfo&)>;
    using OnUserUpdatedCallback = std::function<void(const AccessUserInfo&, const AccessUserInfo&)>;
    using OnUserRemovedCallback = std::function<void(const char*)>;
    using OnUserCountChangedCallback = std::function<void(size_t)>;
    using OnUserChangeCallback = std::function<void(const char*, bool)>;
    using OnUserUsedCallback = std::function<void(const char*, uint32_t)>;
    using OnPinVerificationCallback = std::function<void(const char*, bool)>;

    // === СИНГЛТОН ===
    static AccessManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    AccessManager();
    ~AccessManager();

    // Запрещаем копирование
    AccessManager(const AccessManager&) = delete;
    AccessManager& operator=(const AccessManager&) = delete;

    // === IModule ===
    const char* getName() const override { return "AccessManager"; }
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_ACCESS; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _ready && _initialized; }
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // ========================================================================
    // API УПРАВЛЕНИЯ ПОЛЬЗОВАТЕЛЯМИ (CRUD)
    // ========================================================================

    /**
     * @brief Добавляет нового пользователя
     * @param user Данные пользователя
     * @param pinHash Хэш PIN-кода (опционально)
     * @return true если добавление успешно
     */
    bool addUser(const AccessUserInfo& user, const char* pinHash = nullptr);

    /**
     * @brief Добавляет нового пользователя (упрощенная версия)
     * @param cardId UID карты
     * @param name Имя пользователя
     * @param type Тип ключа
     * @param track Трек-номер
     * @param expiry Время истечения (0 = бессрочно)
     * @param pinHash Хэш PIN-кода (опционально)
     * @return true если добавление успешно
     */
    bool addUser(const char* cardId, const char* name, AccessUserType type,
                uint8_t track, uint32_t expiry, const char* pinHash = nullptr);

    /**
     * @brief Обновляет данные пользователя
     * @param cardId ID пользователя
     * @param user Новые данные
     * @param pinHash Новый PIN-код (опционально)
     * @return true если обновление успешно
     */
    bool updateUser(const char* cardId, const AccessUserInfo& user, const char* pinHash = nullptr);

    /**
     * @brief Удаляет пользователя
     * @param cardId ID пользователя
     * @return true если удаление успешно
     */
    bool removeUser(const char* cardId);

    /**
     * @brief Удаляет пользователя без событий (тихо)
     * @param cardId ID пользователя
     * @return true если удаление успешно
     */
    bool removeUserSilent(const char* cardId);

    /**
     * @brief Блокирует пользователя
     * @param cardId ID пользователя
     * @return true если блокировка успешна
     */
    bool blockUser(const char* cardId);

    /**
     * @brief Разблокирует пользователя
     * @param cardId ID пользователя
     * @return true если разблокировка успешна
     */
    bool unblockUser(const char* cardId);

    /**
     * @brief Очищает всю базу пользователей
     */
    void clearAll();

    // ========================================================================
    // API ПОИСКА
    // ========================================================================

    /**
     * @brief Проверяет, существует ли пользователь
     */
    bool exists(const char* cardId) const;

    /**
     * @brief Проверяет, заблокирован ли пользователь
     */
    bool isBlocked(const char* cardId) const;

    /**
     * @brief Возвращает профиль пользователя по ID
     */
    const AccessUserInfo* getUser(const char* cardId) const { return find(cardId); }

    /**
     * @brief Возвращает список всех пользователей
     */
    const std::vector<AccessUserInfo>& getAllUsers() const { return _users; }

    /**
     * @brief Поиск пользователей с фильтром
     */
    std::vector<AccessUserInfo> findWithFilter(const UserFilter& filter) const;

    /**
     * @brief Возвращает самых используемых пользователей
     */
    std::vector<AccessUserInfo> findMostUsed(size_t limit = 10) const;

    /**
     * @brief Возвращает количество пользователей
     */
    uint32_t getUserCount() const { return _users.size(); }

    // ========================================================================
    // API ИСПОЛЬЗОВАНИЯ
    // ========================================================================

    /**
     * @brief Регистрирует использование ключа
     * @param cardId ID карты
     * @return true если использование зарегистрировано успешно
     */
    bool recordUse(const char* cardId);

    // ========================================================================
    // API PIN-КОДОВ
    // ========================================================================

    /**
     * @brief Проверяет PIN-код
     * @param cardId ID пользователя
     * @param pinHash Хэш PIN-кода
     * @return true если PIN-код верный
     */
    bool verifyPin(const char* cardId, const char* pinHash) const;

    /**
     * @brief Проверяет, есть ли у пользователя PIN-код
     */
    bool hasPin(const char* cardId) const;

    /**
     * @brief Проверяет, требуется ли PIN-код для пользователя
     */
    bool isPinRequired(const char* cardId) const;

    // ========================================================================
    // API МАСТЕР-КЛЮЧА
    // ========================================================================

    /**
     * @brief Проверяет, существует ли мастер-ключ
     */
    bool hasMasterKey() const;

    /**
     * @brief Возвращает ID мастер-ключа
     */
    void getMasterKeyId(char* dest, size_t size) const;

    /**
     * @brief Создает мастер-ключ
     * @param cardId ID карты
     * @return true если создание успешно
     */
    bool createMasterKey(const char* cardId);

    /**
     * @brief Удаляет мастер-ключ
     * @return true если удаление успешно
     */
    bool deleteMasterKey();

    /**
     * @brief Проверяет, является ли ключ мастер-ключом
     */
    bool isMasterKey(const char* cardId) const;

    // ========================================================================
    // API ИСТЕКШИХ КЛЮЧЕЙ
    // ========================================================================

    /**
     * @brief Очищает истекшие ключи
     * @param currentTime Текущее время (Unix-время)
     */
    void cleanupExpired(uint32_t currentTime);

    /**
     * @brief Очищает истекшие ключи и возвращает количество удаленных
     */
    size_t cleanupExpiredCount(uint32_t currentTime);

    // ========================================================================
    // API СТАТИСТИКИ
    // ========================================================================

    /**
     * @brief Обновляет статистику пользователей
     */
    void updateUserStats();

    /**
     * @brief Возвращает статистику пользователей
     */
    UserStats getUserStats() const { return _stats; }

    /**
     * @brief Возвращает журнал изменений
     */
    std::vector<UserChangeLog> getChangeLog(size_t limit = 20) const;

    // ========================================================================
    // API ЭКСПОРТА / ИМПОРТА
    // ========================================================================

    /**
     * @brief Экспортирует базу пользователей в поток
     * @param stream Поток для записи (например, Serial или файл)
     */
    void exportToStream(Stream& stream) const;

    /**
     * @brief Импортирует базу пользователей из потока
     * @param stream Поток для чтения
     * @return true если импорт успешен
     */
    bool importFromStream(Stream& stream);

    /**
     * @brief Создает резервную копию в NVS
     */
    bool backupToNVS();

    /**
     * @brief Восстанавливает базу из NVS
     */
    bool restoreFromNVS();

    // ========================================================================
    // КОЛБЭКИ
    // ========================================================================

    void setOnUserAdded(OnUserAddedCallback cb) { _onUserAdded = cb; }
    void setOnUserUpdated(OnUserUpdatedCallback cb) { _onUserUpdated = cb; }
    void setOnUserRemoved(OnUserRemovedCallback cb) { _onUserRemoved = cb; }
    void setOnUserCountChanged(OnUserCountChangedCallback cb) { _onUserCountChanged = cb; }
    void setOnUserChange(OnUserChangeCallback cb) { _onUserChange = cb; }
    void setOnUserUsed(OnUserUsedCallback cb) { _onUserUsed = cb; }
    void setOnPinVerification(OnPinVerificationCallback cb) { _onPinVerification = cb; }

private:
    // ========================================================================
    // ПРИВАТНЫЕ ДАННЫЕ
    // ========================================================================

    bool _fullInitialized = false;
    bool _fullReady = false;

    // --- База пользователей ---
    std::vector<AccessUserInfo> _users;
    std::map<String, AccessUserInfo*> _userMap;    // Быстрый поиск по ID
    std::map<String, AccessUserInfo*> _cardMap;    // Быстрый поиск по карте

    // --- Журнал изменений ---
    std::vector<UserChangeLog> _changeLog;
    static constexpr uint32_t MAX_CHANGE_LOG = 100;

    // --- Статистика ---
    UserStats _stats;
    uint32_t _lastStatsUpdate = 0;
    static constexpr uint32_t STATS_UPDATE_INTERVAL_MS = 60000;

    // --- Работа с файлом ---
    char _dbPath[32] = "/users.json";
    char _backupPath[32] = "/users.bak";
    bool _isDirty = false;
    uint32_t _dirtyTimestamp = 0;
    uint32_t _lastExpiryCheck = 0;
    static constexpr uint32_t EXPIRY_CHECK_INTERVAL_MS = 600000; // 10 минут
    static constexpr uint32_t FLUSH_DELAY_MS = 5000; // 5 секунд

    // --- PIN-кэш ---
    mutable uint32_t _pinCacheTime = 0;
    mutable char _pinCacheCardId[16] = "";
    mutable char _pinCacheHash[65] = "";

    // --- Счетчики ---
    uint32_t _totalAccessAttempts = 0;
    uint32_t _totalAccessGranted = 0;
    uint32_t _totalAccessDenied = 0;
    uint32_t _totalDatabaseLoads = 0;
    uint32_t _totalDatabaseSaves = 0;
    uint32_t _lastErrorCode = 0;
    uint32_t _totalEventsPublished = 0; // НОВОЕ: счетчик событий

    // --- Колбэки ---
    OnUserAddedCallback _onUserAdded = nullptr;
    OnUserUpdatedCallback _onUserUpdated = nullptr;
    OnUserRemovedCallback _onUserRemoved = nullptr;
    OnUserCountChangedCallback _onUserCountChanged = nullptr;
    OnUserChangeCallback _onUserChange = nullptr;
    OnUserUsedCallback _onUserUsed = nullptr;
    OnPinVerificationCallback _onPinVerification = nullptr;

    // ========================================================================
    // ПРИВАТНЫЕ МЕТОДЫ
    // ========================================================================

    // --- Работа с базой ---
    bool loadFromFile();
    bool saveToFile();
    bool loadFromBackup();
    void markDirty();
    void handleBackgroundSave();

    // --- Поиск и индексация ---
    int findIndex(const char* cardId) const;
    AccessUserInfo* find(const char* cardId);
    const AccessUserInfo* find(const char* cardId) const;
    void rebuildMaps();
    void generateUserId(AccessUserInfo& user);

    // --- PIN-коды ---
    bool setPin(const char* cardId, const char* pinHash);
    bool removePin(const char* cardId);
    bool isPinUnique(const char* pinHash, const char* excludeCard = nullptr) const;
    void clearPinCache();

    // --- Журнал изменений ---
    void addChangeLog(const char* cardId, const char* action, const char* details);

    // --- Публикация событий ---
    void publishUserEvent(const char* cardId, const char* name, uint8_t type,
                         int32_t eventId, bool success, const char* details = nullptr);
    void publishPinEvent(const char* cardId, bool success);
    void publishStatsEvent();
    void publishErrorEvent(const char* errorCode);
    void publishAccessEvent(const char* eventType, const char* details, bool success); // НОВОЕ: через новую шину

    // --- Вспомогательные ---
    const char* keyTypeToString(AccessUserType type) const;
    AccessUserType stringToKeyType(const char* str) const;
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    bool isValidCardId(const char* cardId) const;
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
};