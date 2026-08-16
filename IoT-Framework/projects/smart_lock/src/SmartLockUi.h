// ============================================================================
// SmartLockUi.h — ВЕБ-ЛИЦО КОНТРОЛЛЕРА СКУД (IUiProvider)
// ============================================================================
// Публичная страница "/" — панель жильца: приветствие, статус двери,
// большая кнопка «Открыть» (по ПИНу жильца, если lock.user_pin_enabled).
// Админские операции (база карт, режимы, бэкап) — через /api/dev/lock/*
// с токеном ядерной админ-сессии; админская страница ядра (/admin) даёт
// авто-UI полей lock.* — профиль не дублирует.
//
// Безопасность (политика профиля, C3):
//   · ПИН жильца — ПЕРСОНАЛЬНЫЙ (users.json, СТРОГО уникальный): по ПИНу
//     контроллер знает жильца (имя → приветствие, «последний доступ»);
//   · lock.user_pin — legacy-запасной общий ПИН (анонимный «Гость»);
//   · перебор — универсальный rate-limiter AuthService (ключ "sl_web");
//   · админ — HttpService::isAdminToken(req.token) (сессия ядра).
// ============================================================================
#pragma once

#include <services/IUiProvider.h>
#include "CardDbFormat.h"   // SlUser

class SmartLockUi : public IUiProvider {
public:
    const char* uiTitle() const override { return "smart_lock"; }

    /// Фрагмент панели жильца (бюджет ~2 КБ буфера публичной страницы).
    size_t renderPublicHtml(char* buf, size_t bufSize) override;

    /// /api/dev/lock/* — эндпоинты (см. .cpp, таблица в шапке).
    bool handleApi(const char* pathTail, const ShUiRequest& req,
                   char* responseBuf, size_t bufSize,
                   int& statusCode) override;

private:
    // --- Авторизация ------------------------------------------------------
    bool isAdmin(const ShUiRequest& req) const;
    /// Идентификация жильца по ПИНу (per-user, rate-limited; legacy
    /// lock.user_pin — анонимный «Гость»). nullptr — отказ.
    const SlUser* identifyByPin(const ShUiRequest& req);

    // --- Сессии жильца (RAM-токены, скользящее окно бездействия) ----------
    // Урок 5.0.x: «сессия» жильца была просто ПИНом в sessionStorage
    // браузера — жила бесконечно (админская ядра — 30 мин скользящих).
    // Теперь ПИН обменивается на токен (user/auth), токен скользит при
    // каждом обращении и умирает по lock.user_session_min. ПИН как
    // креденшел из панели убран; серверный фолбэк ?pin= оставлен для
    // прямых API-вызовов (совместимость).
    static constexpr uint8_t SL_UTOK_SLOTS = 4;   // жильцов за панелью разом
    struct ResidentSession {
        char     token[9];        // 8 hex + '\0' (как админские ядра)
        char     name[65];        // имя жильца (SlUser::name) для журнала
        uint32_t expiresMs;       // скользящий дедлайн (millis)
    };
    ResidentSession _usess[SL_UTOK_SLOTS] = {};

    /// Выдать/продлить токен жильцу (свободный/просроченный слот, иначе LRU).
    const char* issueUserToken(const char* name);
    /// Токен валиден? Скользящее продление при успехе; в nameOut (если
    /// не nullptr) — имя для персонализации журнала.
    bool userTokenValid(const char* tok, char* nameOut, size_t nameSize);
    /// Отзыв токена (logout жильца). Незнакомый токен — тихий no-op.
    void dropUserToken(const char* tok);
    /// Окно бездействия из конфига (lock.user_session_min), в миллисекундах.
    static uint32_t userSessionMs();

    // --- Эндпоинты --------------------------------------------------------
    bool apiStatus(char* buf, size_t size);
    bool apiOpen(const ShUiRequest& req, char* buf, size_t size, int& status);
    bool apiSetMode(const ShUiRequest& req, char* buf, size_t size);
    bool apiReadStart(char* buf, size_t size);
    bool apiReadResult(char* buf, size_t size);
    bool apiKeysList(const ShUiRequest& req, char* buf, size_t size);
    bool apiKeysAdd(const ShUiRequest& req, char* buf, size_t size,int& status);
    bool apiKeysUpdate(const ShUiRequest& req, char* buf, size_t size,int& status);
    bool apiKeysRemove(const ShUiRequest& req, char* buf, size_t size,int& status);
    bool apiKeysBlock(const ShUiRequest& req, char* buf, size_t size,int& status);
    bool apiKeysClear(const ShUiRequest& req, char* buf, size_t size,int& status);
    bool apiMasterDelete(const ShUiRequest& req, char* buf, size_t size,int& status);
    bool apiDbBackup(char* buf, size_t size, int& status);
    bool apiDbRestore(char* buf, size_t size, int& status);
    bool apiAudioTest(const ShUiRequest& req, char* buf, size_t size);
    bool apiPazReset(char* buf, size_t size);
    bool apiDlogChannels(char* buf, size_t size);
    bool apiDlog(const ShUiRequest& req, char* buf, size_t size, int& status);
};
