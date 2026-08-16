// ============================================================================
// AuthService.h — АУТЕНТИФИКАЦИЯ, PROVISIONING (C1) И RATE-LIMITING (C3)
// ============================================================================
// Фаза 2, порция 2. Закрывает две дыры монолита v4.2.2:
//
//   C1 — первый старт без "заводского" ПИН-кода (backdoor). В монолите
//        ПИН по умолчанию жил в прошивке — любой, кто читал исходник,
//        знал ключ от всех устройств. Здесь: ПИН отсутствует, пока его
//        не задаст владелец при provisioning'е (через веб-UI Фазы 3);
//        состояние "требуется настройка" публикуется событием
//        AUTH_EVENT_SETUP_REQUIRED.
//
//   C3 — ограничение перебора: счётчик неудач в скользящем окне +
//        блокировка на заданный срок. Механизм УНИВЕРСАЛЬНЫЙ (ключ "admin",
//        "user", "ota", ...) — профильные ПИН-коды (СКУД) и точки API
//        используют тот же лимитер, а не пишут свой.
//
// ПИН хранится ХЭШИРОВАННЫМ в SECRET-поле ConfigService (NVS, не JSON,
// не отдаётся в API). Хэш — djb2(MAC + ПИН): защита от casual-чтения NVS,
// НЕ криптография. Для коммерческой версии — замена на PBKDF2/scrypt
// (точка расширения предусмотрена решением C2, см. дорожную карту).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"

// ============================================================================
// РОЛИ (code в AUTH_EVENT_LOGIN)
// ============================================================================
enum class AuthRole : uint8_t {
    User  = 0,   // профильный пользователь (СКУД: открытие по ПИНу)
    Admin = 1    // администратор: настройки, OTA, provisioning
};

// Бюджеты лимитера
constexpr uint8_t  AUTH_RL_SLOTS     = 8;    // отслеживаемых ключей
constexpr uint8_t  AUTH_RL_KEY_LEN   = 16;   // длина ключа лимитера
// Идеология v5.0: у администратора — полноценный пароль (печатные ASCII
// без пробелов, 4..32), цифровой ПИН 4..8 — у профильных пользователей.
constexpr uint8_t  AUTH_PASSWORD_MIN = 4;
constexpr uint8_t  AUTH_PASSWORD_MAX = 32;
constexpr uint32_t AUTH_SETUP_REMIND_MS = 300000;  // напоминание, 5 мин

class AuthService : public ModuleBase {
public:
    static AuthService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "AuthService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0009; }

    void registerExtensions() override;   // схема auth.*
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 1000; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- PROVISIONING (C1) -------------------------------------------------
    /// ПИН администратора задан (устройство прошло первичную настройку).
    bool isProvisioned() const;

    /// Задать пароль администратора (имя метода — историческое: «ПИН» здесь
    /// давно полноценный пароль; цифровые ПИНы — политика профильных
    /// пользователей, как жильцы у smart_lock).
    /// Если устройство НЕ provisioned — currentPin игнорируется (первичная
    /// настройка, окно provisioning). Если provisioned — currentPin обязан
    /// пройти verifyAdminPin (смена пароля = административная операция).
    /// Формат: 4..32 печатных ASCII без пробелов (isAdminPasswordValid).
    /// false — неверный формат или currentPin.
    bool setAdminPin(const char* newPin, const char* currentPin = nullptr);

    // --- АУТЕНТИФИКАЦИЯ ------------------------------------------------------
    /// Проверить ПИН администратора. Побочные эффекты: rate-limiting
    /// (неудача расходует попытку), события AUTH_EVENT_*.
    /// source — строка-источник для аудита ("web", "serial") — Фаза 3, B3.
    bool verifyAdminPin(const char* pin, const char* source = "local");

    // --- УНИВЕРСАЛЬНЫЙ RATE-LIMITER (C3) ----------------------------------
    /// Ключ заблокирован прямо сейчас?
    bool isRateLimited(const char* key);
    /// Осталось блокировки, сек (0 — не заблокирован).
    uint32_t rateLimitRemainingSec(const char* key);
    /// Фиксация неудачи. Возвращает остаток попыток до блокировки
    /// (0 = только что заблокирован).
    uint8_t noteFailure(const char* key);
    /// Успех — сброс счётчика ключа.
    void noteSuccess(const char* key);

private:
    AuthService() = default;

    // --- ВНУТРЕННЯЯ КУХНЯ ---------------------------------------------------
    /// djb2(MAC_ETH + ':' + pin) -> "h%08lx". MAC — соль экземпляра железа:
    /// снятый с одного устройства NVS-хэш не подойдёт другому.
    void hashPin(const char* pin, char* out, size_t outSize) const;
    bool isAdminPasswordValid(const char* pwd) const;

    struct RateSlot {
        char     key[AUTH_RL_KEY_LEN];
        uint8_t  fails;           // неудач в текущем окне
        uint32_t windowStartMs;   // начало скользящего окна
        uint32_t lockedUntilMs;   // блокировка до (0 — нет)
    };
    RateSlot* findSlot(const char* key, bool createIfAbsent);

    RateSlot _slots[AUTH_RL_SLOTS];
    uint32_t _setupRemindedMs = 0;   // последнее напоминание SETUP_REQUIRED
};
