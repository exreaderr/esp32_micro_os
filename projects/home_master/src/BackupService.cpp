// BackupService.cpp — M3.3 BackupAggregator (см. шапку .h)
// ============================================================================
#include "BackupService.h"
#include "BrokerService.h"
#include "SdService.h"
#include <core/EventBus.h>
#include <core/Events.h>
#include <core/Version.h>
#include <services/ConfigService.h>
#include <services/TimeService.h>
#include <services/NetworkManager.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <time.h>

BackupService& BackupService::getInstance() {
    static BackupService inst;
    return inst;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void BackupService::init() {
    _enabled = cfgGetBool("bk.enabled", true);
    reloadHosts();
    // Смена bk.admin_pin снимает блокировку хостов (причина блокировки —
    // именно он), bk.hosts — перечитываем список, bk.enabled — живой вкл/выкл.
    EventBus::getInstance().subscribe(CFG_EVENT_CHANGED, this);
}

void BackupService::start() {
    // Хук вешаем ВСЕГДА: bk.enabled — живое поле (CFG_NONE), при его
    // включении в рантайме подписка уже должна стоять (включение модуля
    // без ребута — закон живых полей).
    BrokerService::getInstance().addEventHook(&BackupService::onBrokerEvent);
}

void BackupService::stop() {
    BrokerService::getInstance().removeEventHook(&BackupService::onBrokerEvent);
}

// ============================================================================
// КОНФИГ
// ============================================================================
bool BackupService::ipListed(const char* csv, const char* ip) {
    // "10.0.0.1,10.0.0.2" без пробелов; сравнение по целому элементу
    const char* p = csv;
    size_t n = strlen(ip);
    while (p != nullptr && *p != '\0') {
        const char* comma = strchr(p, ',');
        size_t len = comma != nullptr ? (size_t)(comma - p) : strlen(p);
        if (len == n && strncmp(p, ip, n) == 0) return true;
        p = comma != nullptr ? comma + 1 : nullptr;
    }
    return false;
}

void BackupService::reloadHosts() {
    char csv[CFG_VALUE_LEN];
    cfgGetStr("bk.hosts", csv, sizeof(csv), "10.146.75.53,10.146.75.55");
    // Свой IP — чтобы вычеркнуть его из bk.hosts (урок 0.6.2-пр1 ниже).
    char ownIp[20];
    NetworkService::getInstance().ipString(ownIp, sizeof(ownIp));
    // Состояние (lastOk/blocked) переживает перечитывание: ищем по ip.
    HostState fresh[BK_MAX_HOSTS];
    uint8_t freshCount = 0;
    // 0.6.2, bk.self: слот 0 ВСЕГДА сам мастер ("self" — локальный снимок,
    // без сети). ПРИЧИНА ЖЁСТКОСТИ (урок 0.6.2-пр1): если IP самого мастера
    // стоит в bk.hosts обычной строкой, "Снять сейчас" посылает мастера
    // по HTTP к САМОМУ СЕБЕ — однопоточный WebServer ждёт сам себя,
    // дедлок, TWDT 10 с, task_wdt-ребут (пойман в приёмке 05.09).
    int oldSelf = findHost("self");
    if (oldSelf >= 0) fresh[freshCount] = _hosts[oldSelf];
    else strncpy(fresh[freshCount].ip, "self", sizeof(fresh[freshCount].ip) - 1);
    freshCount++;
    char* save = nullptr;
    for (char* t = strtok_r(csv, ",", &save);
         t != nullptr && freshCount < BK_MAX_HOSTS;
         t = strtok_r(nullptr, ",", &save)) {
        // "self" и собственный IP мастера — уже покрыты слотом 0, не плодим
        // сетевой слот на самих себя (см. урок выше).
        if (strcmp(t, "self") == 0 || strcmp(t, ownIp) == 0) continue;
        int old = findHost(t);
        if (old >= 0) fresh[freshCount] = _hosts[old];
        else {
            strncpy(fresh[freshCount].ip, t, sizeof(fresh[freshCount].ip) - 1);
            fresh[freshCount].ip[sizeof(fresh[freshCount].ip) - 1] = '\0';
        }
        freshCount++;
    }
    memcpy(_hosts, fresh, sizeof(_hosts));
    _hostCount = freshCount;
}

int BackupService::findHost(const char* ip) const {
    for (uint8_t i = 0; i < _hostCount; ++i) {
        if (strncmp(_hosts[i].ip, ip, sizeof(_hosts[i].ip)) == 0) return (int)i;
    }
    return -1;
}

bool BackupService::timeValid() const {
    return TimeService::getInstance().getUnixTime() >= 1700000000UL;
}

// ============================================================================
// ШИНА
// ============================================================================
bool BackupService::canHandleEvent(int32_t eventId) const {
    return eventId == CFG_EVENT_CHANGED;
}

void BackupService::onEvent(int32_t eventId, const ShEventData* data) {
    if (eventId != CFG_EVENT_CHANGED || data == nullptr) return;
    if (strcmp(data->payload, "bk.admin_pin") == 0) {
        for (uint8_t i = 0; i < _hostCount; ++i) _hosts[i].blocked = false;
        log(LogLevel::Info, "bk: пароль сменили — блокировки хостов сняты");
    } else if (strcmp(data->payload, "bk.hosts") == 0) {
        reloadHosts();
        log(LogLevel::Info, "bk: список хостов перечитан (%u)", _hostCount);
    } else if (strcmp(data->payload, "bk.enabled") == 0) {
        _enabled = cfgGetBool("bk.enabled", true);
        log(LogLevel::Info, "bk: модуль %s", _enabled ? "включён" : "выключен");
    }
}

// ============================================================================
// ХУК БРОКЕРА: CFG_CHANGED из парка -> внеплановый цикл через ~5 минут.
// Ловим и СВОИ события (мастер тоже публикует в локальный брокер) — лишний
// цикл от события мастера безвреден, а фильтрация по deviceId потребовала
// бы карту id->ip: не надо. СИНХРОННО, контекст tick брокера: только флаг
// и таймер, HTTP/SD здесь НЕЛЬЗЯ (как у журнала M3.1).
// ============================================================================
void BackupService::onBrokerEvent(const BrokerEventInfo& info) {
    BackupService& self = getInstance();
    if (!self._enabled || !cfgGetBool("bk.on_change", true)) return;
    if (info.type != BrokerEventInfo::Publish) return;
    const char* tail = strrchr(info.topic, '/');
    if (tail == nullptr || strcmp(tail + 1, "CFG_CHANGED") != 0) return;
    if (self._onChangeDue) return;   // дребезг серии правок — одна метка
    self._onChangeDue  = true;
    self._onChangeAtMs = millis() + ON_CHANGE_DELAY_MS;
}

// ============================================================================
// TICK — конечный автомат
// ============================================================================
void BackupService::scheduleCycle(uint32_t delayMs, int8_t only) {
    if (_cycleIdx >= 0) return;   // цикл уже идёт — не плодим
    if (only >= 0 && only < (int8_t)_hostCount) {
        _cycleIdx = only;
        _cycleEnd = (int8_t)(only + 1);
    } else {
        _cycleIdx = 0;
        _cycleEnd = (int8_t)_hostCount;
    }
    _nextStepAtMs = millis() + delayMs;
}

void BackupService::tick() {
    // 0.6.2, bk.self: отложенный ребут после восстановления СЕБЯ —
    // срабатывает даже при bk.enabled=false (ответ панели уже ушёл).
    if (_selfRebootAtMs != 0 && (int32_t)(millis() - _selfRebootAtMs) >= 0) {
        log(LogLevel::Info, "bk: self-restore применён — перезагрузка");
        delay(50);
        ESP.restart();
    }
    if (!_enabled) return;
    uint32_t now = millis();

    // 1) Идущий цикл — по хосту за шаг (HTTP блокирующий, разносим).
    if (_cycleIdx >= 0) {
        if ((int32_t)(now - _nextStepAtMs) >= 0) runCycleStep();
        return;
    }

    // 2) Внеплановый цикл (CFG_CHANGED в парке).
    if (_onChangeDue) {
        if ((int32_t)(now - _onChangeAtMs) >= 0) {
            _onChangeDue = false;
            if (timeValid()) {
                log(LogLevel::Info, "bk: внеплановый цикл (CFG_CHANGED в парке)");
                scheduleCycle(0);
            }
        }
        return;
    }

    // 3) Плановый цикл — по unix-времени (см. _nextPlanUnix в .h).
    if (!timeValid()) return;   // без времени планового расписания нет
    uint32_t nowU = (uint32_t)TimeService::getInstance().getUnixTime();
    if (_nextPlanUnix == 0) {
        _nextPlanUnix = nowU + 600;   // первый съём через 10 мин после boot
        return;
    }
    if (nowU >= _nextPlanUnix) {
        _nextPlanUnix = nowU + (uint32_t)cfgGetInt("bk.period_days", 7) * 86400UL;
        log(LogLevel::Info, "bk: плановый цикл съёма");
        scheduleCycle(0);
    }
}

// ============================================================================
// 0.6.2, bk.self: снимок САМОГО мастера — локально (exportSnapshotJson),
// без HTTP и пароля. Те же tmp+rename и ротация, что у парка.
// ============================================================================
void BackupService::stepSelf(HostState& h) {
    if (SdService::getInstance().fs() == nullptr) {
        strncpy(h.lastErr, "no_sd", sizeof(h.lastErr) - 1);
        h.lastErr[sizeof(h.lastErr) - 1] = '\0';
        return;
    }
    // Буфер из кучи на время операции (урок saveToJson: стек loop 8 КБ).
    char* snap = (char*)malloc(BK_SNAPSHOT_CAP);
    if (snap == nullptr) {
        strncpy(h.lastErr, "no_heap", sizeof(h.lastErr) - 1);
        h.lastErr[sizeof(h.lastErr) - 1] = '\0';
        return;
    }
    // 0.6.6: имя хозяина снимка — для карточки (IP + hostname).
    cfgGetStr("sys.hostname", h.host, sizeof(h.host), "");
    size_t len = ConfigService::getInstance().exportSnapshotJson(snap, BK_SNAPSHOT_CAP);
    char err[24] = "export";
    bool ok = (len > 0) && storeSnapshot("self", snap, len, err, sizeof(err));
    free(snap);
    if (ok) {
        h.lastOkUnix = (uint32_t)TimeService::getInstance().getUnixTime();
        h.lastErr[0] = '\0';
        rotate("self", "config-", (uint8_t)cfgGetInt("bk.keep", 10));
        log(LogLevel::Info, "bk: self снимок сохранён (%u Б)", (unsigned)len);
    } else {
        strncpy(h.lastErr, err, sizeof(h.lastErr) - 1);
        h.lastErr[sizeof(h.lastErr) - 1] = '\0';
        log(LogLevel::Warning, "bk: self снимок не удался (%s)", err);
    }
}

// 0.6.6: вытащить "sys.hostname" из снимка (простой скан, без парсера:
// hostname — простое значение без экранирования, по построению).
static void extractHostname(const char* json, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return;
    out[0] = '\0';
    if (json == nullptr) return;
    const char* p = strstr(json, "\"sys.hostname\"");
    if (p == nullptr) return;
    p = strchr(p + 14, ':');
    if (p == nullptr) return;
    p = strchr(p + 1, '"');
    if (p == nullptr) return;
    const char* e = strchr(p + 1, '"');
    if (e == nullptr || e <= p + 1) return;
    size_t n = (size_t)(e - (p + 1));
    if (n >= cap) n = cap - 1;
    memcpy(out, p + 1, n);
    out[n] = '\0';
}

void BackupService::runCycleStep() {
    if (_cycleIdx < 0) return;
    if (_cycleIdx >= _cycleEnd || _cycleIdx >= (int8_t)_hostCount) {
        _cycleIdx    = -1;
        _cycleEnd    = -1;
        _lastRunUnix = (uint32_t)TimeService::getInstance().getUnixTime();
        return;
    }
    HostState& h = _hosts[_cycleIdx];
    _cycleIdx++;
    _nextStepAtMs = millis() + CYCLE_GAP_MS;

    if (h.ip[0] == '\0') return;
    if (strcmp(h.ip, "self") == 0) { stepSelf(h); return; }
    // Урок 0.6.3-пр1: reloadHosts бежит при init, когда DHCP ещё НЕ дал IP
    // (ownIp="0.0.0.0"), поэтому собственный IP может просочиться в список
    // сетевым слотом. Проверяем «а не я ли это» ЗДЕСЬ, в момент цикла,
    // когда IP уже известен: самих себя снимаем локально, без сети.
    // Иначе: HTTP к самому себе → дедлок/зависание → TWDT-ребут ПОСРЕДИ
    // OTA-раздачи зеркала (поймано в приёмке 06.09: stall_timeout у шлюза).
    {
        char ownIp[20];
        NetworkService::getInstance().ipString(ownIp, sizeof(ownIp));
        if (ownIp[0] != '\0' && strcmp(ownIp, "0.0.0.0") != 0 &&
            strcmp(h.ip, ownIp) == 0) {
            log(LogLevel::Info, "bk: %s — это я сам, снимаю локально", h.ip);
            stepSelf(h);
            return;
        }
    }
    if (h.blocked) {
        log(LogLevel::Warning, "bk: %s пропущен (ждёт смены bk.admin_pin)", h.ip);
        return;
    }
    if (SdService::getInstance().fs() == nullptr) {
        strncpy(h.lastErr, "no_sd", sizeof(h.lastErr) - 1);
        h.lastErr[sizeof(h.lastErr) - 1] = '\0';
        return;   // ПАЗ hm.sd уже кричит; дублировать тревогу не надо
    }

    char token[16];
    char err[24] = "";
    if (!loginHost(h.ip, token, sizeof(token), err, sizeof(err))) {
        strncpy(h.lastErr, err, sizeof(h.lastErr) - 1);
        h.lastErr[sizeof(h.lastErr) - 1] = '\0';
        if (strcmp(err, "http_401") == 0) {
            h.blocked = true;
            log(LogLevel::Warning,
                "bk: %s отклонил пароль — хост заблокирован до смены bk.admin_pin",
                h.ip);
        } else {
            log(LogLevel::Warning, "bk: %s вход не удался (%s)", h.ip, err);
        }
        return;
    }

    // Буфер снимка — из кучи на время операции (урок saveToJson: стек
    // loop-задачи 8 КБ, статика — BSS; транзиентам — куча).
    char* snap = (char*)malloc(BK_SNAPSHOT_CAP);
    if (snap == nullptr) {
        strncpy(h.lastErr, "no_heap", sizeof(h.lastErr) - 1);
        h.lastErr[sizeof(h.lastErr) - 1] = '\0';
        return;
    }
    size_t len = 0;
    bool ok = fetchSnapshot(h.ip, token, snap, BK_SNAPSHOT_CAP, &len,
                            err, sizeof(err));
    if (ok) extractHostname(snap, h.host, sizeof(h.host));   // 0.6.6: имя для карточки
    if (ok) ok = storeSnapshot(h.ip, snap, len, err, sizeof(err));
    free(snap);

    if (ok) {
        h.lastOkUnix = (uint32_t)TimeService::getInstance().getUnixTime();
        h.lastErr[0] = '\0';
        rotate(h.ip, "config-", (uint8_t)cfgGetInt("bk.keep", 10));
        log(LogLevel::Info, "bk: %s снимок сохранён (%u Б)", h.ip, (unsigned)len);
        stepExtras(h.ip, token);   // 0.9.5: доп.снимки (база замка и пр.)
    } else {
        strncpy(h.lastErr, err, sizeof(h.lastErr) - 1);
        h.lastErr[sizeof(h.lastErr) - 1] = '\0';
        log(LogLevel::Warning, "bk: %s снимок не удался (%s)", h.ip, err);
    }
}

// ============================================================================
// HTTP-КЛИЕНТ К УСТРОЙСТВУ (контекст tick — там HTTPClient безопасен,
// урок UpdateService: «в loop, где HTTPClient безопасен по стеку»)
// ============================================================================
bool BackupService::loginHost(const char* ip, char* token, size_t tokCap,
                              char* err, size_t errCap) {
    // Страховка (урок 0.6.3-пр1): никогда не ходим по сети на собственный
    // IP — даже если он каким-то путём оказался в списке хостов.
    {
        char ownIp[20];
        NetworkService::getInstance().ipString(ownIp, sizeof(ownIp));
        if (ownIp[0] != '\0' && strcmp(ownIp, "0.0.0.0") != 0 &&
            strcmp(ip, ownIp) == 0) {
            snprintf(err, errCap, "self_ip");
            return false;
        }
    }
    char pin[CFG_VALUE_LEN];
    cfgGetStr("bk.admin_pin", pin, sizeof(pin), "");
    if (pin[0] == '\0') {
        snprintf(err, errCap, "no_pin");
        return false;
    }
    char url[48];
    snprintf(url, sizeof(url), "http://%s/api/auth", ip);
    HTTPClient http;
    http.setTimeout(4000);
    if (!http.begin(url)) { snprintf(err, errCap, "begin"); return false; }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    char body[CFG_VALUE_LEN + 8];
    snprintf(body, sizeof(body), "pin=%s", pin);
    pin[0] = '\0';   // пароль из RAM — сразу (дисциплина секретов)
    int code = http.POST((uint8_t*)body, strlen(body));
    if (code == 401) { http.end(); snprintf(err, errCap, "http_401"); return false; }
    if (code != 200) {
        http.end();
        snprintf(err, errCap, code <= 0 ? "offline" : "http_%d", code);
        return false;
    }
    // Ответ {"token":"xxxxxxxx"} — выдёргиваем без JSON-парсера (8 символов)
    String resp = http.getString();
    http.end();
    int a = resp.indexOf("\"token\":\"");
    if (a < 0) { snprintf(err, errCap, "no_token"); return false; }
    a += 9;
    int b = resp.indexOf('"', a);
    if (b < 0 || b - a >= (int)tokCap) { snprintf(err, errCap, "no_token"); return false; }
    memcpy(token, resp.c_str() + a, (size_t)(b - a));
    token[b - a] = '\0';
    return true;
}

bool BackupService::fetchSnapshot(const char* ip, const char* token,
                                  char* buf, size_t cap, size_t* outLen,
                                  char* err, size_t errCap) {
    char url[56];
    snprintf(url, sizeof(url), "http://%s/api/config/export", ip);
    HTTPClient http;
    http.setTimeout(6000);
    if (!http.begin(url)) { snprintf(err, errCap, "begin"); return false; }
    http.addHeader("X-Auth-Token", token);
    int code = http.GET();
    if (code == 401) { snprintf(err, errCap, "http_401"); http.end(); return false; }
    if (code == 404) { snprintf(err, errCap, "old_fw"); http.end(); return false; }
    if (code != 200) {
        snprintf(err, errCap, code <= 0 ? "offline" : "http_%d", code);
        http.end();
        return false;
    }
    String resp = http.getString();
    http.end();
    if (resp.length() < 2 || resp[0] != '{' || (size_t)resp.length() >= cap) {
        snprintf(err, errCap, "bad_body");
        return false;
    }
    memcpy(buf, resp.c_str(), (size_t)resp.length() + 1);
    *outLen = (size_t)resp.length();
    return true;
}

// ============================================================================
// SD: хранилище снимков
// ============================================================================
bool BackupService::storeSnapshot(const char* ip, const char* data, size_t len,
                                  char* err, size_t errCap) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { snprintf(err, errCap, "no_sd"); return false; }

    // Имя из ЛОКАЛЬНОГО времени (как сегменты журнала): config-YYYYMMDD-HHMMSS
    struct tm t;
    if (!TimeService::getInstance().getLocalTime(t)) {
        snprintf(err, errCap, "no_time");
        return false;
    }
    char dir[40];
    snprintf(dir, sizeof(dir), "/backup/%s", ip);
    if (!sd->exists("/backup")) sd->mkdir("/backup");
    if (!sd->exists(dir) && !sd->mkdir(dir)) {
        snprintf(err, errCap, "mkdir");
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "%s/config-%04d%02d%02d-%02d%02d%02d.json",
             dir, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    // tmp+rename: обрыв питания посреди записи ≠ битый «последний» снимок.
    char tmp[68];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    File f = sd->open(tmp, FILE_WRITE);
    if (!f) { snprintf(err, errCap, "open"); return false; }
    size_t w = f.write((const uint8_t*)data, len);
    f.close();
    if (w != len) { sd->remove(tmp); snprintf(err, errCap, "write"); return false; }
    if (sd->exists(path)) sd->remove(path);
    if (!sd->rename(tmp, path)) { sd->remove(tmp); snprintf(err, errCap, "rename"); return false; }

    // latest.json — указатель «самого свежего» (та же tmp+rename).
    char latest[64], ltmp[68];
    snprintf(latest, sizeof(latest), "%s/latest.json", dir);
    snprintf(ltmp, sizeof(ltmp), "%s.tmp", latest);
    File lf = sd->open(ltmp, FILE_WRITE);
    if (lf) {
        size_t lw = lf.write((const uint8_t*)data, len);
        lf.close();
        if (lw == len) {
            if (sd->exists(latest)) sd->remove(latest);
            sd->rename(ltmp, latest);
        } else sd->remove(ltmp);
    }
    return true;
}

void BackupService::rotate(const char* ip, const char* prefix, uint8_t keep) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) return;
    char dir[40];
    snprintf(dir, sizeof(dir), "/backup/%s", ip);
    File d = sd->open(dir);
    if (!d) return;
    // Имена <prefix>YYYYMMDD-HHMMSS.json сортируются хронологически —
    // собираем и режем хвост сверх keep. Снимков мало (≤60), пузырёк честен.
    // 0.9.5: префикс — параметр ("config-", "<slug>-"); latest*.json и
    // чужие серии не трогаем.
    const size_t plen = strlen(prefix);
    char names[64][40];
    uint8_t n = 0;
    for (File f = d.openNextFile(); f && n < 64; f = d.openNextFile()) {
        const char* nm = f.name();   // может быть с путём — берём хвост
        const char* base = strrchr(nm, '/');
        base = base != nullptr ? base + 1 : nm;
        if (strncmp(base, prefix, plen) != 0) continue;   // latest* и прочие — мимо
        size_t l = strlen(base);
        if (l >= 40) continue;
        // вставка с сохранением порядка
        uint8_t i = n;
        while (i > 0 && strcmp(names[i - 1], base) > 0) {
            memcpy(names[i], names[i - 1], 40);
            i--;
        }
        memcpy(names[i], base, l + 1);
        n++;
    }
    d.close();
    while (n > keep) {
        char victim[96];
        snprintf(victim, sizeof(victim), "%s/%s", dir, names[0]);
        sd->remove(victim);
        log(LogLevel::Info, "bk: %s ротация — удалён %s", ip, names[0]);
        memmove(names, names + 1, (n - 1) * 40);
        n--;
    }
}

// ============================================================================
// 0.9.5 (m38): ДОП.СНИМКИ — bk.extras ("ip:путь,ip:путь", путь от /api/dev/)
// ============================================================================
// Слаг серии из пути: последний сегмент, а если он "export" — предыдущий
// ("lock/users/export" -> "users"). Только [A-Za-z0-9_-], до 24 символов.
static void extraSlug(const char* path, char* out, size_t cap) {
    out[0] = '\0';
    if (path == nullptr) return;
    size_t l = strlen(path);
    while (l > 0 && path[l - 1] == '/') l--;              // хвостовые слэши
    if (l == 0) return;
    const char* end = path + l;
    const char* beg = end;
    while (beg > path && beg[-1] != '/') beg--;           // последний сегмент
    if ((size_t)(end - beg) == 6 && strncmp(beg, "export", 6) == 0 &&
        beg > path) {
        end = beg - 1;                                     // перед '/'
        beg = end;   // 0.9.5-фикс (полевой прогон 22.10): beg тоже отступаем
        while (beg > path && beg[-1] != '/') beg--;        // предыдущий сегмент
    }
    size_t n = 0;
    for (const char* p = beg; p < end && n < cap - 1; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') out[n++] = c;
    }
    out[n] = '\0';
}

void BackupService::stepExtras(const char* ip, const char* token) {
    char list[CFG_VALUE_LEN];
    cfgGetStr("bk.extras", list, sizeof(list), "");
    if (list[0] == '\0') return;
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) return;

    // Буфер крупнее конфиг-снимка (users.json замка ≈ 30 КБ) — из кучи
    // на время шага (тот же урок saveToJson: транзиентам — куча).
    char* buf = (char*)malloc(BK_EXTRA_CAP);
    if (buf == nullptr) {
        log(LogLevel::Warning, "bk: %s extras — нет кучи", ip);
        return;
    }
    const uint8_t keep = (uint8_t)cfgGetInt("bk.keep", 10);

    uint8_t done = 0;
    char* save = nullptr;
    for (char* ent = strtok_r(list, ",", &save);
         ent != nullptr && done < BK_MAX_EXTRAS;
         ent = strtok_r(nullptr, ",", &save)) {
        char* colon = strchr(ent, ':');
        if (colon == nullptr) continue;
        *colon = '\0';
        if (strcmp(ent, ip) != 0) continue;               // запись чужого хоста
        const char* path = colon + 1;
        if (path[0] == '\0') continue;
        done++;

        char slug[25];
        extraSlug(path, slug, sizeof(slug));
        if (slug[0] == '\0') {
            log(LogLevel::Warning, "bk: %s extra «%s» — пустой слаг, пропуск",
                ip, path);
            continue;
        }

        char url[96];
        snprintf(url, sizeof(url), "http://%s/api/dev/%s", ip, path);
        HTTPClient http;
        http.setTimeout(8000);
        if (!http.begin(url)) {
            log(LogLevel::Warning, "bk: %s extra %s — begin", ip, slug);
            continue;
        }
        http.addHeader("X-Auth-Token", token);
        int code = http.GET();
        if (code != 200) {
            // 404 = хост на старой прошивке без endpoint'а — не беда и
            // не ошибка хоста (lastErr не трогаем — конфиг уже снят).
            if (code == 404) {
                log(LogLevel::Info,
                    "bk: %s extra %s — нет endpoint'а (старая прошивка)",
                    ip, slug);
            } else {
                log(LogLevel::Warning, "bk: %s extra %s — http_%d",
                    ip, slug, code);
            }
            http.end();
            continue;
        }
        // Потоково в буфер с потолком: String на 30+ КБ — риск фрагментации
        // кучи (урок: большие ответы — только в заранее выделенный буфер).
        WiFiClient* stream = http.getStreamPtr();
        size_t total = 0;
        const uint32_t deadline = millis() + 8000;
        while ((int32_t)(millis() - deadline) < 0) {
            int av = stream->available();
            if (av > 0) {
                const size_t room = BK_EXTRA_CAP - 1 - total;
                if (room == 0) break;
                if ((size_t)av > room) av = (int)room;
                int r = stream->read((uint8_t*)buf + total, av);
                if (r <= 0) break;
                total += (size_t)r;
            } else if (!http.connected()) {
                break;
            } else {
                delay(1);
            }
        }
        http.end();
        buf[total] = '\0';
        if (total < 2 || (buf[0] != '{' && buf[0] != '[')) {
            log(LogLevel::Warning, "bk: %s extra %s — bad_body (%u Б)",
                ip, slug, (unsigned)total);
            continue;
        }

        struct tm t;
        if (!TimeService::getInstance().getLocalTime(t)) {
            log(LogLevel::Warning, "bk: %s extra %s — no_time", ip, slug);
            continue;
        }
        char dir[40];
        snprintf(dir, sizeof(dir), "/backup/%s", ip);   // каталог уже есть (конфиг)
        char fpath[80];
        snprintf(fpath, sizeof(fpath), "%s/%s-%04d%02d%02d-%02d%02d%02d.json",
                 dir, slug, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);

        // tmp+rename — та же дисциплина, что у конфиг-снимков.
        char tmp[84];
        snprintf(tmp, sizeof(tmp), "%s.tmp", fpath);
        File f = sd->open(tmp, FILE_WRITE);
        if (!f) { log(LogLevel::Warning, "bk: %s extra %s — open", ip, slug); continue; }
        size_t w = f.write((const uint8_t*)buf, total);
        f.close();
        if (w != total) {
            sd->remove(tmp);
            log(LogLevel::Warning, "bk: %s extra %s — write", ip, slug);
            continue;
        }
        if (sd->exists(fpath)) sd->remove(fpath);
        if (!sd->rename(tmp, fpath)) {
            sd->remove(tmp);
            log(LogLevel::Warning, "bk: %s extra %s — rename", ip, slug);
            continue;
        }
        char latest[80], ltmp[84];
        snprintf(latest, sizeof(latest), "%s/latest-%s.json", dir, slug);
        snprintf(ltmp, sizeof(ltmp), "%s.tmp", latest);
        File lf = sd->open(ltmp, FILE_WRITE);
        if (lf) {
            size_t lw = lf.write((const uint8_t*)buf, total);
            lf.close();
            if (lw == total) {
                if (sd->exists(latest)) sd->remove(latest);
                sd->rename(ltmp, latest);
            } else sd->remove(ltmp);
        }
        char prefix[28];
        snprintf(prefix, sizeof(prefix), "%s-", slug);
        rotate(ip, prefix, keep);
        log(LogLevel::Info, "bk: %s extra %s сохранён (%u Б)",
            ip, slug, (unsigned)total);
    }
    free(buf);
}

// ============================================================================
// ВОССТАНОВЛЕНИЕ: снимок с SD -> /api/config/import устройства (ребут его)
// ============================================================================
bool BackupService::pushSnapshot(const char* ip, const char* file,
                                 char* err, size_t errCap) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { snprintf(err, errCap, "no_sd"); return false; }

    // Путь собираем сами — траверсал режем на входе (file — только имя).
    // 0.9.5 прогон-3: восстанавливаются ТОЛЬКО конфиг-снимки (extras —
    // пользовательские базы и пр. — хранятся, но не разворачиваются
    // через /api/config/import: замок чужой JSON как конфиг не примет,
    // а вот мусорный CFG_CHANGED-шторм нам не нужен).
    if (file == nullptr || strstr(file, "..") != nullptr ||
        strchr(file, '/') != nullptr ||
        strncmp(file, "config-", 7) != 0) {
        snprintf(err, errCap, "bad_name");
        return false;
    }
    char path[96];
    snprintf(path, sizeof(path), "/backup/%s/%s", ip, file);
    File f = sd->open(path, FILE_READ);
    if (!f) { snprintf(err, errCap, "no_file"); return false; }
    size_t len = f.size();
    if (len == 0 || len >= BK_SNAPSHOT_CAP) {
        f.close();
        snprintf(err, errCap, "bad_size");
        return false;
    }
    char* snap = (char*)malloc(len + 1);
    if (snap == nullptr) { f.close(); snprintf(err, errCap, "no_heap"); return false; }
    size_t r = f.read((uint8_t*)snap, len);
    f.close();
    if (r != len) { free(snap); snprintf(err, errCap, "read"); return false; }
    snap[len] = '\0';

    char token[16];
    bool ok = loginHost(ip, token, sizeof(token), err, errCap);
    if (ok) {
        char url[56];
        snprintf(url, sizeof(url), "http://%s/api/config/import", ip);
        HTTPClient http;
        http.setTimeout(8000);
        if (!http.begin(url)) {
            snprintf(err, errCap, "begin");
            ok = false;
        } else {
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-Auth-Token", token);
            int code = http.POST((uint8_t*)snap, len);
            if (code == 200) {
                log(LogLevel::Info,
                    "bk: %s снимок %s развёрнут, устройство перезагружается",
                    ip, file);
            } else {
                snprintf(err, errCap, code == 422 ? "corrupt" :
                         code == 401 ? "http_401" :
                         code <= 0 ? "offline" : "http_%d", code);
                ok = false;
            }
            http.end();
        }
    }
    free(snap);
    return ok;
}

// ============================================================================
// API ДЛЯ ПАНЕЛИ (вызывает HomeMasterUi::handleApi, admin уже проверен ядром)
// ============================================================================
size_t BackupService::apiStatus(char* buf, size_t bufSize) {
    uint32_t nowU = timeValid()
        ? (uint32_t)TimeService::getInstance().getUnixTime() : 0;
    size_t pos = 0;
    int n = snprintf(buf, bufSize, "{\"enabled\":%u,\"nextPlanIn\":%ld,"
                     "\"running\":%d,\"hosts\":[",
                     _enabled ? 1 : 0,
                     (_nextPlanUnix > nowU && nowU > 0)
                         ? (long)(_nextPlanUnix - nowU) : -1L,
                     _cycleIdx >= 0 ? 1 : 0);
    if (n > 0) pos = (size_t)n;
    for (uint8_t i = 0; i < _hostCount && pos < bufSize - 96; ++i) {
        const HostState& h = _hosts[i];
        n = snprintf(buf + pos, bufSize - pos,
                     "%s{\"ip\":\"%s\",\"host\":\"%s\",\"lastOk\":%lu,\"err\":\"%s\",\"blocked\":%u}",
                     i ? "," : "", h.ip, h.host, (unsigned long)h.lastOkUnix,
                     h.lastErr, h.blocked ? 1 : 0);
        if (n < 0) break;
        pos += (size_t)n;
    }
    if (pos < bufSize - 3) {
        buf[pos++] = ']';
        buf[pos++] = '}';
        buf[pos]   = '\0';
    }
    return pos;
}

size_t BackupService::apiFiles(const char* ip, char* buf, size_t bufSize) {
    size_t pos = (size_t)snprintf(buf, bufSize, "{\"ip\":\"%s\",\"files\":[", ip);
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) {
        if (pos < bufSize - 20) strcpy(buf + pos, "],\"err\":\"no_sd\"}");
        return strlen(buf);
    }
    char dir[40];
    snprintf(dir, sizeof(dir), "/backup/%s", ip);
    File d = sd->open(dir);
    bool first = true;
    if (d) {
        // Свежие первыми: имена сортируются — выводим в обратном порядке,
        // для этого сначала в массив (снимков ≤60, как в rotate).
        // 0.9.5 прогон-3: extras (users-*.json и др. серии bk.extras)
        // отдаём ОТДЕЛЬНЫМ массивом "extras" — они не конфиги, кнопки
        // «Развернуть» у них в морде нет (и apiRestore их не примет).
        char names[60][40];
        uint8_t n = 0;
        char xnames[16][40];
        uint8_t xn = 0;
        for (File f = d.openNextFile(); f && (n < 60 || xn < 16); f = d.openNextFile()) {
            const char* nm = f.name();
            const char* base = strrchr(nm, '/');
            base = base != nullptr ? base + 1 : nm;
            size_t l = strlen(base);
            if (l >= 40) continue;
            if (strncmp(base, "config-", 7) != 0) {
                // extras: *.json серий bk.extras; latest-* — мимо
                if (xn >= 16 || l < 6 || strcmp(base + l - 5, ".json") != 0 ||
                    strncmp(base, "latest", 6) == 0) continue;
                uint8_t i = xn;
                while (i > 0 && strcmp(xnames[i - 1], base) > 0) {
                    memcpy(xnames[i], xnames[i - 1], 40);
                    i--;
                }
                memcpy(xnames[i], base, l + 1);
                xn++;
                continue;
            }
            uint8_t i = n;
            while (i > 0 && strcmp(names[i - 1], base) > 0) {
                memcpy(names[i], names[i - 1], 40);
                i--;
            }
            memcpy(names[i], base, l + 1);
            n++;
        }
        d.close();
        for (int i = (int)n - 1; i >= 0 && pos < bufSize - 48; --i) {
            int w = snprintf(buf + pos, bufSize - pos, "%s\"%s\"",
                             first ? "" : ",", names[i]);
            if (w < 0) break;
            pos += (size_t)w;
            first = false;
        }
        if (pos < bufSize - 12) {
            memcpy(buf + pos, "],\"extras\":[", 12);
            pos += 12;
        }
        for (int i = (int)xn - 1; i >= 0 && pos < bufSize - 48; --i) {
            int w = snprintf(buf + pos, bufSize - pos, "%s\"%s\"",
                             first ? "" : ",", xnames[i]);
            if (w < 0) break;
            pos += (size_t)w;
            first = false;
        }
    }
    if (pos < bufSize - 3) { buf[pos++] = ']'; buf[pos++] = '}'; buf[pos] = '\0'; }
    return pos;
}

size_t BackupService::apiSnapshot(const char* ip, char* buf, size_t bufSize) {
    // Ручной съём: "all" — весь список, иначе один хост из bk.hosts.
    if (ip == nullptr || strcmp(ip, "all") == 0) {
        scheduleCycle(0);
        return (size_t)snprintf(buf, bufSize, "{\"ok\":1,\"queued\":\"all\"}");
    }
    int idx = findHost(ip);
    if (idx < 0) {
        return (size_t)snprintf(buf, bufSize,
                                "{\"ok\":0,\"err\":\"not_in_bk.hosts\"}");
    }
    scheduleCycle(0, (int8_t)idx);
    return (size_t)snprintf(buf, bufSize, "{\"ok\":1,\"queued\":\"%s\"}", ip);
}

size_t BackupService::apiRestore(const char* ip, const char* file,
                                 char* buf, size_t bufSize) {
    char err[24] = "";
    // Страховка (урок 0.6.2-пр1): собственный IP мастера — это "self",
    // сетевой запрос к самому себе недопустим (дедлок WebServer, TWDT).
    {
        char ownIp[20];
        NetworkService::getInstance().ipString(ownIp, sizeof(ownIp));
        if (ip != nullptr && strcmp(ip, ownIp) == 0) ip = "self";
    }
    // 0.6.2, bk.self: восстановление САМОГО мастера — локально:
    // читаем снимок с SD, applySnapshotJson, отложенный ребут.
    // Ответ несёт reboot_in_ms — панель покажет оверлей перезагрузки.
    if (strcmp(ip, "self") == 0) {
        if (strchr(file, '/') != nullptr || strstr(file, "..") != nullptr ||
            strncmp(file, "config-", 7) != 0) {   // 0.9.5-пр3: только конфиги
            return (size_t)snprintf(buf, bufSize, "{\"ok\":0,\"err\":\"name\"}");
        }
        fs::FS* sd = SdService::getInstance().fs();
        if (sd == nullptr) {
            return (size_t)snprintf(buf, bufSize, "{\"ok\":0,\"err\":\"no_sd\"}");
        }
        char path[64];
        snprintf(path, sizeof(path), "/backup/self/%s", file);
        File f = sd->open(path, FILE_READ);
        if (!f) {
            return (size_t)snprintf(buf, bufSize, "{\"ok\":0,\"err\":\"no_file\"}");
        }
        char* snap = (char*)malloc(BK_SNAPSHOT_CAP);
        if (snap == nullptr) {
            f.close();
            return (size_t)snprintf(buf, bufSize, "{\"ok\":0,\"err\":\"no_heap\"}");
        }
        size_t len = f.read((uint8_t*)snap, BK_SNAPSHOT_CAP - 1);
        f.close();
        snap[len] = '\0';
        int applied = ConfigService::getInstance().applySnapshotJson(snap);
        free(snap);
        if (applied == 0) {
            return (size_t)snprintf(buf, bufSize,
                                    "{\"ok\":0,\"err\":\"snapshot_corrupt\"}");
        }
        _selfRebootAtMs = millis() + 1500;
        log(LogLevel::Info, "bk: self-restore из %s (полей %d), ребут через 1.5 с",
            file, applied);
        return (size_t)snprintf(buf, bufSize,
                                "{\"ok\":1,\"applied\":%d,\"reboot_in_ms\":1500}",
                                applied);
    }
    if (pushSnapshot(ip, file, err, sizeof(err))) {
        return (size_t)snprintf(buf, bufSize, "{\"ok\":1}");
    }
    return (size_t)snprintf(buf, bufSize, "{\"ok\":0,\"err\":\"%s\"}", err);
}

// ============================================================================
// ДЛЯ ПАЗ (hm.bk)
// ============================================================================
uint8_t BackupService::troubleCount(char* out, size_t cap) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < _hostCount; ++i) {
        const HostState& h = _hosts[i];
        if (!h.blocked && h.lastErr[0] == '\0') continue;
        if (n == 0 && out != nullptr && cap > 0) {
            snprintf(out, cap, "%s:%s", h.ip, h.blocked ? "blocked" : h.lastErr);
        }
        n++;
    }
    return n;
}

// ============================================================================
// ДЛЯ ПАЗ (hm.fleet, 0.6.8): присутствие парка по сессиям брокера
// ============================================================================
uint8_t BackupService::fleetSilence(char* out, size_t cap,
                                    uint8_t& checkable) const {
    checkable = 0;
    uint8_t silent = 0;
    if (out != nullptr && cap > 0) out[0] = '\0';
    BrokerService& br = BrokerService::getInstance();
    for (uint8_t i = 0; i < _hostCount; ++i) {
        const HostState& h = _hosts[i];
        if (strcmp(h.ip, "self") == 0) continue;     // мастер — не клиент сам себе
        if (h.host[0] == '\0') continue;             // имени нет — нет данных
        checkable++;
        if (!br.hasClient(h.host)) {
            if (silent == 0 && out != nullptr && cap > 0) {
                snprintf(out, cap, "%s молчит", h.host);
            }
            silent++;
        }
    }
    return silent;
}
