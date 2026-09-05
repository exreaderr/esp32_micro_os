// BackupService.h — M3.3 BackupAggregator: снимки конфигов парка на SD
// ============================================================================
// Идея (боль владельца 04.09.2026): NVS-бэкап на самом устройстве спасает
// от перепрошивки, но не от «настроил криво и не помню что». Мастер
// периодически (bk.period_days, дефолт 7 дней) забирает ПОЛНЫЕ снимки
// конфигов устройств (GET /api/config/export, ядро 5.8.5) и хранит историю
// на SD: /backup/<ip>/config-YYYYMMDD-HHMMSS.json + latest.json.
// Внеплановый снимок: ~5 мин после события CFG_CHANGED любого устройства
// (bk.on_change). Восстановление — POST тела снимка на /api/config/import
// устройства (то же ядро 5.8.5), инициатор — панель мастера.
//
// СЕКРЕТЫ: снимки полные (решение владельца 04.09.2026 — контур проводной
// и доверенный; флэш устройств и так хранит секреты открыто, SD мастера =
// второй экземпляр того же класса хранения). SD мастера = физический
// периметр щитка; перенос карты = перенос конфигурации всего парка
// («флот у соседа» — сценарий владельца).
//
// Fail-safe и стражи:
//   · время недостоверно (нет NTP/DS3231) — плановый съём НЕ делаем
//     (имя файла было бы ложью; как purge в замке — без времени молчим);
//   · 401 bad_pin от устройства — хост блокируется до смены bk.admin_pin
//     (rate-limiter устройства после 5 неудач закрыл бы админку всем —
//     мастер не имеет права быть источником блокировки);
//   · SD не смонтирована — тихая деградация (ПАЗ hm.bk), без падений;
//   · запись снимка tmp+rename (дёрганье питания ≠ битый latest);
//   · устройство офлайн/старого ядра (<5.8.5, нет export) — ошибка в
//     статус хоста, остальные обрабатываются.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <core/ShTypes.h>

struct BrokerEventInfo;   // BrokerService.h — только в .cpp (шапка лёгкая)

class BackupService : public ModuleBase {
public:
    static BackupService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "BackupService"; }
    const char* getVersion() const override { return "0.2.0-m33"; }
    // 0.2.0 (0.6.2, bk.self): мастер бэкапит и СЕБЯ — псевдохост "self"
    // первым в цикле, локально (exportSnapshotJson, без HTTP/пароля),
    // в /backup/self/. Восстановление: applySnapshotJson + отложенный
    // ребут мастера (ответ содержит reboot_in_ms — панель покажет
    // оверлей перезагрузки). Закрывает дыру M3.3: флот бэкапился,
    // а его глава — нет (владелец 05.09: «делаем и бэкап мастера,
    // всё-таки во главе флота»).
    ModuleId getModuleId() const override { return 0x1107; }   // hm: 0x1101=Sd, 0x1102=App, 0x1103=Broker, 0x1104=Bridge, 0x1105=WxMirror, 0x1106=Journal
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- API для панели (HomeMasterUi) ------------------------------------
    /// Список хостов и статусов: {"hosts":[{"ip":"...","lastOk":U,
    /// "lastErr":"...","files":N,"blocked":0/1},...],"nextIn":секунды}
    size_t apiStatus(char* buf, size_t bufSize);
    /// Список файлов снимков хоста: {"ip":"...","files":["config-...json",...]}
    size_t apiFiles(const char* ip, char* buf, size_t bufSize);
    /// Ручной съём: ip — конкретный или "all". Возврат: ok + статус.
    size_t apiSnapshot(const char* ip, char* buf, size_t bufSize);
    /// Развернуть снимок файла на устройство ip (устройство перезагрузится).
    size_t apiRestore(const char* ip, const char* file, char* buf, size_t bufSize);

    /// Хук брокера (multi-hook, как у журнала): CFG_CHANGED из парка.
    static void onBrokerEvent(const BrokerEventInfo& info);

    // --- Для ПАЗ (hm.bk) ---------------------------------------------------
    uint32_t lastRunUnix() const { return _lastRunUnix; }
    bool     isEnabled() const { return _enabled; }
    /// Сколько хостов в беде (blocked или lastErr); первую — в out
    /// ("ip:err" / "ip:blocked"). 0 — всё чисто.
    uint8_t  troubleCount(char* out, size_t cap) const;

private:
    BackupService() = default;

    static constexpr uint8_t  BK_MAX_HOSTS     = 6;
    static constexpr uint32_t CYCLE_GAP_MS     = 15000;    // между хостами в цикле
    static constexpr uint32_t ON_CHANGE_DELAY_MS = 300000; // ~5 мин после CFG_CHANGED
    static constexpr size_t   BK_SNAPSHOT_CAP  = 96 * (32 + 48 + 6) + 16;  // = CFG_SNAPSHOT_CAP ядра

    struct HostState {
        char     ip[20]      = "";
        uint32_t lastOkUnix  = 0;        // последний удачный снимок
        char     lastErr[32] = "";       // "http_401" / "offline" / ...
        uint8_t  files       = 0;        // снимков на SD (оценка, при статусе)
        bool     blocked     = false;    // 401: ждём смену bk.admin_pin
    };

    void     reloadHosts();              // bk.hosts -> _hosts[] (слот 0 — "self")
    int      findHost(const char* ip) const;
    void     stepSelf(HostState& h);     // 0.6.2: снимок самого мастера (локально)
    /// Запуск цикла: only >= 0 — точечно один хост (ручной «Снять»),
    /// иначе весь список. Цикл идёт [_cycleIdx, _cycleEnd).
    void     scheduleCycle(uint32_t delayMs, int8_t only = -1);
    bool     timeValid() const;
    void     runCycleStep();             // конечный автомат съёма (по хосту за шаг)
    bool     loginHost(const char* ip, char* token, size_t tokCap,
                       char* err, size_t errCap);
    bool     fetchSnapshot(const char* ip, const char* token,
                           char* buf, size_t cap, size_t* outLen,
                           char* err, size_t errCap);
    bool     pushSnapshot(const char* ip, const char* file,
                          char* err, size_t errCap);
    bool     storeSnapshot(const char* ip, const char* data, size_t len,
                           char* err, size_t errCap);
    void     rotate(const char* ip, uint8_t keep);
    static bool ipListed(const char* csv, const char* ip);   // "a,b,c" без пробелов

    HostState _hosts[BK_MAX_HOSTS];
    uint8_t   _hostCount     = 0;
    bool      _enabled       = false;

    // Конечный автомат цикла: _cycleIdx < 0 — цикла нет; 0..N-1 — хост шага.
    int8_t    _cycleIdx      = -1;
    int8_t    _cycleEnd      = -1;       // конец диапазона (точечный = idx+1)
    uint32_t  _nextStepAtMs  = 0;        // когда обрабатывать _hosts[_cycleIdx]
    // Плановое расписание — по unix-времени: период до 90 суток в millis
    // переполнил бы uint32 (урок: интервалы длиннее 49 суток — только unix).
    // 0 = ещё не назначено (ждём достоверного времени; тогда = now + 10 мин
    // — первый съём вскоре после boot, потом уже +bk.period_days).
    uint32_t  _nextPlanUnix  = 0;
    bool      _onChangeDue   = false;    // был CFG_CHANGED — ждём 5 мин
    uint32_t  _onChangeAtMs  = 0;

    uint32_t  _lastRunUnix   = 0;        // для «следующий через»
    uint32_t  _selfRebootAtMs = 0;       // 0.6.2: отложенный ребут после self-restore
};
