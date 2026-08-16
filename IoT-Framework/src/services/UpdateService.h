// ============================================================================
// UpdateService.h — OTA ОБНОВЛЕНИЕ С A/B-ОТКАТОМ (предложение A1)
// ============================================================================
// Фаза 1. Каркас защиты от «кирпича» после прошивки. ESP32 имеет штатную
// двухраздельную схему (ota_0/ota_1): новая прошивка пишется в неактивный
// раздел и стартует с флагом PENDING_VERIFY. Дальше два исхода:
//
//   · прошивка проработала VALIDATE_AFTER_MS без аварий -> помечаем VALID
//     (esp_ota_mark_app_valid_cancel_rollback) — она становится постоянной;
//   · прошивка упала/зависла до валидации -> загрузчик ESP32 САМ откатывается
//     на предыдущий раздел при следующей загрузке.
//
// В связке с bootloop-счётчиком Kernel'а это даёт полную цепочку A1:
//   bootloop >= 3 -> Safe Mode; свежая нестабильная прошивка -> авто-откат.
//
// Загрузка прошивки по сети (HTTP/HA, как в монолите v2.5.0) — Phase 2,
// когда появится NetworkManager. Здесь: механика разделов + совместимость.
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include <esp_ota_ops.h>

// Сколько свежая прошивка должна проработать, чтобы считаться валидной.
// Совпадает с KERNEL_STABLE_MS (60 с стабильной работы).
constexpr uint32_t OTA_VALIDATE_AFTER_MS = 60000;

// Залежь №3 (Phase 3): приём образа из веб-панели, проверка манифеста на
// сервере HA, журнал истории OTA. Манифест — паттерн монолита v2.5.0:
//   http://<mqtt.host>:8123/<sys.ota_path>/<sys.hostname>/version.json
//   {"version":"5.0.7","url":"firmware.bin","notes":"..."}
constexpr uint8_t  OTA_HISTORY_SIZE  = 5;    // глубина журнала истории
constexpr uint32_t OTA_HTTP_TIMEOUT_MS = 4000; // GET манифеста (блокирует loop!)
// Phase 4: фоновая загрузка образов самим устройством
constexpr uint32_t OTA_DL_TIMEOUT_MS   = 10000; // TCP/чтение HTTPClient
constexpr uint32_t OTA_DL_STALL_MS     = 10000; // нет байт — аварийный обрыв
constexpr size_t   OTA_DL_TICK_BUDGET  = 16384; // байт за тик (loop не душим)
constexpr uint32_t OTA_DL_MANIFEST_MAX_AGE_MS = 600000; // свежесть манифеста

/// Запись истории OTA (/ota_history.ndjson).
struct OtaHistoryEntry {
    uint32_t unix;
    uint32_t uptime_s;
    uint8_t  ok;          // 1 — успех
    char     src[10];     // "web", "boot", "rollback"
    char     type[6];     // "fw", "fs"
    uint32_t bytes;
    uint16_t dur_s;
    char     err[32];
    char     ver[24];     // версия из app descriptor образа ("" — не читана)
};

// Урок 5.0.14: запись ФС-образа СТИРАЕТ раздел вместе с /ota_history.ndjson
// (и всеми runtime-файлами без NVS-зеркал). Исход OTA складываем в NVS
// (переживает замену ФС) и проигрываем в историю при следующей загрузке.
constexpr uint32_t OTA_REPLAY_MAGIC = 0x4F544152UL;   // 'OTAR'
constexpr uint8_t  OTA_REPLAY_MAX   = 2;              // fw + fs за один сеанс
struct OtaReplayEntry {
    uint32_t unix;
    uint32_t bytes;
    uint16_t dur_s;
    uint8_t  ok;
    char     src[8];      // "remote" | "web"
    char     type[4];     // "fw" | "fs"
    char     ver[20];
    char     err[20];
};
struct OtaReplayPack {
    uint32_t magic;
    uint8_t  count;
    uint8_t  reserved[3];
    OtaReplayEntry e[OTA_REPLAY_MAX];
};

class UpdateService : public ModuleBase {
public:
    static UpdateService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "UpdateService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0008; }

    void registerExtensions() override;   // sys.ota_path (Phase 3)
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    // 100 мс — Phase 4: фоновая загрузка идёт кооперативными кусками в
    // tick (бюджет OTA_DL_TICK_BUDGET); в простое тело tick тривиально.
    uint32_t getTickIntervalMs() const override { return 100; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- СОСТОЯНИЕ ПРОШИВКИ ---------------------------------------------------
    /// true — текущая прошивка ещё не валидирована (свежая, после OTA).
    bool isPendingValidation() const { return _pendingValidation; }

    /// Версия прошивки (МикроОС — из core/Version.h, урок 5.0.10).
    const char* firmwareVersion() const { return _fwVersion; }
    /// Хэш сборки ядра из app descriptor (ee57070) — для диагностики,
    /// НЕ для различения версий МикроОС (он одинаков у всех сборок).
    const char* firmwareBuild() const { return _fwBuild; }

    // --- ОПЕРАЦИИ ------------------------------------------------------------
    /// Принудительный откат на предыдущий раздел + ребут
    /// (админская команда / ПАЗ при критической деградации свежей прошивки).
    void requestRollback();

    // --- WEB-UPLOAD (залежь №3) ----------------------------------------------
    /// Начало приёма образа из панели. command: U_FLASH / U_SPIFFS;
    /// size = 0 -> UPDATE_SIZE_UNKNOWN. Повторный begin обрывает старый приём.
    bool uploadBegin(int command, size_t size);
    /// Очередной кусок образа. false — запись не удалась (приём мёртв).
    bool uploadWrite(const uint8_t* data, size_t len);
    /// Завершение. true — образ цел и записан: можно ребутиться.
    bool uploadEnd();
    /// Аварийный обрыв (клиент отвалился / админ отменил).
    void uploadAbort();
    bool uploadActive() const { return _rxState == OtaRxState::Receiving; }
    /// Версия из app descriptor ПОСЛЕДНЕГО принятого образа ("" — не читана).
    const char* lastUploadedVersion() const { return _rxVersion; }

    // --- МАНИФЕСТ ОБНОВЛЕНИЙ (залежь №3) --------------------------------------
    /// GET манифеста version.json с сервера HA. URL: http://<mqtt.host>:8123/
    /// <sys.ota_path>/<sys.hostname>/version.json (схема монолита v2.5.0).
    /// ВНИМАНИЕ: блокирует loop до OTA_HTTP_TIMEOUT_MS — вызывать только
    /// из HTTP-обработчика (кнопка «Проверить обновление»), не из tick!
    bool checkRemote();
    bool updateAvailable() const { return _updateAvailable; }
    /// Отложенная проверка манифеста (из MQTT-команды): фактический GET
    /// выполнит tick() в контексте loop — HTTPClient в чужой задаче с
    /// маленьким стеком (esp-mqtt ~6 КБ) — игра с огнём.
    void requestRemoteCheck() { _checkRequested = true; }

    /// Полное состояние OTA для /api/ota/info (панель): версия, приём,
    /// манифест, фоновая загрузка, хвост истории. Возвращает длину JSON.
    size_t otaInfoJson(char* buf, size_t n) const;

    // --- УДАЛЁННОЕ ОБНОВЛЕНИЕ (Phase 4): скачать образы по манифесту ---------
    /// Скачать прошивку (а при withFs и наличии fs_url — и ФС) с сервера
    /// HA и записать в OTA-разделы. Запуск отложенный: работа идёт в tick()
    /// кооперативными кусками (loop не блокируется, WDT доволен, панель
    /// следит за dl в otaInfoJson). После успешной записи всех частей
    /// устройство САМО перезагружается через 1.5 с.
    void requestRemoteUpdate(bool withFs) {
        _dlWithFs = withFs;
        _dlRequested = true;
    }
    bool downloadActive() const {
        return _dlState == DlState::Fw || _dlState == DlState::Fs;
    }

private:
    UpdateService() = default;

    /// Эффективный URL манифеста (из mqtt.host + sys.ota_path + hostname).
    void manifestUrl(char* buf, size_t n) const;

    // --- История OTA (NDJSON /ota_history.ndjson, кольцо в RAM) ---------------
    void historyAdd(const char* src, const char* type, bool ok,
                    uint32_t bytes, uint16_t dur_s, const char* err,
                    const char* ver = "");
    void historyLoad();
    void historyPersist(const OtaHistoryEntry& e);
    // --- NVS-replay исхода OTA (переживает замену ФС, урок 5.0.14) ---------
    void replaySave(const char* src, const char* type, bool ok,
                    uint32_t bytes, uint16_t dur_s, const char* err,
                    const char* ver);
    void replayLoad();                 // из init(): история -> очистка NVS

    enum class OtaRxState : uint8_t { Idle, Receiving, Error };

    // --- Phase 4: фоновая загрузка образов самим устройством ----------------
    enum class DlState : uint8_t { Idle, Fw, Fs, Done, Failed };
    void dlStart();                    // вход по _dlRequested (из tick)
    void dlTick();                     // один бюджетный кусок за вызов
    bool dlBeginPart(bool fs);         // открыть HTTP + Update на часть
    void dlFinishPart();               // Update.end + следующая часть/ребут
    void dlFail(const char* err);      // авария: история, уборка, Failed
    void dlCleanup();                  // закрыть HTTPClient (heap)
    void urlResolve(const char* src, char* out, size_t n) const;

    bool _pendingValidation = false;   // раздел в состоянии PENDING_VERIFY
    uint32_t _bootMs = 0;              // отсчёт стабильной работы
    char _fwVersion[32] = "unknown";   // версия МикроОС (core/Version.h)
    char _fwBuild[16] = "?";           // git-хэш ядра из esp_app_desc_t

    // Приём образа из панели
    OtaRxState _rxState = OtaRxState::Idle;
    size_t   _rxBytes = 0;
    int      _rxCommand = 0;           // U_FLASH / U_SPIFFS (для истории)
    size_t   _rxSize = 0;              // заявленный размер (0 — неизвестен)
    bool     _rxBegun = false;         // Update.begin СДЕЛАН (урок 5.0.14:
                                       // отложен до проверки магии головы)
    uint32_t _rxStartMs = 0;
    char     _rxError[48] = "";
    // Голова образа (первые байты): магия ESP32 + esp_app_desc_t — защита
    // от заливки «не того» файла (урок 5.0.7: панель принимала любой .bin,
    // старая прошивка + новая FS = not_found/ERR_CONTENT_DECODING).
    uint8_t  _rxHead[80];
    uint8_t  _rxHeadLen = 0;
    char     _rxVersion[33] = "";      // версия из app descriptor образа

    // Манифест обновлений
    bool     _updateAvailable = false;
    char     _remoteVersion[24] = "";
    char     _remoteNotes[48] = "";    // changelog из манифеста
    char     _remoteFwUrl[72] = "";    // fw_url (Phase 4: загрузка силами устр-ва)
    char     _remoteFsUrl[72] = "";    // fs_url
    char     _remoteMd5[36] = "";      // md5 прошивки (опционально)
    char     _remoteFsMd5[36] = "";    // fs_md5 (опционально)
    char     _remoteMinFs[12] = "";    // min_fs_version (маркер совместимости A1)
    uint32_t _lastCheckMs = 0;
    bool     _checkRequested = false;  // отложенная проверка (MQTT)

    // Phase 4: состояние фоновой загрузки
    DlState  _dlState = DlState::Idle;
    bool     _dlRequested = false;     // отложенный запуск (HTTP/MQTT -> tick)
    bool     _dlWithFs = false;        // качать и ФС (если есть fs_url)
    bool     _dlFwOk = false;          // fw записан в этом сеансе
    void*    _dlHttp = nullptr;        // HTTPClient* (heap — не в BSS!)
    bool     _dlBegun = false;         // Update.begin сделан (урок 5.0.14:
                                       // отложен до магии первого куска)
    uint32_t _dlBytes = 0;             // принято по текущей части
    int32_t  _dlTotal = -1;            // Content-Length (-1 — неизвестен)
    uint32_t _dlLastRxMs = 0;          // watchdog стойла (нет байт)
    uint32_t _dlStartMs = 0;           // начало части (dur_s в историю)
    char     _dlErr[48] = "";          // причина Failed для панели
    uint32_t _restartAtMs = 0;         // авторебут после успеха

    // История
    OtaHistoryEntry _history[OTA_HISTORY_SIZE];
    uint8_t  _historyHead = 0;
    uint8_t  _historyCount = 0;
    bool     _historyLoaded = false;   // ленивая загрузка (порядок модулей)
};
