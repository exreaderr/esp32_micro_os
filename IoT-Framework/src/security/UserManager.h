// ============================================================================
// UserManager.h - БАЗОВОЕ УПРАВЛЕНИЕ ПОЛЬЗОВАТЕЛЯМИ (ЛЕГКАЯ ВЕРСИЯ) v5.0
// ============================================================================
// Описание: Управляет двумя типами пользователей: администратор и гость.
//           Используется для большинства контроллеров (не СКУД).
//           Обеспечивает простую аутентификацию и управление правами.
//
// АРХИТЕКТУРНЫЕ ПРИНЦИПЫ:
// 1. Минимализм: только два типа пользователей.
// 2. Простота: вход без пароля для гостя, пароль для администратора.
// 3. Изоляция: все операции через события.
// 4. Безопасность: хранение пароля в зашифрованном виде (SHA-256).
// 5. Совместимость: полное сохранение логики из USER_MANAGER_LIGHT.
// ============================================================================
#pragma once

#include "core/IModule.h"
#include "core/ShEventData.h"
#include <functional>
#include <Preferences.h>

// ============================================================================
// 1. СТРУКТУРЫ ДАННЫХ
// ============================================================================

/**
 * @brief Роли пользователей (для легкой версии)
 */
enum class UserRole : uint8_t {
    GUEST = 0,          // Гость (только просмотр)
    ADMIN = 1           // Администратор (полный доступ)
};

/**
 * @brief Информация об администраторе
 */
struct AdminProfile {
    char passwordHash[64];      // Хэш пароля (SHA-256)
    char cardId[16];            // UID карты (опционально)
    uint32_t createdAt;         // Время создания
    uint8_t track;              // Трек-номер (для озвучивания)
    bool isActive;              // Активен ли администратор
};

/**
 * @brief Статистика администратора
 */
struct AdminStats {
    uint32_t loginCount;        // Количество успешных входов
    uint32_t failedAttempts;    // Количество неудачных попыток
    uint32_t lastLoginTime;     // Время последнего входа
    uint32_t lastLogoutTime;    // Время последнего выхода
    bool hasPassword;           // Установлен ли пароль
    bool hasCardId;             // Привязана ли карта
};

// ============================================================================
// 2. КЛАСС МЕНЕДЖЕРА ПОЛЬЗОВАТЕЛЕЙ (ЛЕГКАЯ ВЕРСИЯ)
// ============================================================================

class UserManager : public IModule {
public:
    // === КОЛБЭКИ ===
    using OnLoginCallback = std::function<void(UserRole)>;
    using OnLogoutCallback = std::function<void()>;
    using OnErrorCallback = std::function<void(const char*)>;

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
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_USER; }
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
    // API АУТЕНТИФИКАЦИИ
    // ========================================================================

    /**
     * @brief Вход в систему как администратор
     * @param passwordHash Хэш пароля (SHA-256)
     * @return true если вход успешен
     */
    bool loginAdmin(const char* passwordHash);

    /**
     * @brief Вход в систему как администратор по карте (опционально с PIN)
     * @param cardId UID карты
     * @param pinHash Хэш PIN-кода (опционально)
     * @return true если вход успешен
     */
    bool loginAdmin(const char* cardId, const char* pinHash = nullptr);

    /**
     * @brief Выход из системы
     */
    void logoutAdmin();

    /**
     * @brief Проверяет, аутентифицирован ли администратор
     */
    bool isAdminAuthenticated() const { return _adminAuthenticated; }

    /**
     * @brief Возвращает ID текущей сессии
     */
    uint32_t getSessionId() const { return _sessionId; }

    // ========================================================================
    // API УПРАВЛЕНИЯ АДМИНИСТРАТОРОМ
    // ========================================================================

    /**
     * @brief Устанавливает пароль администратора
     * @param passwordHash Хэш пароля (SHA-256)
     * @return true если пароль установлен успешно
     */
    bool setAdminPassword(const char* passwordHash);

    /**
     * @brief Устанавливает карту администратора
     * @param cardId UID карты
     * @return true если карта установлена успешно
     */
    bool setAdminCard(const char* cardId);

    /**
     * @brief Устанавливает трек-номер администратора (для озвучивания)
     * @param track Трек-номер (1-99)
     */
    void setAdminTrack(uint8_t track);

    /**
     * @brief Проверяет, есть ли администратор
     */
    bool hasAdmin() const;

    /**
     * @brief Возвращает ID администратора (карту, если есть)
     * @param dest Буфер для ID
     * @param size Размер буфера
     */
    void getAdminId(char* dest, size_t size) const;

    /**
     * @brief Возвращает трек-номер администратора
     */
    uint8_t getAdminTrack() const { return _admin.track; }

    // ========================================================================
    // API УПРАВЛЕНИЯ ГОСТЕМ
    // ========================================================================

    /**
     * @brief Включает или отключает гостевой доступ
     */
    void enableGuestAccess(bool enable) { _enableGuest = enable; }

    /**
     * @brief Проверяет, включен ли гостевой доступ
     */
    bool isGuestAccessEnabled() const { return _enableGuest; }

    /**
     * @brief Проверяет, является ли текущий пользователь гостем
     */
    bool isGuest() const { return !_adminAuthenticated && _enableGuest; }

    // ========================================================================
    // КОЛБЭКИ
    // ========================================================================

    void setOnLoginCallback(OnLoginCallback cb) { _onLoginCallback = cb; }
    void setOnLogoutCallback(OnLogoutCallback cb) { _onLogoutCallback = cb; }
    void setOnErrorCallback(OnErrorCallback cb) { _onErrorCallback = cb; }

    // ========================================================================
    // ДИАГНОСТИКА
    // ========================================================================

    /**
     * @brief Возвращает статистику администратора
     */
    const AdminStats& getAdminStats() const { return _adminStats; }

    /**
     * @brief Возвращает количество попыток входа
     */
    uint32_t getAuthAttempts() const { return _authAttempts; }

    /**
     * @brief Сбрасывает счетчик попыток входа
     */
    void resetAuthAttempts() { _authAttempts = 0; }

    /**
     * @brief Очищает все данные администратора
     */
    void clearAdminData();

private:
    // ========================================================================
    // ПРИВАТНЫЕ ДАННЫЕ
    // ========================================================================

    uint8_t _id = MODULE_ID_USER;       // ID модуля (UserManager)
    bool _initialized = false;
    bool _ready = false;

    // --- Администратор ---
    AdminProfile _admin;                    // Профиль администратора
    AdminStats _adminStats;                 // Статистика администратора
    bool _adminAuthenticated = false;       // Флаг аутентификации
    uint32_t _sessionId = 0;                // ID текущей сессии

    // --- Конфигурация ---
    bool _enableGuest = true;               // Включен ли гостевой доступ
    uint32_t _sessionTimeoutMs = 900000;    // Таймаут сессии (15 минут)
    uint32_t _maxAuthAttempts = 5;          // Макс. попыток входа
    uint32_t _authBlockTimeMs = 300000;     // Блокировка на 5 минут
    uint32_t _authAttempts = 0;             // Текущее количество попыток
    uint32_t _lastAuthAttemptTime = 0;      // Время последней попытки

    // --- NVS (для хранения настроек) ---
    Preferences _pref;
    const char* _nvsNamespace = "user_mgr";
    const char* _adminKey = "admin_hash";
    const char* _adminCardKey = "admin_card";
    const char* _adminCreatedKey = "admin_created";
    const char* _adminTrackKey = "admin_track";

    // --- Статистика ---
    uint32_t _totalLogins = 0;
    uint32_t _totalFailedLogins = 0;
    uint32_t _totalPasswordChanges = 0;

    // --- Колбэки ---
    OnLoginCallback _onLoginCallback = nullptr;
    OnLogoutCallback _onLogoutCallback = nullptr;
    OnErrorCallback _onErrorCallback = nullptr;

    // ========================================================================
    // ПРИВАТНЫЕ МЕТОДЫ
    // ========================================================================

    bool verifyAdminPassword(const char* passwordHash) const;
    bool verifyAdminCard(const char* cardId) const;
    void saveAdminToNVS();
    void loadAdminFromNVS();
    void clearAdminNVS();
    void publishAuthEvent(bool success, const char* role, const char* details = nullptr);
    void publishErrorEvent(const char* errorCode);
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    bool isValidPasswordHash(const char* hash) const;
    bool isValidCardId(const char* cardId) const;
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    void generateSecureToken(char* dest, size_t size);
    bool isAuthBlocked() const;
};