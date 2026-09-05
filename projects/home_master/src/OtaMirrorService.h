// OtaMirrorService.h — 0.6.1: OTA-ЗЕРКАЛО ПАРКА НА МАСТЕРЕ
// ============================================================================
// Идея (владелец 05.09.2026): «если мастер проверял бы наличие новых версий
// прошивок на верхнем брокере и зеркалировал их себе. Тогда логическая
// цепочка замыкается, и прошивка и бэкап лежат на мастере. Не нужно
// указывать адрес верхнего брокера. Система становится ещё более
// самодостаточной».
//
// Почему ядро не тронуто: UpdateService устройств строит URL манифеста из
// СВОЕГО mqtt.host: http://<mqtt.host>:8123/<sys.ota_path>/<hostname>/
// version.json. У устройств парка mqtt.host УЖЕ = мастер (.54) — они и так
// спрашивают мастера на порту 8123. Значит, достаточно, чтобы мастер:
//   1) зеркалировал: по расписанию (otam.period_h, дефолт 6 ч) опрашивает
//      HA (СВОЙ mqtt.host = верхний брокер 10.146.75.5:8123), забирает
//      version.json + firmware.bin + littlefs.bin каждого хоста парка и
//      кладёт на SD: /ota/<hostname>/. Манифест пишется ПОСЛЕДНИМ, после
//      проверки md5 бинарей (манифест = точка коммита: дёрганье питания
//      посреди скачивания оставляет старую целостную пару);
//   2) раздавал: второй WebServer на :8123 отдаёт РОВНО whitelist
//      /local/ota/<host>/(version.json|firmware.bin|littlefs.bin) с SD.
//      Без авторизации: прошивка — не секрет (решение владельца 04.09 про
//      полные снимки покрывает класс), устройства токенов не шлют;
//      сервер read-only, traversal-страж (нет ".."), хосты — только из
//      otam.hosts. Смена конфига устройств НЕ требуется.
//
// Мастер сам себя не зеркалирует: его mqtt.host = HA, он и так обновляется
// с HA напрямую. otam.hosts — hostname-ы ПАРКА (sys.hostname устройств).
//
// Fail-safe:
//   · SD не смонтирована — тихая деградация (ПАЗ hm.ota), без падений;
//   · HA офлайн/404 — ошибка в статус хоста, раздача ранее снятого
//     зеркала продолжается (устройства обновляются и без HA, и без WAN);
//   · md5 из манифеста (fw_md5/fs_md5) проверяется ПОСЛЕ скачивания на SD,
//     битый файл не подменяет рабочий (tmp + verify + rename);
//   · пока зеркало хоста пусто (ещё не скачано) — раздача даёт 404,
//     устройство мягко пишет history и живёт дальше (штатный путь ядра).
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <core/ShTypes.h>
#include <WebServer.h>

class OtaMirrorService : public ModuleBase {
public:
    static OtaMirrorService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "OtaMirrorService"; }
    const char* getVersion() const override { return "0.1.0-om1"; }
    ModuleId getModuleId() const override { return 0x1108; }   // hm: ... 0x1107=Backup, 0x1108=OtaMirror
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- API для панели (HomeMasterUi) ------------------------------------
    /// {"enabled":1,"period_h":6,"nextIn":сек,"hosts":[{"host":"...",
    /// "version":"x.y.z","lastOkUnix":U,"lastErr":"...","fwSize":N,
    /// "fsSize":N},...]}
    size_t apiStatus(char* buf, size_t bufSize);
    /// Ручная проверка: host — конкретный hostname или "all".
    size_t apiCheck(const char* host, char* buf, size_t bufSize);

    // --- Для ПАЗ (hm.ota) --------------------------------------------------
    bool     isEnabled() const { return _enabled; }
    uint32_t lastRunUnix() const { return _lastRunUnix; }
    /// Сколько хостов в беде (lastErr непуст и версии нет совсем — т.е.
    /// зеркало для хоста ни разу не собрано; ошибка опроса при ЖИВОМ
    /// зеркале — не беда, раздача работает). Первую — в out "host:err".
    uint8_t  troubleCount(char* out, size_t cap) const;

private:
    OtaMirrorService() = default;

    static constexpr uint8_t  OM_MAX_HOSTS   = 6;
    static constexpr uint32_t CYCLE_GAP_MS   = 5000;      // между хостами в цикле
    static constexpr uint32_t FIRST_POLL_MS  = 120000;    // 1-й опрос через 2 мин после boot
    static constexpr size_t   MANIFEST_CAP   = 2048;      // version.json — маленький
    static constexpr uint16_t OTA_PORT       = 8123;      // как у HA (урок: порт зашит в ядре устройств)

    struct HostState {
        char     host[24]     = "";      // sys.hostname устройства
        char     version[20]  = "";      // зеркалированная версия ("" = зеркала нет)
        uint32_t lastOkUnix   = 0;       // последняя удачная сверка/закачка
        char     lastErr[24]  = "";      // "offline"/"http_404"/"md5_fw"/...
        uint32_t fwSize       = 0;
        uint32_t fsSize       = 0;
    };

    void     reloadHosts();              // otam.hosts -> _hosts[] (стейты сохраняются)
    int      findHost(const char* host) const;
    void     scheduleCycle(uint32_t delayMs, int8_t only = -1);
    void     runCycleStep();             // конечный автомат: одна ФАЗА хоста за тик
    void     finishHost(HostState& h, const char* err);   // итог хоста + следующий

    // --- одна итерация по хосту -------------------------------------------
    bool     fetchManifest(const char* host, char* buf, size_t cap,
                           char* err, size_t errCap);
    bool     downloadBin(const char* host, const char* url,
                         const char* localName, const char* expectMd5,
                         uint32_t* outSize, char* err, size_t errCap);
    bool     commitBin(const char* host, const char* localName,
                       char* err, size_t errCap);    // tmp -> финал (rename)
    bool     storeManifest(const char* host, const char* data, size_t len,
                           char* err, size_t errCap);
    static bool hostListed(const char* csv, const char* host);
    /// urlResolve-семантика ядра: абсолютный как есть; "/path" — от корня HA;
    /// имя файла — от каталога манифеста этого хоста.
    void     urlResolve(const char* host, const char* src, char* out, size_t n) const;
    static bool md5FileOk(fs::File& f, const char* expectHex);  // MD5Builder ядра Arduino

    // --- раздача :8123 -----------------------------------------------------
    void     handleOtaHttp();            // onNotFound: whitelist-раздача с SD

    HostState _hosts[OM_MAX_HOSTS];
    uint8_t   _hostCount   = 0;
    bool      _enabled     = false;

    // Конечный автомат опроса (как у BackupService): _cycleIdx < 0 — цикла
    // нет; 0..N-1 — хост шага. Период в ЧАСАХ в millis помещается в uint32
    // (168 ч = 605 млн < 4,3 млрд) — unix здесь не нужен, в отличие от
    // bk.period_days (урок 49 суток там актуален, тут — нет).
    int8_t    _cycleIdx     = -1;
    int8_t    _cycleEnd     = -1;
    uint32_t  _nextStepAtMs = 0;
    // 0.6.1-fix (приёмка 05.09): шаг хоста разбит на ФАЗЫ по одной за тик
    // (0=манифест, 1=fw, 2=fs, 3=коммит-манифеста). Причина: WDT loopTask
    // 10 с, а сумма «манифест + 2 скачивания» в одном тике её превышала;
    // хуже того, цикл скачивания по connected() ВИС на keep-alive HA —
    // сторож перезагружал кристалл каждые ~2 минуты (урок: ручной цикл
    // чтения HTTP выходит по Content-Length/закрытию, не по connected()).
    uint8_t   _phase        = 0;
    char      _manifest[MANIFEST_CAP];   // манифест текущего хоста (между фазами)
    char      _pVer[20], _pFwUrl[160], _pFsUrl[160], _pFwMd5[40], _pFsMd5[40];
    uint32_t  _nextPollAtMs = 0;         // 0 = назначить FIRST_POLL_MS при 1-м tick
    uint32_t  _lastRunUnix  = 0;

    WebServer _otaServer{OTA_PORT};      // второй сервер, раздача зеркала
    bool      _serverUp   = false;
};
