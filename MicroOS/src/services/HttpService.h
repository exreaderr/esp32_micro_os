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

namespace fs { class File; }   // streamFileDownload — без таскания FS.h в шапку

// Бюджеты
constexpr uint8_t  HTTP_TOKEN_SLOTS   = 4;     // одновременных админ-сессий
constexpr uint8_t  HTTP_TOKEN_LEN     = 9;     // 8 hex + '\0'
constexpr uint32_t HTTP_SESSION_MS    = 30UL * 60 * 1000;  // скользящее окно
constexpr uint32_t HTTP_RESTART_DELAY_MS = 1500; // ack успеет уйти
// 5.6.1: 16 КБ -> 24 КБ под CFG_MAX_FIELDS=96 (×~250 Б/поле с запасом).
constexpr size_t   HTTP_JSON_BUF      = 24576; // config JSON (схемы модулей).
// 5.1.1: схема 61 поля ≈ 10 КБ — 8 КБ МОЛЧА обрезали хвост (потерянные поля
// панели 5.1.0). Буфер — HEAP в init() (урок outbox: BSS dram0_0_seg полон).
constexpr size_t   HTTP_PAGE_BUF      = 4096;  // публичная страница (динамич.)
// 5.8.0, HTTP-гард: веб поднимаем НЕ сразу за сетью, а через паузу —
// бут-шторм (NTP/MQTT/брокер/SD) не делим с lwIP-сокетами браузеров
// (урок 14.08: три браузера на загрузке замка уронили heap до 5 КБ;
//  вкладки сами переподключаются через пару секунд — потерь нет).
constexpr uint32_t HTTP_WEB_DELAY_MS  = 10000; // пауза после подъёма сети

class HttpService : public ModuleBase {
public:
    static HttpService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "HttpService"; }
    const char* getVersion() const override { return "5.1.2"; }
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

    /// 5.8.0, «Скачать журнал»: потоковая отдача файла как attachment
    /// (Content-Disposition + streamFile кусками — файл целиком в heap
    /// НЕ поднимается). Ответ уходит ПОЛНОСТЬЮ внутри вызова: провайдер
    /// API обязан вернуть statusCode=0 (см. handleApiDev), иначе ядро
    /// пошлёт JSON поверх потока. false — файл не открыт/пустой вызов.
    bool streamFileDownload(fs::File& f, const char* downloadName);

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
    void handleApiConfigBackupInfo();  // admin GET: есть ли бэкап в NVS
    void handleApiConfigBackup();      // admin POST: снимок всех полей -> NVS
    void handleApiConfigRestore();     // admin POST: NVS -> поля + ребут
    void handleApiConfigExport();      // admin GET: ПОЛНЫЙ снимок (5.8.5, M3.3)
    void handleApiConfigImport();      // admin POST: снимок -> поля + ребут
    void handleApiLogs();              // admin: tail лога
    void handleApiAudit();             // admin: выгрузка /audit.log
    void handleApiReboot();            // admin
    void handleApiOtaInfo();           // admin: состояние OTA (залежь №3)
    void handleApiOtaCheck();          // admin: проверить манифест на HA
    void handleApiOtaUpdate();         // admin: фоновая загрузка (Phase 4)
    void handleApiOtaUploadDone();     // admin: финиш приёма образа (POST)
    void handleApiOtaUploadChunk();    // куски образа (upload-обработчик)
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
    uint32_t _netUpSinceMs = 0;   // 0 = сети нет (HTTP-гард, см. выше)
    IUiProvider* _ui = nullptr;

    // Сессии (RAM-only)
    struct Session { char token[HTTP_TOKEN_LEN]; uint32_t expiresMs; };
    Session  _sessions[HTTP_TOKEN_SLOTS];

    char     _currentToken[HTTP_TOKEN_LEN] = "";  // токен текущего запроса
    uint32_t _restartAtMs = 0;

    // Приём OTA-образа (залежь №3): флаги на время одного multipart-запроса.
    // WebServer шлёт куски в upload-обработчик, а HTTP-ответ формирует
    // финиш-обработчик — поэтому 401 здесь откладывается (флаг ниже),
    // иначе тело запроса задушило бы ответ.
    bool     _otaAuthFail = false;
    bool     _otaRxFailed = false;
    bool     _otaRxOk = false;

    // Кольцевой буфер аргументов для uiArgTrampoline (профиль читает
    // несколько аргументов подряд — одного буфера мало).
    // Размер — с запасом от МАКСИМАЛЬНОГО числа getArg в одном обработчике:
    // apiKeysAdd читает 6 (id,name,type,track,expiry_days,pin). При 4 слотах
    // 5-й/6-й аргумент ЗАТИРАЛ ранее прочитанные — id превращался в значение
    // pin («4266») и add() отвечал bad_id (жук добавления жильца с ПИНом).
    static constexpr uint8_t HTTP_ARG_RING = 8;
    char     _argRing[HTTP_ARG_RING][48];
    uint8_t  _argRingPos = 0;

    // Рабочие буферы. JSON — heap-указатель (выделяется ОДИН раз в init(),
    // new(std::nothrow): 16 КБ в BSS не влезают — урок переполнения dram0_0_seg
    // на 1832 байта). nullptr = аллокация не удалась → малый статический
    // запасной: API работает, но длинные схемы снова усечены (громкий лог
    // ConfigService::toJson это зафиксирует).
    char*    _jsonBuf = nullptr;
    char     _jsonBufFallback[2048];
    char*    jsonBuf()           { return _jsonBuf ? _jsonBuf : _jsonBufFallback; }
    size_t   jsonBufSize() const { return _jsonBuf ? HTTP_JSON_BUF
                                                   : sizeof(_jsonBufFallback); }
    char     _pageBuf[HTTP_PAGE_BUF];
};
