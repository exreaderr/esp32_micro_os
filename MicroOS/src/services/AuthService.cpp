// ============================================================================
// AuthService.cpp — реализация аутентификации, provisioning и rate-limiting
// ============================================================================
#include "AuthService.h"
#include "ConfigService.h"
#include "../core/Events.h"
#include <esp_mac.h>              // esp_read_mac — соль хэша (E1)

AuthService& AuthService::getInstance() {
    static AuthService instance;
    return instance;
}

// ============================================================================
// РЕГИСТРАЦИЯ КОНФИГ-СХЕМЫ
// ============================================================================
// auth.admin_pin — SECRET: хранится в NVS, никогда не попадает в JSON,
// не отдаётся в API/UI (правила ConfigService). Пустое значение = C1,
// устройство ждёт provisioning.
// ============================================================================
void AuthService::registerExtensions() {
    ConfigService::getInstance().addFields("Безопасность", {
        { "auth.admin_pin",  ConfigType::SECRET, "", 0, 0, CFG_SECRET,
          "Безопасность", "Пароль администратора (хэш, задаётся при настройке)" },
        { "auth.max_fails",  ConfigType::UINT, "5", 3, 10, CFG_NONE,
          "Безопасность", "Неудач до блокировки" },
        { "auth.lockout_min", ConfigType::UINT, "5", 1, 60, CFG_NONE,
          "Безопасность", "Длительность блокировки, мин" },
        { "auth.window_min", ConfigType::UINT, "10", 1, 60, CFG_NONE,
          "Безопасность", "Окно подсчёта неудач, мин" },
    });
}

// ============================================================================
// INIT / START / STOP / TICK
// ============================================================================
void AuthService::init() {
    memset(_slots, 0, sizeof(_slots));
    _initialized = true;
    log(LogLevel::Info, "init: %s",
        isProvisioned() ? "provisioned" : "NOT provisioned (C1 setup pending)");
}

void AuthService::start() {
    _started = true;
    if (!isProvisioned()) {
        // C1: первый старт без ПИН-кода. Событие — сигнал веб-UI (Фаза 3)
        // открыть мастер первичной настройки, а профилю — НЕ требовать
        // админ-действий, которым некого авторизовать.
        ShEventData d; d.clear();
        postEvent(AUTH_EVENT_SETUP_REQUIRED, &d);
        _setupRemindedMs = millis();
        log(LogLevel::Warning,
            "SETUP REQUIRED: admin PIN not set (provisioning mode)");
    }
}

void AuthService::stop() { _started = false; }

void AuthService::tick() {
    // Периодическое напоминание о незавершённом provisioning'е:
    // устройство без ПИН-кода — открытая дверь, об этом нельзя забыть.
    if (!isProvisioned() &&
        millis() - _setupRemindedMs >= AUTH_SETUP_REMIND_MS) {
        _setupRemindedMs = millis();
        ShEventData d; d.clear();
        postEvent(AUTH_EVENT_SETUP_REQUIRED, &d);
        log(LogLevel::Warning, "SETUP REQUIRED (reminder)");
    }
}

// ============================================================================
// PROVISIONING (C1)
// ============================================================================
bool AuthService::isProvisioned() const {
    char stored[CFG_VALUE_LEN];
    cfgGetStr("auth.admin_pin", stored, sizeof(stored), "");
    return stored[0] != '\0';
}

bool AuthService::setAdminPin(const char* newPin, const char* currentPin) {
    if (newPin == nullptr || !isAdminPasswordValid(newPin)) {
        log(LogLevel::Warning, "setAdminPin: invalid password format");
        return false;
    }

    bool wasProvisioned = isProvisioned();
    if (wasProvisioned) {
        // Смена ПИН-кода — только с действующим (и через rate-limiter:
        // перебор currentPin ограничен так же, как перебор при входе).
        if (currentPin == nullptr || !verifyAdminPin(currentPin, "setpin")) {
            return false;
        }
    }

    char hashed[CFG_VALUE_LEN];
    hashPin(newPin, hashed, sizeof(hashed));
    if (!ConfigService::getInstance().set("auth.admin_pin", hashed)) {
        publishError("PIN_SAVE");
        return false;
    }

    noteSuccess("admin");
    log(LogLevel::Info, "admin password %s",
        wasProvisioned ? "changed" : "SET (provisioning complete)");
    return true;
}

// ============================================================================
// АУТЕНТИФИКАЦИЯ
// ============================================================================
bool AuthService::verifyAdminPin(const char* pin, const char* source) {
    if (!isProvisioned() || pin == nullptr) return false;

    if (isRateLimited("admin")) {
        // Уже заблокированы: новых событий не плодим (LOCKED_OUT ушло
        // при переходе в блокировку), но факт фиксируем в лог для аудита.
        log(LogLevel::Warning, "login attempt while locked out (%s)", source);
        return false;
    }

    char candidate[CFG_VALUE_LEN];
    hashPin(pin, candidate, sizeof(candidate));
    char stored[CFG_VALUE_LEN];
    cfgGetStr("auth.admin_pin", stored, sizeof(stored), "");

    if (strcmp(candidate, stored) == 0) {
        noteSuccess("admin");
        ShEventData d; d.clear();
        d.code = (int32_t)AuthRole::Admin;
        safeStrCopy(d.payload, sizeof(d.payload), source);
        postEvent(AUTH_EVENT_LOGIN, &d);
        return true;
    }

    uint8_t remaining = noteFailure("admin");
    ShEventData d; d.clear();
    if (remaining == 0) {
        d.code = (int32_t)(cfgGetUInt("auth.lockout_min", 5) * 60);
        safeStrCopy(d.payload, sizeof(d.payload), source);
        postEvent(AUTH_EVENT_LOCKED_OUT, &d);
        log(LogLevel::Warning, "LOCKED OUT for %ld s (%s)", (long)d.code, source);
    } else {
        d.code = remaining;
        safeStrCopy(d.payload, sizeof(d.payload), source);
        postEvent(AUTH_EVENT_LOGIN_FAILED, &d);
        log(LogLevel::Warning, "login failed, %u attempts left (%s)",
            remaining, source);
    }
    return false;
}

// ============================================================================
// УНИВЕРСАЛЬНЫЙ RATE-LIMITER (C3)
// ============================================================================
// Скользящее окно: неудачи старше auth.window_min выбывают. После
// auth.max_fails неудач в окне — блокировка на auth.lockout_min.
// Ключи: "admin" (здесь), "user"/"ota"/... (профили, Фаза 3) — один
// механизм на все точки перебора, таблица статическая, без кучи.
// ============================================================================
AuthService::RateSlot* AuthService::findSlot(const char* key,
                                             bool createIfAbsent) {
    RateSlot* oldest = &_slots[0];
    for (uint8_t i = 0; i < AUTH_RL_SLOTS; ++i) {
        if (_slots[i].key[0] != '\0' &&
            strncmp(_slots[i].key, key, AUTH_RL_KEY_LEN) == 0) {
            return &_slots[i];
        }
        if (_slots[i].windowStartMs < oldest->windowStartMs) oldest = &_slots[i];
    }
    if (!createIfAbsent) return nullptr;
    // Свободный или самый старый слот (LRU-вытеснение: ключи короткоживущие,
    // 8 слотов хватает с запасом на все точки перебора устройства).
    memset(oldest, 0, sizeof(RateSlot));
    safeStrCopy(oldest->key, sizeof(oldest->key), key);
    oldest->windowStartMs = millis();
    return oldest;
}

bool AuthService::isRateLimited(const char* key) {
    RateSlot* s = findSlot(key, false);
    return s != nullptr && s->lockedUntilMs != 0 &&
           (int32_t)(millis() - s->lockedUntilMs) < 0;
}

uint32_t AuthService::rateLimitRemainingSec(const char* key) {
    RateSlot* s = findSlot(key, false);
    if (s == nullptr || s->lockedUntilMs == 0) return 0;
    int32_t left = (int32_t)(s->lockedUntilMs - millis());
    return left > 0 ? (uint32_t)left / 1000 : 0;
}

uint8_t AuthService::noteFailure(const char* key) {
    RateSlot* s = findSlot(key, true);
    uint32_t now = millis();
    uint32_t windowMs = cfgGetUInt("auth.window_min", 10) * 60000UL;
    uint8_t  maxFails = (uint8_t)cfgGetUInt("auth.max_fails", 5);

    // Окно истекло — считаем заново
    if (now - s->windowStartMs >= windowMs) {
        s->fails = 0;
        s->windowStartMs = now;
    }
    if (s->fails < 255) s->fails++;

    if (s->fails >= maxFails) {
        s->lockedUntilMs = now + cfgGetUInt("auth.lockout_min", 5) * 60000UL;
        s->fails = 0;
        s->windowStartMs = now;
        return 0;
    }
    return (uint8_t)(maxFails - s->fails);
}

void AuthService::noteSuccess(const char* key) {
    RateSlot* s = findSlot(key, false);
    if (s != nullptr) {
        s->fails = 0;
        s->lockedUntilMs = 0;
        s->windowStartMs = millis();
    }
}

// ============================================================================
// ХЭШИРОВАНИЕ ПИН-КОДА
// ============================================================================
void AuthService::hashPin(const char* pin, char* out, size_t outSize) const {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    uint32_t h = 5381;   // djb2
    for (uint8_t i = 0; i < 6; ++i) h = h * 33 + mac[i];
    h = h * 33 + ':';
    for (const char* p = pin; *p; ++p) h = h * 33 + (uint8_t)*p;
    snprintf(out, outSize, "h%08lx", (unsigned long)h);
}

bool AuthService::isAdminPasswordValid(const char* pwd) const {
    // Полноценный пароль администратора: печатные ASCII без пробелов,
    // 4..32 символа. Цифровой ПИН — политика профильных пользователей.
    size_t len = strlen(pwd);
    if (len < AUTH_PASSWORD_MIN || len > AUTH_PASSWORD_MAX) return false;
    for (const char* p = pwd; *p; ++p) {
        if (*p <= ' ' || *p > '~') return false;
    }
    return true;
}
