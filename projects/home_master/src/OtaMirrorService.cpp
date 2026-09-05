// OtaMirrorService.cpp — 0.6.1: OTA-зеркало парка на мастере (см. шапку .h)
// ============================================================================
#include "OtaMirrorService.h"
#include "SdService.h"
#include <core/EventBus.h>
#include <core/Events.h>
#include <services/ConfigService.h>
#include <services/TimeService.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <time.h>

OtaMirrorService& OtaMirrorService::getInstance() {
    static OtaMirrorService inst;
    return inst;
}

// Извлечение строки из JSON с произвольными пробелами — близнец otaJsonStr
// из UpdateService (урок 5.0.7: ArduinoJson в проекте нет и не надо).
static bool omJsonStr(const char* js, const char* key, char* out, size_t n) {
    const char* p = strstr(js, key);
    if (p == nullptr) return false;
    p = strchr(p + strlen(key), ':');
    if (p == nullptr) return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '"') return false;
    ++p;
    const char* q = strchr(p, '"');
    if (q == nullptr) return false;
    size_t len = (size_t)(q - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void OtaMirrorService::init() {
    _enabled = cfgGetBool("otam.enabled", true);
    reloadHosts();
    EventBus::getInstance().subscribe(CFG_EVENT_CHANGED, this);
}

void OtaMirrorService::start() {
    if (_enabled) {
        // Второй HTTP-сервер: только whitelist-раздача /local/ota/ с SD.
        // onNotFound ловит всё — внутри строгая проверка пути (handleOtaHttp).
        _otaServer.onNotFound([this]() { handleOtaHttp(); });
        _otaServer.begin();
        _serverUp = true;
    }
    log(LogLevel::Info, "OTA-зеркало %s (%u хостов, порт %u)",
        _enabled ? "включено" : "выключено", _hostCount, OTA_PORT);
}

void OtaMirrorService::stop() {
    if (_serverUp) { _otaServer.stop(); _serverUp = false; }
}

void OtaMirrorService::tick() {
    // handleClient — в каждом тике (дешёво; устройства качают fw/fs именно
    // отсюда, отзывчивость раздачи важна: 1,1 МБ по 512 B в ответ).
    if (_serverUp) _otaServer.handleClient();

    if (!_enabled) return;

    if (_nextPollAtMs == 0) _nextPollAtMs = millis() + FIRST_POLL_MS;

    // 1) идёт цикл — обрабатываем хост шага
    if (_cycleIdx >= 0 && (int32_t)(millis() - _nextStepAtMs) >= 0) {
        runCycleStep();
        return;
    }
    // 2) пора начинать новый цикл
    if (_cycleIdx < 0 && (int32_t)(millis() - _nextPollAtMs) >= 0) {
        scheduleCycle(0);
    }
}

// ============================================================================
// ШИНА (поля CFG_NONE — живые)
// ============================================================================
bool OtaMirrorService::canHandleEvent(int32_t eventId) const {
    return eventId == CFG_EVENT_CHANGED;
}

void OtaMirrorService::onEvent(int32_t eventId, const ShEventData* data) {
    if (eventId != CFG_EVENT_CHANGED || data == nullptr) return;
    if (strcmp(data->payload, "otam.enabled") == 0) {
        _enabled = cfgGetBool("otam.enabled", true);
        if (_enabled && !_serverUp) {
            _otaServer.onNotFound([this]() { handleOtaHttp(); });
            _otaServer.begin();
            _serverUp = true;
        }
        if (_enabled) scheduleCycle(2000);
        log(LogLevel::Info, "OTA-зеркало %s", _enabled ? "включено" : "выключено");
    } else if (strcmp(data->payload, "otam.hosts") == 0) {
        reloadHosts();
        if (_enabled) scheduleCycle(2000);
    } else if (strcmp(data->payload, "otam.period_h") == 0) {
        uint32_t h = cfgGetUInt("otam.period_h", 6);
        if (h < 1) h = 1;
        if (h > 168) h = 168;
        _nextPollAtMs = millis() + h * 3600000UL;
    }
}

// ============================================================================
// СПИСОК ХОСТОВ
// ============================================================================
bool OtaMirrorService::hostListed(const char* csv, const char* host) {
    size_t hl = strlen(host);
    if (hl == 0) return false;
    const char* p = csv;
    while (*p != '\0') {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == hl && strncmp(p, host, len) == 0) return true;
        if (comma == nullptr) break;
        p = comma + 1;
    }
    return false;
}

void OtaMirrorService::reloadHosts() {
    char csv[CFG_VALUE_LEN];
    cfgGetStr("otam.hosts", csv, sizeof(csv), "smart_lock,weather_gate");
    // Стейты хостов переживают перечитывание списка (урок BackupService:
    // список живой, историю ошибок не затираем).
    HostState old[OM_MAX_HOSTS];
    uint8_t oldCount = _hostCount;
    memcpy(old, _hosts, sizeof(old));

    _hostCount = 0;
    const char* p = csv;
    while (*p != '\0' && _hostCount < OM_MAX_HOSTS) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len > 0 && len < sizeof(_hosts[0].host)) {
            HostState h;
            memcpy(h.host, p, len);
            h.host[len] = '\0';
            for (uint8_t i = 0; i < oldCount; ++i) {
                if (strcmp(old[i].host, h.host) == 0) { h = old[i]; break; }
            }
            _hosts[_hostCount++] = h;
        }
        if (comma == nullptr) break;
        p = comma + 1;
    }
    log(LogLevel::Info, "OTA-зеркало: хостов в списке %u", _hostCount);
}

int OtaMirrorService::findHost(const char* host) const {
    for (uint8_t i = 0; i < _hostCount; ++i) {
        if (strcmp(_hosts[i].host, host) == 0) return (int)i;
    }
    return -1;
}

// ============================================================================
// КОНЕЧНЫЙ АВТОМАТ ЦИКЛА (зеркало BackupService: один хост за шаг,
// tick не держим — HTTP по 1,1 МБ растягиваем по хостам)
// ============================================================================
void OtaMirrorService::scheduleCycle(uint32_t delayMs, int8_t only) {
    if (only >= 0 && only < (int8_t)_hostCount) {
        _cycleIdx = only;
        _cycleEnd = only + 1;
    } else {
        _cycleIdx = 0;
        _cycleEnd = (int8_t)_hostCount;
    }
    _nextStepAtMs = millis() + delayMs;
}

void OtaMirrorService::runCycleStep() {
    if (_cycleIdx < 0 || _cycleIdx >= _cycleEnd) {
        _cycleIdx = -1;
        _phase = 0;
        uint32_t h = cfgGetUInt("otam.period_h", 6);
        if (h < 1) h = 1;
        if (h > 168) h = 168;
        _nextPollAtMs = millis() + h * 3600000UL;
        uint32_t nowU = TimeService::getInstance().getUnixTime();
        if (nowU >= 1700000000UL) _lastRunUnix = nowU;
        return;
    }
    HostState& h = _hosts[_cycleIdx];
    char err[24] = "";

    // Одна ФАЗА за тик (см. .h): каждая фаза ограничена по времени и укладывается
    // в бюджет WDT loopTask (10 с) с запасом; между фазами пауза CYCLE_GAP_MS.
    switch (_phase) {
    case 0: {   // манифест + решение «качать/не качать»
        fs::FS* sd = SdService::getInstance().fs();
        if (sd == nullptr) { finishHost(h, "no_sd"); return; }
        if (!fetchManifest(h.host, _manifest, sizeof(_manifest), err, sizeof(err))) {
            finishHost(h, err);
            return;
        }
        omJsonStr(_manifest, "\"version\"", _pVer, sizeof(_pVer));
        omJsonStr(_manifest, "\"fw_url\"", _pFwUrl, sizeof(_pFwUrl));
        omJsonStr(_manifest, "\"fs_url\"", _pFsUrl, sizeof(_pFsUrl));
        // Урок 5.0.12: поле Build Master — «fw_md5» (не «md5»)
        if (!omJsonStr(_manifest, "\"fw_md5\"", _pFwMd5, sizeof(_pFwMd5)))
            omJsonStr(_manifest, "\"md5\"", _pFwMd5, sizeof(_pFwMd5));
        omJsonStr(_manifest, "\"fs_md5\"", _pFsMd5, sizeof(_pFsMd5));
        if (_pVer[0] == '\0' || (_pFwUrl[0] == '\0' && _pFsUrl[0] == '\0')) {
            finishHost(h, "manifest_bad");
            return;
        }
        if (strcmp(h.version, _pVer) == 0) { finishHost(h, nullptr); return; }
        if (h.version[0] == '\0') {
            // Первый опрос после ребута мастера: RAM чист, но зеркало могло
            // собраться прошлым циклом — сверимся с манифестом на SD.
            char path[96], sdVer[20] = "";
            snprintf(path, sizeof(path), "/ota/%s/version.json", h.host);
            File f = sd->open(path, FILE_READ);
            if (f) {
                static char js[MANIFEST_CAP];   // static, не стек
                size_t r = f.read((uint8_t*)js, sizeof(js) - 1);
                f.close();
                js[r] = '\0';
                omJsonStr(js, "\"version\"", sdVer, sizeof(sdVer));
            }
            if (strcmp(sdVer, _pVer) == 0) {
                safeStrCopy(h.version, sizeof(h.version), sdVer);
                finishHost(h, nullptr);
                return;
            }
        }
        _phase = 1;
        _nextStepAtMs = millis() + CYCLE_GAP_MS;
        return;
    }
    case 1: {   // firmware.bin: tmp -> md5 -> rename
        if (_pFwUrl[0] == '\0') { _phase = 2; _nextStepAtMs = millis() + CYCLE_GAP_MS; return; }
        char url[200];
        urlResolve(h.host, _pFwUrl, url, sizeof(url));
        uint32_t sz = 0;
        if (!downloadBin(h.host, url, "firmware.bin", _pFwMd5, &sz, err, sizeof(err)) ||
            !commitBin(h.host, "firmware.bin", err, sizeof(err))) {
            finishHost(h, err);
            return;
        }
        h.fwSize = sz;
        _phase = 2;
        _nextStepAtMs = millis() + CYCLE_GAP_MS;
        return;
    }
    case 2: {   // littlefs.bin: tmp -> md5 -> rename
        if (_pFsUrl[0] == '\0') { _phase = 3; _nextStepAtMs = millis() + CYCLE_GAP_MS; return; }
        char url[200];
        urlResolve(h.host, _pFsUrl, url, sizeof(url));
        uint32_t sz = 0;
        if (!downloadBin(h.host, url, "littlefs.bin", _pFsMd5, &sz, err, sizeof(err)) ||
            !commitBin(h.host, "littlefs.bin", err, sizeof(err))) {
            finishHost(h, err);
            return;
        }
        h.fsSize = sz;
        _phase = 3;
        _nextStepAtMs = millis() + CYCLE_GAP_MS;
        return;
    }
    default: {  // 3 — коммит: манифест ПОСЛЕДНИМ (точка коммита)
        if (storeManifest(h.host, _manifest, strlen(_manifest), err, sizeof(err))) {
            safeStrCopy(h.version, sizeof(h.version), _pVer);
            log(LogLevel::Info, "OTA-зеркало: %s -> %s (fw %u, fs %u)",
                h.host, h.version, h.fwSize, h.fsSize);
            finishHost(h, nullptr);
        } else {
            finishHost(h, err);
        }
        return;
    }
    }
}

void OtaMirrorService::finishHost(HostState& h, const char* err) {
    uint32_t nowU = TimeService::getInstance().getUnixTime();
    if (err != nullptr && err[0] != '\0') {
        strncpy(h.lastErr, err, sizeof(h.lastErr) - 1);
        h.lastErr[sizeof(h.lastErr) - 1] = '\0';
        log(LogLevel::Warning, "OTA-зеркало: %s — %s", h.host, err);
    } else {
        h.lastErr[0] = '\0';
        if (nowU >= 1700000000UL) h.lastOkUnix = nowU;
    }
    ++_cycleIdx;
    _phase = 0;
    _nextStepAtMs = millis() + CYCLE_GAP_MS;
    if (_cycleIdx >= _cycleEnd) {
        _cycleIdx = -1;
        uint32_t ph = cfgGetUInt("otam.period_h", 6);
        if (ph < 1) ph = 1;
        if (ph > 168) ph = 168;
        _nextPollAtMs = millis() + ph * 3600000UL;
        if (nowU >= 1700000000UL) _lastRunUnix = nowU;
    }
}

// ============================================================================
// HTTP-КЛИЕНТ К HA (контекст tick — там HTTPClient безопасен, урок
// UpdateService: «в loop, где HTTPClient безопасен по стеку»)
// ============================================================================
void OtaMirrorService::urlResolve(const char* host, const char* src,
                                  char* out, size_t n) const {
    if (strncmp(src, "http", 4) == 0) {
        safeStrCopy(out, n, src);
        return;
    }
    char ha[CFG_VALUE_LEN];
    cfgGetStr("mqtt.host", ha, sizeof(ha), "");
    if (src[0] == '/') {
        snprintf(out, n, "http://%s:8123%s", ha, src);
        return;
    }
    snprintf(out, n, "http://%s:8123/local/ota/%s/%s", ha, host, src);
}

bool OtaMirrorService::fetchManifest(const char* host, char* buf, size_t cap,
                                     char* err, size_t errCap) {
    char ha[CFG_VALUE_LEN];
    cfgGetStr("mqtt.host", ha, sizeof(ha), "");
    char url[200];
    snprintf(url, sizeof(url), "http://%s:8123/local/ota/%s/version.json", ha, host);
    HTTPClient http;
    http.setTimeout(4000);   // фаза целиком < WDT (10 с): connect+headers ≤ 4 с
    if (!http.begin(url)) { snprintf(err, errCap, "begin"); return false; }
    int code = http.GET();
    if (code != 200) {
        http.end();
        snprintf(err, errCap, code <= 0 ? "offline" : "http_%d", code);
        return false;
    }
    String body = http.getString();
    http.end();
    if (body.length() == 0 || body.length() >= (int)cap || body[0] != '{') {
        snprintf(err, errCap, "manifest_bad");
        return false;
    }
    memcpy(buf, body.c_str(), body.length() + 1);
    return true;
}

bool OtaMirrorService::md5FileOk(fs::File& f, const char* expectHex) {
    if (expectHex == nullptr || expectHex[0] == '\0') return true;  // нет эталона — верим
    MD5Builder md5;
    md5.begin();
    uint8_t buf[512];
    f.seek(0);
    while (f.available()) {
        int r = f.read(buf, sizeof(buf));
        if (r <= 0) break;
        md5.add(buf, (size_t)r);
    }
    md5.calculate();
    String hex = md5.toString();
    return hex.equalsIgnoreCase(expectHex);
}

bool OtaMirrorService::downloadBin(const char* host, const char* url,
                                   const char* localName, const char* expectMd5,
                                   uint32_t* outSize, char* err, size_t errCap) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { snprintf(err, errCap, "no_sd"); return false; }

    char dir[64], tmp[80];
    snprintf(dir, sizeof(dir), "/ota/%s", host);
    snprintf(tmp, sizeof(tmp), "%s/%s.tmp", dir, localName);
    // Урок 0.6.1-fix: SD.mkdir НЕ рекурсивен — сначала родитель /ota.
    if (!sd->exists("/ota") && !sd->mkdir("/ota")) { snprintf(err, errCap, "sd_mkdir"); return false; }
    if (!sd->exists(dir) && !sd->mkdir(dir)) { snprintf(err, errCap, "sd_mkdir"); return false; }
    sd->remove(tmp);   // остатки прошлого обрыва

    HTTPClient http;
    http.setTimeout(4000);   // connect/headers ≤ 4 с; тело сторожит цикл ниже (7 с cap)
    if (!http.begin(url)) { snprintf(err, errCap, "begin"); return false; }
    int code = http.GET();
    if (code != 200) {
        http.end();
        snprintf(err, errCap, code <= 0 ? "offline" : "http_%d", code);
        return false;
    }
    File f = sd->open(tmp, FILE_WRITE);
    if (!f) { http.end(); snprintf(err, errCap, "sd_open"); return false; }

    // getStreamPtr: тип по ядру (core 3.x — NetworkClient*, core 2.x —
    // WiFiClient*) — auto, чтобы не привязываться к версии платформы.
    auto* stream = http.getStreamPtr();
    uint8_t buf[512];
    uint32_t total = 0;
    bool ok = true;
    int expect = http.getSize();          // Content-Length (-1 = chunked)
    uint32_t dlStart = millis(), lastRx = millis();
    // Урок 0.6.1-fix (приёмка 05.09): выход по Content-Length/закрытию, НЕ по
    // connected() — HA держит keep-alive ПОСЛЕ последнего байта, цикл по
    // connected() висел вечно, и WDT (10 с) перезагружал кристалл каждые
    // ~2 минуты. Плюс сторожа: 2,5 с без прогресса = stall, 7 с всего = cap
    // (одна фаза обязана укладываться в бюджет WDT с запасом).
    while (expect < 0 ? stream->connected() : total < (uint32_t)expect) {
        size_t avail = stream->available();
        if (avail > 0) {
            int r = stream->read(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
            if (r <= 0) break;
            if (f.write(buf, (size_t)r) != (size_t)r) {
                snprintf(err, errCap, "sd_write");
                ok = false;
                break;
            }
            total += (uint32_t)r;
            lastRx = millis();
        } else if (!stream->connected()) {
            break;
        } else if (millis() - lastRx > 2500) {
            snprintf(err, errCap, "dl_stall");
            ok = false;
            break;
        } else {
            delay(1);
        }
        if (millis() - dlStart > 7000) {
            snprintf(err, errCap, "dl_timeout");
            ok = false;
            break;
        }
    }
    f.close();
    http.end();
    // Недокачанный файл (сервер закрыл раньше Content-Length) — не годится.
    if (ok && expect >= 0 && total != (uint32_t)expect) {
        snprintf(err, errCap, "dl_short");
        ok = false;
    }
    if (!ok || total == 0) {
        sd->remove(tmp);
        if (ok) snprintf(err, errCap, "empty");
        return false;
    }

    // MD5 эталона из манифеста — битый файл не подменяет рабочий.
    File chk = sd->open(tmp, FILE_READ);
    if (!chk) { sd->remove(tmp); snprintf(err, errCap, "sd_open"); return false; }
    bool md5ok = md5FileOk(chk, expectMd5);
    chk.close();
    if (!md5ok) {
        sd->remove(tmp);
        snprintf(err, errCap, "md5_bad");
        log(LogLevel::Error, "OTA-зеркало: %s %s — md5 не сошёлся", host, localName);
        return false;
    }
    if (outSize != nullptr) *outSize = total;
    return true;
}

bool OtaMirrorService::commitBin(const char* host, const char* localName,
                                 char* err, size_t errCap) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { snprintf(err, errCap, "no_sd"); return false; }
    char tmp[80], fin[80];
    snprintf(tmp, sizeof(tmp), "/ota/%s/%s.tmp", host, localName);
    snprintf(fin, sizeof(fin), "/ota/%s/%s", host, localName);
    sd->remove(fin);
    if (!sd->rename(tmp, fin)) { sd->remove(tmp); snprintf(err, errCap, "rename"); return false; }
    return true;
}

bool OtaMirrorService::storeManifest(const char* host, const char* data,
                                     size_t len, char* err, size_t errCap) {
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { snprintf(err, errCap, "no_sd"); return false; }
    char tmp[80], fin[80];
    snprintf(tmp, sizeof(tmp), "/ota/%s/version.json.tmp", host);
    snprintf(fin, sizeof(fin), "/ota/%s/version.json", host);
    File f = sd->open(tmp, FILE_WRITE);
    if (!f) { snprintf(err, errCap, "sd_open"); return false; }
    size_t w = f.write((const uint8_t*)data, len);
    f.close();
    if (w != len) { sd->remove(tmp); snprintf(err, errCap, "sd_write"); return false; }
    sd->remove(fin);
    if (!sd->rename(tmp, fin)) { sd->remove(tmp); snprintf(err, errCap, "rename"); return false; }
    return true;
}

// ============================================================================
// РАЗДАЧА :8123 (whitelist, read-only, без авторизации — устройства токенов
// не шлют; прошивка не секрет — решение владельца 04.09)
// ============================================================================
void OtaMirrorService::handleOtaHttp() {
    String uri = _otaServer.uri();
    // Строго: /local/ota/<host>/<file>, host из otam.hosts, file из тройки.
    static const char* PREFIX = "/local/ota/";
    if (!uri.startsWith(PREFIX) || uri.indexOf("..") >= 0) {
        _otaServer.send(404, "text/plain", "not_found");
        return;
    }
    String rest = uri.substring(strlen(PREFIX));
    int slash = rest.indexOf('/');
    if (slash <= 0) { _otaServer.send(404, "text/plain", "not_found"); return; }
    String host = rest.substring(0, slash);
    String file = rest.substring(slash + 1);
    bool fileOk = (file == "version.json" || file == "firmware.bin" ||
                   file == "littlefs.bin");
    char csv[CFG_VALUE_LEN];
    cfgGetStr("otam.hosts", csv, sizeof(csv), "");
    if (!fileOk || !hostListed(csv, host.c_str())) {
        _otaServer.send(404, "text/plain", "not_found");
        return;
    }
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) { _otaServer.send(503, "text/plain", "no_sd"); return; }
    char path[96];
    snprintf(path, sizeof(path), "/ota/%s/%s", host.c_str(), file.c_str());
    File f = sd->open(path, FILE_READ);
    if (!f) { _otaServer.send(404, "text/plain", "not_mirrored"); return; }
    _otaServer.streamFile(f, file == "version.json" ? "application/json"
                                                    : "application/octet-stream");
    f.close();
}

// ============================================================================
// API ДЛЯ ПАНЕЛИ
// ============================================================================
size_t OtaMirrorService::apiStatus(char* buf, size_t bufSize) {
    fs::FS* sd = SdService::getInstance().fs();
    size_t n = 0;
    uint32_t nextIn = 0;
    if (_enabled) {
        if (_cycleIdx >= 0) nextIn = 0;   // цикл идёт прямо сейчас
        else {
            int32_t d = (int32_t)(_nextPollAtMs - millis());
            nextIn = d > 0 ? (uint32_t)d / 1000 : 0;
        }
    }
    n += snprintf(buf + n, bufSize - n,
                  "{\"enabled\":%u,\"period_h\":%u,\"lastRunUnix\":%lu,"
                  "\"nextIn\":%lu,\"hosts\":[",
                  _enabled ? 1u : 0u, cfgGetUInt("otam.period_h", 6),
                  (unsigned long)_lastRunUnix, (unsigned long)nextIn);
    for (uint8_t i = 0; i < _hostCount; ++i) {
        const HostState& h = _hosts[i];
        // Размеры — по факту с SD (могли пережить ребут мастера).
        uint32_t fw = h.fwSize, fsz = h.fsSize;
        if (sd != nullptr && (fw == 0 || fsz == 0)) {
            char path[96];
            snprintf(path, sizeof(path), "/ota/%s/firmware.bin", h.host);
            if (fw == 0 && sd->exists(path)) { File f = sd->open(path, FILE_READ); fw = f.size(); f.close(); }
            snprintf(path, sizeof(path), "/ota/%s/littlefs.bin", h.host);
            if (fsz == 0 && sd->exists(path)) { File f = sd->open(path, FILE_READ); fsz = f.size(); f.close(); }
        }
        // Версию тоже поднимаем с SD, если RAM пуст (ребут мастера).
        char ver[20];
        safeStrCopy(ver, sizeof(ver), h.version);
        if (ver[0] == '\0' && sd != nullptr) {
            char path[96];
            snprintf(path, sizeof(path), "/ota/%s/version.json", h.host);
            File f = sd->open(path, FILE_READ);
            if (f) {
                static char js[MANIFEST_CAP];   // static, не стек
                size_t r = f.read((uint8_t*)js, sizeof(js) - 1);
                f.close();
                js[r] = '\0';
                omJsonStr(js, "\"version\"", ver, sizeof(ver));
            }
        }
        n += snprintf(buf + n, bufSize - n,
                      "%s{\"host\":\"%s\",\"version\":\"%s\",\"lastOkUnix\":%lu,"
                      "\"lastErr\":\"%s\",\"fwSize\":%lu,\"fsSize\":%lu}",
                      i ? "," : "", h.host, ver, (unsigned long)h.lastOkUnix,
                      h.lastErr, (unsigned long)fw, (unsigned long)fsz);
    }
    n += snprintf(buf + n, bufSize - n, "]}");
    return n;
}

size_t OtaMirrorService::apiCheck(const char* host, char* buf, size_t bufSize) {
    if (host != nullptr && strcmp(host, "all") != 0) {
        int idx = findHost(host);
        if (idx < 0) {
            return (size_t)snprintf(buf, bufSize, "{\"error\":\"unknown_host\"}");
        }
        scheduleCycle(0, (int8_t)idx);
    } else {
        scheduleCycle(0);
    }
    return (size_t)snprintf(buf, bufSize, "{\"ok\":1}");
}

// ============================================================================
// ДЛЯ ПАЗ
// ============================================================================
uint8_t OtaMirrorService::troubleCount(char* out, size_t cap) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < _hostCount; ++i) {
        // Беда = зеркало хоста НИ РАЗУ не собрано и есть ошибка. Ошибка
        // опроса при живом зеркале (версия есть) — не беда: раздача работает.
        if (_hosts[i].lastErr[0] != '\0' && _hosts[i].version[0] == '\0') {
            if (n == 0 && out != nullptr && cap > 0) {
                snprintf(out, cap, "%s:%s", _hosts[i].host, _hosts[i].lastErr);
            }
            ++n;
        }
    }
    return n;
}
