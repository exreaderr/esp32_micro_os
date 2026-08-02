// ============================================================================
// HttpService.h — ВЕБ-СЕРВЕР И РАЗДЕЛЬНЫЙ ВЕБ-UI (Фаза 3, порция 3)
// ============================================================================
// «Лицо» устройства. Две части (решение пользователя):
//   · ПУБЛИЧНАЯ "/"        — идентичность + состояние + секция профиля
//                            (IUiProvider). Для СКУД доступ к функциям —
//                            по профильному ПИНу (политика провайдера).
//   · АДМИНСКАЯ "/admin"   — сеть, параметры (авто-UI из ConfigService-схем),
//                            телеметрия, журналы, аудит, OTA. Только по
//                            ПИН-коду администратора (AuthService).
//
// Безопасность (C1/C3):
//   · ПИН никогда не хранится в открытом виде и не передаётся дальше
//     /api/auth — сессия по токену (X-Auth-Token, 30 мин скользящего окна,
//     4 слота, RAM-only: ребут = выход всех сессий);
//   · перебор ПИНа ограничен универсальным rate-limiter'ом AuthService
//     (блокировка -> 401 с остатком блокировки);
//   · первый старт без ПИНа: /api/setup (C1) — мастер provisioning'а,
//     работает только пока устройство НЕ provisioned;
//   · SECRET-поля не покидают устройство (правило ConfigService).
//
// Запускается и в Safe Mode — это recovery-интерфейс: при аварийном режиме
// публичная страница показывает диагностику, админская даёт OTA/конфиг.
//
// Реализация — WebServer из ядра Arduino (синхронный, однопоточный;
// handleClient из нашего tick => обработчики живут в контексте loop и
// могут безопасно звать сервисы).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "IUiProvider.h"
#include <WebServer.h>

// Бюджеты
constexpr uint8_t  HTTP_TOKEN_SLOTS   = 4;     // одновременных админ-сессий
constexpr uint8_t  HTTP_TOKEN_LEN     = 9;     // 8 hex + '\0'
constexpr uint32_t HTTP_SESSION_MS    = 30UL * 60 * 1000;  // скользящее окно
constexpr uint32_t HTTP_RESTART_DELAY_MS = 1500; // ack успеет уйти
constexpr size_t   HTTP_JSON_BUF      = 8192;  // config JSON (схемы модулей)
constexpr size_t   HTTP_PAGE_BUF      = 4096;  // публичная страница (динамич.)

class HttpService : public ModuleBase {
public:
    static HttpService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "HttpService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0102; }   // транспорт

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;                 // handleClient + стейт-машина
    uint32_t getTickIntervalMs() const override { return 5; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- ИНЖЕКЦИЯ UI ПРОФИЛЯ (из registerExtensions профиля) --------------
    void setUiProvider(IUiProvider* provider) { _ui = provider; }

    /// Токен текущего запроса (для IUiProvider::handleApi — авторизация
    /// профильного уровня). Валиден только внутри обработчика.
    const char* lastToken() const { return _currentToken; }

    /// Валиден ли токен админской сессии (для IUiProvider: админские
    /// эндпоинты профиля). Скользящее окно продлевается, как у checkAdmin.
    bool isAdminToken(const char* token);

private:
    HttpService() = default;

    // --- СЕРВЕР -------------------------------------------------------------
    void serverStart();
    void serverStop();
    void registerRoutes();

    // --- АУТЕНТИФИКАЦИЯ СЕССИЙ ------------------------------------------------
    bool checkAdmin();                 // токен из запроса валиден? (+скольжение)
    bool sessionValid(const char* tok);// общий код checkAdmin/isAdminToken
    const char* issueToken();          // новая сессия после verifyAdminPin
    void dropToken(const char* token);

    // Колбэк аргументов для ShUiRequest (кольцевой буфер: значение живёт
    // до HTTP_ARG_RING-1 следующих вызовов).
    static const char* uiArgTrampoline(void* ctx, const char* name);

    // --- ОБРАБОТЧИКИ МАРШРУТОВ ------------------------------------------------
    void handleRoot();                 // публичная страница
    void handleAdmin();                // админская (PROGMEM)
    void handleApiSystem();            // публичный статус (кратко)
    void handleApiAuth();              // POST pin -> token (rate-limited)
    void handleApiSetup();             // POST pin (C1, только не provisioned)
    void handleApiLogout();
    void handleApiTelemetry();         // admin: снимок B1
    void handleApiConfigGet();         // admin: значения (без секретов)
    void handleApiConfigSet();         // admin: key&value -> ConfigService
    void handleApiLogs();              // admin: tail лога
    void handleApiAudit();             // admin: выгрузка /audit.log
    void handleApiReboot();            // admin
    void handleApiOta();               // admin: url -> UpdateService (A1)
    void handleApiHealth();            // admin: сводка ПАЗ (HealthMonitor)
    void handleApiAuthChange();        // смена пароля: old+new (rate-limited)
    void handleApiTimeSync();          // admin: принудительный NTP-запрос
    void handleApiTimeSet();           // admin: время от браузера (unix UTC)
    void handleApiDev();               // профильный API (/api/dev/*)
    void handleWebFile();              // статика профиля из LittleFS (/web/*)
    void handleNotFound();

    void sendJson(int code, const char* json);
    bool requireAdmin();               // false -> 401 уже отправлен

    // --- ДАННЫЕ -----------------------------------------------------------------
    WebServer _server{80};
    bool     _serverUp = false;
    IUiProvider* _ui = nullptr;

    // Сессии (RAM-only)
    struct Session { char token[HTTP_TOKEN_LEN]; uint32_t expiresMs; };
    Session  _sessions[HTTP_TOKEN_SLOTS];

    char     _currentToken[HTTP_TOKEN_LEN] = "";  // токен текущего запроса
    uint32_t _restartAtMs = 0;

    // Кольцевой буфер аргументов для uiArgTrampoline (профиль читает
    // несколько аргументов подряд — одного буфера мало).
    // Размер — с запасом от МАКСИМАЛЬНОГО числа getArg в одном обработчике:
    // apiKeysAdd читает 6 (id,name,type,track,expiry_days,pin). При 4 слотах
    // 5-й/6-й аргумент ЗАТИРАЛ ранее прочитанные — id превращался в значение
    // pin («4266») и add() отвечал bad_id (жук добавления жильца с ПИНом).
    static constexpr uint8_t HTTP_ARG_RING = 8;
    char     _argRing[HTTP_ARG_RING][48];
    uint8_t  _argRingPos = 0;

    // Рабочие буферы (BSS, не стек)
    char     _jsonBuf[HTTP_JSON_BUF];
    char     _pageBuf[HTTP_PAGE_BUF];
};
