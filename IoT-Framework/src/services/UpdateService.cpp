// ============================================================================
// UpdateService.cpp — реализация OTA с A/B-откатом
// ============================================================================
#include "UpdateService.h"
#include "ConfigService.h"
#include "NetworkManager.h"
#include "StorageService.h"
#include "TimeService.h"
#include "../core/Events.h"
#include "../core/ResourceManager.h"
#include "../core/Version.h"
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <Update.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <cstring>
#include <cstdlib>

// Журнал истории OTA (NDJSON; append не атомарен — хвост может побиться
// при обесточке, см. StorageService::appendFile; история — не критична).
static constexpr const char* OTA_HISTORY_PATH = "/ota_history.ndjson";
// NVS-replay исхода OTA (переживает замену ФС): пакет до двух записей
// (fw+fs), проигрывается в историю при следующей загрузке (урок 5.0.14).
static constexpr const char* OTA_NVS_NS  = "ota";
static constexpr const char* OTA_NVS_KEY = "replay";

UpdateService& UpdateService::getInstance() {
    static UpdateService instance;
    return instance;
}

// ============================================================================
// КОНФИГ-СХЕМА (залежь №3)
// ============================================================================
void UpdateService::registerExtensions() {
    // Только ПУТЬ: полный URL в поле не влезает (CFG_VALUE_LEN=48) и дублирует
    // то, что система и так знает (mqtt.host, sys.hostname). Пустое значение
    // = умолчание "local/ota" (статический каталог HA, схема монолита).
    ConfigService::getInstance().addFields("Система", {
        { "sys.ota_path", ConfigType::STRING, "local/ota", 0, 0, CFG_NONE,
          "Система", "OTA: путь на HA (http://<mqtt>:8123/<путь>/<host>/version.json)" },
    });
}

// ============================================================================
// INIT: определить состояние текущего раздела
// ============================================================================
void UpdateService::init() {
    _bootMs = millis();

    // --- Версия прошивки ---------------------------------------------------
    // Урок 5.0.10: esp_app_desc_t.version — git-хэш ЯДРА Arduino (ee57070),
    // одинаковый у всех сборок МикроОС: по нему 5.0.8 и 5.0.9 неразличимы,
    // рассинхрон «прошивка/FS» оператор не видел. Версия МикроОС — из
    // core/Version.h (единый источник, бамп при каждом релизе); хэш ядра
    // оставляем рядом для глубокой диагностики.
    safeStrCopy(_fwVersion, sizeof(_fwVersion), MICROOS_VERSION);
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc != nullptr) {
        safeStrCopy(_fwBuild, sizeof(_fwBuild), desc->version);
    }

    // --- Состояние раздела: PENDING_VERIFY = свежая прошивка после OTA ----
    esp_ota_img_states_t state;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &state) == ESP_OK) {
        _pendingValidation = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }

    if (_pendingValidation) {
        log(LogLevel::Warning,
            "firmware %s is PENDING_VERIFY: will validate after %lu s stable",
            _fwVersion, (unsigned long)(OTA_VALIDATE_AFTER_MS / 1000));
        // Если эта прошивка упадёт/зависнет ДО валидации — загрузчик ESP32
        // при следующей загрузке сам вернёт предыдущий раздел. Ничего
        // делать не нужно: молчание = отказ.
    } else {
        log(LogLevel::Info, "firmware %s (build %s), partition 0x%lx, validated",
            _fwVersion, _fwBuild, (unsigned long)(running ? running->address : 0));
    }

    // NVS-реестр namespace (A2) + replay исхода прошлого OTA: если до
    // ребута писали ФС, /ota_history.ndjson умерла вместе с разделом —
    // исход лежал в NVS и сейчас ляжет в историю новой ФС (урок 5.0.14).
    if (!ResourceManager::getInstance().claimNvsNamespace(OTA_NVS_NS,
                                                        "UpdateService")) {
        log(LogLevel::Warning, "NVS ns '%s' уже занят", OTA_NVS_NS);
    }
    replayLoad();
    _initialized = true;
}

void UpdateService::start() {
    _started = true;
}

void UpdateService::stop() {
    _started = false;
}

// ============================================================================
// TICK: авто-валидация свежей прошивки после 60 с стабильной работы
// ============================================================================
void UpdateService::tick() {
    // Отложенная проверка манифеста (MQTT-команда «ota check») — в контексте
    // loop, где HTTPClient безопасен по стеку.
    if (_checkRequested) {
        _checkRequested = false;
        checkRemote();
    }

    // Phase 4: отложенный запуск фоновой загрузки (кнопка панели /
    // MQTT «ota update»), затем кооперативная качалка — кусок за тик.
    if (_dlRequested) {
        _dlRequested = false;
        dlStart();
    }
    dlTick();
    // Авторебут после успешной записи всех частей (1.5 с — панель успевает
    // увидеть dl.state=done и показать «Перезагрузка…»).
    if (_restartAtMs != 0 && (int32_t)(millis() - _restartAtMs) >= 0) {
        ESP.restart();
    }

    if (!_pendingValidation) return;
    if (millis() - _bootMs < OTA_VALIDATE_AFTER_MS) return;

    esp_err_t rc = esp_ota_mark_app_valid_cancel_rollback();
    _pendingValidation = false;

    ShEventData d; d.clear();
    if (rc == ESP_OK) {
        log(LogLevel::Info, "firmware %s VALIDATED (rollback cancelled)",
            _fwVersion);
        postEvent(OTA_EVENT_SUCCESS, &d);
        // Залежь №3: свежая прошивка проработала минуту и признана валидной —
        // точка в истории («какая прошивка когда встала окончательно»).
        historyAdd("boot", "fw", true, 0,
                   (uint16_t)((millis() - _bootMs) / 1000), "validated");
    } else {
        // Не удалось снять флаг — следующая загрузка сделает откат.
        // Фиксируем, но не паникуем: устройство работает.
        log(LogLevel::Error, "validate failed rc=%d, rollback will occur",
            (int)rc);
        safeStrCopy(d.payload, sizeof(d.payload), "VALIDATE_FAILED");
        postEvent(OTA_EVENT_FAILED, &d);
        historyAdd("boot", "fw", false, 0, 0, "validate_failed", _fwVersion);
    }
}

// ============================================================================
// ОПЕРАЦИИ
// ============================================================================
void UpdateService::requestRollback() {
    log(LogLevel::Critical, "rollback requested -> previous partition, REBOOT");
    historyAdd("rollback", "fw", true, 0, 0, "");
    ShEventData d; d.clear();
    postEvent(OTA_EVENT_ROLLBACK, &d);
    delay(200);   // дать событию уйти в очередь шины
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Сюда не возвращаемся.
}

// ============================================================================
// УДАЛЁННОЕ ОБНОВЛЕНИЕ (Phase 4): устройство САМО скачивает образы по манифесту
// ============================================================================
// Цепочка: конвейер Build Master публикует образы + version.json на HA ->
// панель (кнопка «Обновить с сервера») или MQTT («ota update») ставит
// _dlRequested -> tick() стартует машину состояний. Качалка КООПЕРАТИВНАЯ:
// бюджет OTA_DL_TICK_BUDGET байт за тик — loop не блокируется на секунды
// (WDT 10 с не дремлет, веб-панель живёт и показывает прогресс).
// Защита слоями: magic-контроль головы образа (как web-upload), md5 из
// манифеста (если конвейер публикует), min_fs_version (совместимость ФС,
// A1), A/B-валидация с автооткатом загрузчика после ребута.
// ============================================================================

// Сравнение «x.y.z» (битые строки вроде «..2» считаются 0.0.0).
static int semverCmp(const char* a, const char* b) {
    int pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
    sscanf(a, "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
    sscanf(b, "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
    for (int i = 0; i < 3; ++i) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

void UpdateService::urlResolve(const char* src, char* out, size_t n) const {
    if (strncmp(src, "http", 4) == 0) {
        safeStrCopy(out, n, src);   // абсолютный URL из манифеста — как есть
        return;
    }
    if (src[0] == '/') {
        // Абсолютный путь — от корня HA
        char host[CFG_VALUE_LEN];
        cfgGetStr("mqtt.host", host, sizeof(host), "");
        snprintf(out, n, "http://%s:8123%s", host, src);
        return;
    }
    // Просто имя файла — от КАТАЛОГА манифеста (урок 5.0.12: Build Master
    // кладёт firmware.bin/littlefs.bin рядом с version.json, fw_url в
    // манифесте — «firmware.bin»).
    char murl[128];
    manifestUrl(murl, sizeof(murl));
    char* slash = strrchr(murl, '/');
    if (slash != nullptr) {
        slash[1] = '\0';
        snprintf(out, n, "%s%s", murl, src);
    } else {
        safeStrCopy(out, n, src);
    }
}

void UpdateService::dlCleanup() {
    if (_dlHttp != nullptr) {
        HTTPClient* http = (HTTPClient*)_dlHttp;
        http->end();
        delete http;
        _dlHttp = nullptr;
    }
}

void UpdateService::dlFail(const char* err) {
    safeStrCopy(_dlErr, sizeof(_dlErr), err ? err : "?");
    log(LogLevel::Error, "OTA download failed: %s", _dlErr);
    if (_dlState == DlState::Fw || _dlState == DlState::Fs) {
        historyAdd("remote", (_dlState == DlState::Fs) ? "fs" : "fw",
                   false, _dlBytes,
                   (uint16_t)((millis() - _dlStartMs) / 1000), _dlErr);
        if (_dlState == DlState::Fs && _dlBegun) {
            // Update.begin СТЁР раздел: и эта запись, и fw-запись в файле
            // истории мертвы. Исход — в NVS (fw уже лежит там с момента
            // финиша своей части). Не begin'ули — раздел цел, файл жив.
            replaySave("remote", "fs", false, _dlBytes,
                       (uint16_t)((millis() - _dlStartMs) / 1000), _dlErr,
                       _remoteVersion);
        }
        if (_dlBegun) Update.abort();   // часть могла быть наполовину записана
    }
    dlCleanup();
    _dlState = DlState::Failed;
}

void UpdateService::dlStart() {
    _dlState = DlState::Idle;
    _dlErr[0] = '\0';
    _dlFwOk = false;
    _restartAtMs = 0;

    if (_rxState == OtaRxState::Receiving) { dlFail("upload_active"); return; }
    if (!NetworkService::getInstance().isConnected()) {
        dlFail("no_network");
        return;
    }
    // Манифест должен быть свежим: конвейер мог выложить релиз только что.
    if (_lastCheckMs == 0 ||
        millis() - _lastCheckMs > OTA_DL_MANIFEST_MAX_AGE_MS) {
        log(LogLevel::Info, "OTA download: refreshing manifest first");
        if (!checkRemote()) { dlFail("manifest_unavailable"); return; }
    }
    if (_remoteFwUrl[0] == '\0') { dlFail("no_fw_url"); return; }

    if (!dlBeginPart(false)) return;   // dlFail уже вызван внутри
    log(LogLevel::Info, "OTA download started (fw%s)",
        (_dlWithFs && _remoteFsUrl[0]) ? " + fs" : "");
}

bool UpdateService::dlBeginPart(bool fs) {
    // --- Маркер совместимости ФС (A1): min_fs_version -------------------
    if (fs && _remoteMinFs[0] != '\0') {
        // Эффективная прошивка для ФС: если в этом сеансе записали новую —
        // она и встанет рядом с ФС после общего ребута.
        const char* eff = _dlFwOk ? _remoteVersion : _fwVersion;
        if (semverCmp(eff, _remoteMinFs) < 0) {
            log(LogLevel::Warning,
                "OTA download: fs skipped (needs fw >= %s, have %s)",
                _remoteMinFs, eff);
            historyAdd("remote", "fs", false, 0, 0, "min_fs_version",
                       _remoteMinFs);
            // Раздел НЕ трогали — файл истории доживёт до ребута, replay
            // fw-части в NVS не нужен (иначе на новой прошивке будет дубль).
            StorageService::getInstance().nvsRemove(OTA_NVS_NS, OTA_NVS_KEY);
            _dlState = DlState::Done;   // fw записан — ребут всё равно нужен
            _restartAtMs = millis() + 1500;
            return false;
        }
    }

    // Урок 5.0.14: Update.begin СТИРАЕТ раздел сразу — он отложен до
    // первого куска в dlTick, где по голове образа проверяется магия.
    // «Не тот» файл по ссылке отклоняется ДО смерти раздела; демонт
    // LittleFS — тоже там, в последний ответственный момент.

    // Состояние — СРАЗУ: dlFail из любой точки ниже запишет в историю
    // правильную часть (иначе фейл старта ФС лёг бы как «fw»).
    _dlState = fs ? DlState::Fs : DlState::Fw;

    char url[96];
    urlResolve(fs ? _remoteFsUrl : _remoteFwUrl, url, sizeof(url));

    HTTPClient* http = new HTTPClient;
    if (http == nullptr) { dlFail("no_heap"); return false; }
    http->setTimeout(OTA_DL_TIMEOUT_MS);
    if (!http->begin(url)) { delete http; dlFail("bad_url"); return false; }
    int code = http->GET();
    if (code != 200) {
        char e[24];
        snprintf(e, sizeof(e), "http_%d", code);
        http->end();
        delete http;
        dlFail(e);
        return false;
    }

    _dlHttp = http;
    _dlBegun = false;
    _dlBytes = 0;
    _dlTotal = http->getSize();   // -1, если сервер не сказал длину
    _dlLastRxMs = millis();
    _dlStartMs = millis();
    log(LogLevel::Info, "OTA download: %s from %s (%ld bytes)",
        fs ? "fs" : "fw", url, (long)_dlTotal);
    return true;
}

void UpdateService::dlTick() {
    if (_dlState != DlState::Fw && _dlState != DlState::Fs) return;
    HTTPClient* http = (HTTPClient*)_dlHttp;
    if (http == nullptr) { dlFail("lost_http"); return; }
    WiFiClient* stream = http->getStreamPtr();
    const bool fs = (_dlState == DlState::Fs);

    // Статик, не стек: 4 КБ в loopTask + глубокая цепочка flash-записи
    // на S3 — та же мина, что «stack canary» в ConfigService (06.08.2026).
    // dlTick зовётся только из tick (один поток), гонок нет.
    static uint8_t buf[4096];
    size_t budget = OTA_DL_TICK_BUDGET;
    while (budget > 0) {
        int avail = stream->available();
        if (avail <= 0) break;
        size_t want = ((size_t)avail < sizeof(buf)) ? (size_t)avail
                                                    : sizeof(buf);
        if (want > budget) want = budget;
        int r = stream->read(buf, want);
        if (r <= 0) break;
        // Отложенный begin (урок 5.0.14): сначала магия головы — тот же
        // контроль, что у web-upload (урок 5.0.7), — и только потом
        // раздел стирается Update.begin. «Не тот» файл по ссылке не
        // должен убить устройство.
        if (!_dlBegun) {
            if (!fs && buf[0] != 0xE9) { dlFail("bad_magic"); return; }
            if (fs && r >= 16 && memcmp(buf + 8, "littlefs", 8) != 0) {
                dlFail("bad_magic");
                return;
            }
            // Раздел уходит под запись: LittleFS демонтируем, чтобы
            // записи сервисов не легли поверх свежего образа (война
            // корневых метапар на блоках 0/1 после ребута).
            if (fs) StorageService::getInstance().unmountFs();
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, fs ? U_SPIFFS : U_FLASH)) {
                dlFail("update_begin");
                return;
            }
            // Контроль целостности — если конвейер публикует md5 части.
            const char* md5 = fs ? _remoteFsMd5 : _remoteMd5;
            if (md5[0] != '\0') Update.setMD5(md5);
            _dlBegun = true;
        }
        if (Update.write(buf, (size_t)r) != (size_t)r) {
            dlFail("write_error");
            return;
        }
        _dlBytes += (size_t)r;
        budget -= (size_t)r;
        _dlLastRxMs = millis();
    }

    // Стойл: сервер молчит дольше лимита — обрыв (история зафиксирует).
    if (millis() - _dlLastRxMs > OTA_DL_STALL_MS) {
        dlFail("stall_timeout");
        return;
    }
    // Финиш известен только по Content-Length; сервер без длины —
    // считаем конец по закрытию потока (connected()==false и пусто).
    if (_dlTotal > 0 && _dlBytes >= (uint32_t)_dlTotal) {
        dlFinishPart();
    } else if (_dlTotal <= 0 && !stream->connected() &&
               stream->available() == 0 && _dlBytes > 0) {
        dlFinishPart();
    }
}

void UpdateService::dlFinishPart() {
    const bool fs = (_dlState == DlState::Fs);
    uint16_t dur = (uint16_t)((millis() - _dlStartMs) / 1000);
    if (!_dlBegun) {   // ноль байтов — раздел не тронут, просто авария
        dlFail("empty_download");
        return;
    }
    if (!Update.end(true)) {   // true — даже если пришло меньше объявленного
        char e[32];
        snprintf(e, sizeof(e), "end: %.20s", Update.errorString());
        dlFail(e);
        return;
    }
    dlCleanup();
    historyAdd("remote", fs ? "fs" : "fw", true, _dlBytes, dur, "",
               _remoteVersion);
    log(LogLevel::Info, "OTA download: %s done, %lu bytes in %u s",
        fs ? "fs" : "fw", (unsigned long)_dlBytes, dur);

    if (!fs) {
        _dlFwOk = true;
        if (_dlWithFs && _remoteFsUrl[0] != '\0') {
            // ФС-часть сотрёт раздел вместе с файлом истории — исход fw
            // заранее кладём в NVS, проиграется на новой ФС после ребута.
            replaySave("remote", "fw", true, _dlBytes, dur, "",
                       _remoteVersion);
            dlBeginPart(true);   // сразу вторая часть — ребут будет общим
            return;
        }
    } else {
        // ФС записана: её история-запись выше ушла в демонтированную ФС
        // (мёртвый груз) — исход надёжно лежит только в NVS.
        replaySave("remote", "fs", true, _dlBytes, dur, "", _remoteVersion);
    }
    _dlState = DlState::Done;
    _restartAtMs = millis() + 1500;   // ребут применит обе части разом
}

// ============================================================================
// WEB-UPLOAD (залежь №3): приём образа из панели кусками
// ============================================================================
// Обработчики HttpService живут в контексте loop (handleClient из tick),
// поэтому здесь никаких задач/мьютексов: приём — просто стейт-машина
// поверх Arduino Update. A/B-валидация после ребута — штатная (init/tick).
bool UpdateService::uploadBegin(int command, size_t size) {
    if (_rxState == OtaRxState::Receiving && _rxBegun) {
        Update.abort();   // повторный begin обрывает брошенный приём
    }
    _rxState = OtaRxState::Idle;
    _rxBytes = 0;
    _rxError[0] = '\0';
    _rxHeadLen = 0;
    _rxVersion[0] = '\0';
    _rxCommand = command;
    _rxSize = size;
    _rxBegun = false;
    _rxStartMs = millis();

    // Урок 5.0.14: Update.begin СТИРАЕТ раздел сразу — поэтому он отложен
    // до первого куска (uploadWrite), где по голове образа проверяется
    // магия. «Не тот» файл отклоняется ДО смерти раздела; демонт LittleFS
    // — тоже там, в последний ответственный момент.
    _rxState = OtaRxState::Receiving;

    ShEventData d; d.clear();
    d.code = command;
    safeStrCopy(d.payload, sizeof(d.payload),
                command == U_SPIFFS ? "FS" : "FW");
    postEvent(OTA_EVENT_STARTED, &d);
    log(LogLevel::Info, "OTA upload begin (%s, size %s)",
        command == U_SPIFFS ? "FS" : "FW",
        size == 0 ? "unknown" : "known");
    return true;
}

bool UpdateService::uploadWrite(const uint8_t* data, size_t len) {
    if (_rxState != OtaRxState::Receiving || len == 0) return false;

    // --- Отложенный begin (урок 5.0.14): сначала магия, потом раздел -----
    if (!_rxBegun) {
        // Защита от «не того» файла (урок 5.0.7). Прошивка ESP32 ВСЕГДА
        // начинается с 0xE9 (image magic); образ ФС — с магии "littlefs"
        // на смещении 8 суперблока. Отклоняем ДО Update.begin — раздел
        // при этом вообще не тронут.
        if (_rxCommand == U_FLASH && data[0] != 0xE9) {
            safeStrCopy(_rxError, sizeof(_rxError),
                        "bad magic: not an ESP32 app");
            _rxState = OtaRxState::Error;
            log(LogLevel::Error,
                "OTA uploadWrite: rejected, first byte 0x%02x",
                (unsigned)data[0]);
            return false;
        }
        if (_rxCommand == U_SPIFFS && len >= 16 &&
            memcmp(data + 8, "littlefs", 8) != 0) {
            safeStrCopy(_rxError, sizeof(_rxError),
                        "bad magic: not a LittleFS image");
            _rxState = OtaRxState::Error;
            log(LogLevel::Error,
                "OTA uploadWrite: rejected, no littlefs magic");
            return false;
        }
        // Раздел уходит под запись: LittleFS демонтируем, чтобы записи
        // сервисов не легли поверх свежего образа (война метапар 0/1).
        if (_rxCommand == U_SPIFFS) {
            StorageService::getInstance().unmountFs();
        }
        if (!Update.begin(_rxSize == 0 ? UPDATE_SIZE_UNKNOWN : _rxSize,
                          _rxCommand)) {
            snprintf(_rxError, sizeof(_rxError), "begin: %s",
                     Update.errorString());
            _rxState = OtaRxState::Error;
            log(LogLevel::Error, "OTA uploadBegin failed: %s", _rxError);
            return false;
        }
        _rxBegun = true;
    }
    // Голова образа — для esp_app_desc_t (версия заливаемой прошивки).
    if (_rxHeadLen < sizeof(_rxHead)) {
        size_t room = sizeof(_rxHead) - _rxHeadLen;
        size_t take = (len < room) ? len : room;
        memcpy(_rxHead + _rxHeadLen, data, take);
        _rxHeadLen += (uint8_t)take;
    }

    size_t written = Update.write(const_cast<uint8_t*>(data), len);
    _rxBytes += written;
    if (written != len) {
        snprintf(_rxError, sizeof(_rxError), "write: %s", Update.errorString());
        _rxState = OtaRxState::Error;
        Update.abort();
        log(LogLevel::Error, "OTA uploadWrite failed at %u: %s",
            (unsigned)_rxBytes, _rxError);
        return false;
    }
    return true;
}

bool UpdateService::uploadEnd() {
    if (_rxState != OtaRxState::Receiving) return false;
    if (!_rxBegun) {   // ни одного куска — раздел не тронут, просто отказ
        safeStrCopy(_rxError, sizeof(_rxError), "empty upload");
        _rxState = OtaRxState::Error;
        log(LogLevel::Error, "OTA uploadEnd: empty upload");
        return false;
    }
    // evenIfRemaining=true: при UPDATE_SIZE_UNKNOWN остаток неизвестен.
    if (!Update.end(true)) {
        snprintf(_rxError, sizeof(_rxError), "end: %s", Update.errorString());
        _rxState = OtaRxState::Error;
        log(LogLevel::Error, "OTA uploadEnd failed: %s", _rxError);
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), _rxError);
        postEvent(OTA_EVENT_FAILED, &d);
        historyAdd("web", _rxCommand == U_SPIFFS ? "fs" : "fw", false,
                   (uint32_t)_rxBytes,
                   (uint16_t)((millis() - _rxStartMs) / 1000), _rxError,
                   _rxVersion);
        if (_rxCommand == U_SPIFFS)   // раздел уже перезаписан — в NVS
            replaySave("web", "fs", false, (uint32_t)_rxBytes,
                       (uint16_t)((millis() - _rxStartMs) / 1000), _rxError,
                       _rxVersion);
        return false;
    }
    uint16_t dur = (uint16_t)((millis() - _rxStartMs) / 1000);
    _rxState = OtaRxState::Idle;

    // --- Версия залитого образа из esp_app_desc_t ----------------------------
    // Раскладка бинаря: image header (0x18) + segment header (0x8) ->
    // esp_app_desc_t @ 0x20: magic 0xABCD5432 @ +0x00, version[32] @ +0x10.
    // Итого версия — байты [0x30, 0x50) — ровно влезает в _rxHead[80].
    if (_rxCommand == U_FLASH && _rxHeadLen >= 80 &&
        _rxHead[0x20] == 0x32 && _rxHead[0x21] == 0x54 &&
        _rxHead[0x22] == 0xCD && _rxHead[0x23] == 0xAB) {
        memcpy(_rxVersion, _rxHead + 0x30, 32);
        _rxVersion[32] = '\0';
        log(LogLevel::Info, "OTA uploaded firmware version: %s", _rxVersion);
    }

    log(LogLevel::Info, "OTA upload complete: %u bytes in %u s -> REBOOT",
        (unsigned)_rxBytes, (unsigned)dur);
    ShEventData d; d.clear();
    d.code = (int32_t)(_rxBytes / 1024);
    postEvent(OTA_EVENT_SUCCESS, &d);
    historyAdd("web", _rxCommand == U_SPIFFS ? "fs" : "fw", true,
               (uint32_t)_rxBytes, dur, "", _rxVersion);
    if (_rxCommand == U_SPIFFS)   // ФС перезаписана — исход надёжно в NVS
        replaySave("web", "fs", true, (uint32_t)_rxBytes, dur, "",
                   _rxVersion);
    return true;
}

void UpdateService::uploadAbort() {
    bool wasReceiving = (_rxState == OtaRxState::Receiving);
    if (wasReceiving && _rxBegun) Update.abort();
    if (wasReceiving && _rxBytes > 0) {
        historyAdd("web", _rxCommand == U_SPIFFS ? "fs" : "fw", false,
                   (uint32_t)_rxBytes,
                   (uint16_t)((millis() - _rxStartMs) / 1000), "abort");
        if (_rxCommand == U_SPIFFS && _rxBegun)   // раздел частично перезаписан
            replaySave("web", "fs", false, (uint32_t)_rxBytes,
                       (uint16_t)((millis() - _rxStartMs) / 1000), "abort",
                       _rxVersion);
    }
    _rxState = OtaRxState::Idle;
    _rxBytes = 0;
    _rxError[0] = '\0';
}

// ============================================================================
// МАНИФЕСТ ОБНОВЛЕНИЙ (залежь №3): version.json на сервере HA
// ============================================================================
void UpdateService::manifestUrl(char* buf, size_t n) const {
    char host[CFG_VALUE_LEN];
    char hname[CFG_VALUE_LEN];
    char path[CFG_VALUE_LEN];
    cfgGetStr("mqtt.host", host, sizeof(host), "");
    cfgGetStr("sys.hostname", hname, sizeof(hname), "microos");
    cfgGetStr("sys.ota_path", path, sizeof(path), "local/ota");
    if (path[0] == '\0') safeStrCopy(path, sizeof(path), "local/ota");
    // Схема монолита v2.5.0: статика HA (www/ -> /local/) рядом с брокером
    snprintf(buf, n, "http://%s:8123/%s/%s/version.json",
             host[0] ? host : "0.0.0.0", path, hname[0] ? hname : "microos");
}

// Извлечение строки из JSON с ПРОИЗВОЛЬНЫМИ пробелами (ArduinoJson в проекте
// нет). Урок 5.0.7: манифест монолита форматирован для людей —
// "version": "2.4.4" (пробел после двоеточия), а парсер искал точное
// "version":" -> устройство видело 200 OK, но считало манифест «недоступным».
static bool otaJsonStr(const char* js, const char* key, char* out, size_t n) {
    const char* p = strstr(js, key);        // key = "\"version\"" (без ':'!)
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

bool UpdateService::checkRemote() {
    _lastCheckMs = millis();
    _updateAvailable = false;
    _remoteVersion[0] = '\0';

    if (!NetworkService::getInstance().isConnected()) {
        log(LogLevel::Warning, "OTA check: no network");
        return false;
    }
    char url[128];
    manifestUrl(url, sizeof(url));
    if (strstr(url, "0.0.0.0") != nullptr) {
        log(LogLevel::Warning, "OTA check: mqtt.host not set, no manifest url");
        return false;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    if (code != 200) {
        log(LogLevel::Warning, "OTA check: HTTP %d for %s", code, url);
        http.end();
        return false;
    }
    // Манифест крошечный — читаем в буфер (не String: ядро без динамики).
    char js[512];   // монолитный формат с changelog/md5 длиннее «version»-only
    size_t got = http.getStream().readBytes(js, sizeof(js) - 1);
    js[got] = '\0';
    http.end();

    if (!otaJsonStr(js, "\"version\"", _remoteVersion,
                    sizeof(_remoteVersion))) {
        // Первые байты тела — в лог: при следующем «манифест недоступен»
        // будет видно, ЧТО реально ответил сервер (урок 5.0.7).
        log(LogLevel::Warning, "OTA check: manifest without version: %.120s", js);
        return false;
    }
    // Монолитный формат (v2.5.0): changelog + fw_url/fs_url + md5.
    // Поля необязательные — отсутствие не ошибка.
    otaJsonStr(js, "\"changelog\"", _remoteNotes, sizeof(_remoteNotes));
    otaJsonStr(js, "\"fw_url\"", _remoteFwUrl, sizeof(_remoteFwUrl));
    otaJsonStr(js, "\"fs_url\"", _remoteFsUrl, sizeof(_remoteFsUrl));
    // Phase 4: контроль целостности и совместимости (если конвейер их
    // публикует; нет полей — защищают магия образов + A/B-откат).
    // Урок 5.0.12: у Build Master поле прошивки — «fw_md5», не «md5».
    otaJsonStr(js, "\"fw_md5\"", _remoteMd5, sizeof(_remoteMd5));
    if (_remoteMd5[0] == '\0') {
        otaJsonStr(js, "\"md5\"", _remoteMd5, sizeof(_remoteMd5));
    }
    otaJsonStr(js, "\"fs_md5\"", _remoteFsMd5, sizeof(_remoteFsMd5));
    otaJsonStr(js, "\"min_fs_version\"", _remoteMinFs, sizeof(_remoteMinFs));

    // Направление важно (урок 5.0.14): обновление — только если сервер
    // СТРОГО новее. strcmp-различие ловило и ДАУНГРЕЙД: после отката
    // панель предлагала «обновиться» на более старую версию с сервера.
    _updateAvailable = (semverCmp(_remoteVersion, _fwVersion) > 0);
    log(LogLevel::Info, "OTA check: local %s, remote %s -> %s",
        _fwVersion, _remoteVersion,
        _updateAvailable ? "UPDATE AVAILABLE" : "up to date");
    return true;
}

// ============================================================================
// ИСТОРИЯ OTA (залежь №3): кольцо в RAM + NDJSON на LittleFS
// ============================================================================
void UpdateService::historyAdd(const char* src, const char* type, bool ok,
                               uint32_t bytes, uint16_t dur_s,
                               const char* err, const char* ver) {
    if (!_historyLoaded) historyLoad();

    OtaHistoryEntry& e = _history[_historyHead];
    e.unix = (uint32_t)TimeService::getInstance().getUnixTime();
    if (e.unix < 1700000000UL) e.unix = 0;  // часы не выставлены — не врём
    e.uptime_s = millis() / 1000;
    e.ok = ok ? 1 : 0;
    safeStrCopy(e.src, sizeof(e.src), src ? src : "?");
    safeStrCopy(e.type, sizeof(e.type), type ? type : "?");
    e.bytes = bytes;
    e.dur_s = dur_s;
    safeStrCopy(e.err, sizeof(e.err), err ? err : "");
    safeStrCopy(e.ver, sizeof(e.ver), ver ? ver : "");
    for (char* p = e.err; *p; ++p) if (*p == '"' || *p == '\\') *p = '\'';
    for (char* p = e.ver; *p; ++p) if (*p == '"' || *p == '\\') *p = '\'';

    _historyHead = (uint8_t)((_historyHead + 1) % OTA_HISTORY_SIZE);
    if (_historyCount < OTA_HISTORY_SIZE) _historyCount++;

    historyPersist(e);
}

void UpdateService::historyPersist(const OtaHistoryEntry& e) {
    char line[224];
    int n = snprintf(line, sizeof(line),
        "{\"unix\":%lu,\"uptime_s\":%lu,\"ok\":%u,\"src\":\"%s\","
        "\"type\":\"%s\",\"bytes\":%lu,\"dur_s\":%u,\"err\":\"%s\","
        "\"ver\":\"%s\"}\n",
        (unsigned long)e.unix, (unsigned long)e.uptime_s, (unsigned)e.ok,
        e.src, e.type, (unsigned long)e.bytes, (unsigned)e.dur_s, e.err, e.ver);
    if (n <= 0) return;
    StorageService::getInstance().appendFile(OTA_HISTORY_PATH, line);
}

// ============================================================================
// NVS-REPLAY ИСХОДА OTA (урок 5.0.14)
// ============================================================================
// Запись ФС-образа стирает раздел с /ota_history.ndjson (и вообще со всеми
// runtime-файлами без NVS-зеркал), поэтому исход каждой записанной части
// кладём в NVS: он переживает замену ФС. При следующей загрузке replayLoad
// проигрывает записи в историю уже новой ФС и очищает ключ. Запись в NVS
// идёт ПАРАЛЛЕЛЬНО обычному historyAdd — если ФС не трогали (только
// прошивка / min_fs-skip), replay всё равно сработает и запись просто
// продублируется... нет: история-кольцо держит 5, дубль не страшен, но мы
// зовём replaySave ТОЛЬКО когда раздел реально перезаписан (ФС-части) —
// fw-запись кладём заранее, т.к. последующая ФС-часть сотрёт её из файла.
void UpdateService::replaySave(const char* src, const char* type, bool ok,
                               uint32_t bytes, uint16_t dur_s,
                               const char* err, const char* ver) {
    StorageService& fs = StorageService::getInstance();
    OtaReplayPack pack;
    memset(&pack, 0, sizeof(pack));
    size_t got = fs.nvsRestore(OTA_NVS_NS, OTA_NVS_KEY, &pack, sizeof(pack));
    if (got != sizeof(pack) || pack.magic != OTA_REPLAY_MAGIC ||
        pack.count >= OTA_REPLAY_MAX) {
        memset(&pack, 0, sizeof(pack));   // чужое/битое/полное — начать с нуля
        pack.magic = OTA_REPLAY_MAGIC;
        pack.count = 0;
    }
    OtaReplayEntry& e = pack.e[pack.count];
    e.unix = (uint32_t)TimeService::getInstance().getUnixTime();
    if (e.unix < 1700000000UL) e.unix = 0;
    e.bytes = bytes;
    e.dur_s = dur_s;
    e.ok = ok ? 1 : 0;
    safeStrCopy(e.src, sizeof(e.src), src ? src : "?");
    safeStrCopy(e.type, sizeof(e.type), type ? type : "?");
    safeStrCopy(e.ver, sizeof(e.ver), ver ? ver : "");
    safeStrCopy(e.err, sizeof(e.err), err ? err : "");
    for (char* p = e.err; *p; ++p) if (*p == '"' || *p == '\\') *p = '\'';
    pack.count++;
    if (!fs.nvsBackup(OTA_NVS_NS, OTA_NVS_KEY, &pack, sizeof(pack))) {
        log(LogLevel::Warning, "ota replay: NVS save failed");
    }
}

void UpdateService::replayLoad() {
    StorageService& fs = StorageService::getInstance();
    OtaReplayPack pack;
    size_t got = fs.nvsRestore(OTA_NVS_NS, OTA_NVS_KEY, &pack, sizeof(pack));
    if (got != sizeof(pack) || pack.magic != OTA_REPLAY_MAGIC) return;
    fs.nvsRemove(OTA_NVS_NS, OTA_NVS_KEY);   // одноразовая запись

    if (!_historyLoaded) historyLoad();
    for (uint8_t i = 0; i < pack.count && i < OTA_REPLAY_MAX; ++i) {
        const OtaReplayEntry& r = pack.e[i];
        OtaHistoryEntry& e = _history[_historyHead];
        e.unix = r.unix;                     // исходное время, не «сейчас»
        e.uptime_s = 0;                      // аптайм прошлой жизни неизвестен
        e.ok = r.ok;
        safeStrCopy(e.src, sizeof(e.src), r.src);
        safeStrCopy(e.type, sizeof(e.type), r.type);
        e.bytes = r.bytes;
        e.dur_s = r.dur_s;
        safeStrCopy(e.err, sizeof(e.err), r.err);
        safeStrCopy(e.ver, sizeof(e.ver), r.ver);
        _historyHead = (uint8_t)((_historyHead + 1) % OTA_HISTORY_SIZE);
        if (_historyCount < OTA_HISTORY_SIZE) _historyCount++;
        historyPersist(e);
        log(LogLevel::Info, "ota replay: %s %s %s (%lu bytes)",
            r.src, r.type, r.ok ? "OK" : "FAIL", (unsigned long)r.bytes);
    }
}

static bool otaJGetU32(const char* line, const char* key, uint32_t& out) {
    const char* p = strstr(line, key);
    if (p == nullptr) return false;
    out = (uint32_t)strtoul(p + strlen(key), nullptr, 10);
    return true;
}

void UpdateService::historyLoad() {
    _historyLoaded = true;
    _historyHead = 0;
    _historyCount = 0;

    StorageService& fs = StorageService::getInstance();
    size_t size = fs.fileSize(OTA_HISTORY_PATH);
    if (size == 0) return;

    // Читаем ХВОСТ (свежие записи); компакция — ниже. Буфер ОДИН на разбор
    // и компакцию (урок линковки: dram0_0_seg не резиновый).
    static uint8_t buf[OTA_HISTORY_SIZE * 224 + 1];
    size_t cap = sizeof(buf) - 1;
    size_t skip = (size > cap) ? size - cap : 0;
    File f = fs.openRead(OTA_HISTORY_PATH);
    if (!f) return;
    if (skip > 0) f.seek(skip);
    size_t got = f.read(buf, cap);
    f.close();
    if (got == 0) return;
    buf[got] = '\0';

    char* text = (char*)buf;
    if (skip > 0) {
        char* nl = strchr(text, '\n');
        if (nl == nullptr) return;
        text = nl + 1;
    }

    uint16_t lines = 0;
    char* save = nullptr;
    for (char* ln = strtok_r(text, "\n", &save); ln != nullptr;
         ln = strtok_r(nullptr, "\n", &save)) {
        if (ln[0] != '{') continue;
        lines++;
        OtaHistoryEntry& e = _history[_historyHead];
        memset(&e, 0, sizeof(e));
        otaJGetU32(ln, "\"unix\":", e.unix);
        otaJGetU32(ln, "\"uptime_s\":", e.uptime_s);
        uint32_t v = 0;
        if (otaJGetU32(ln, "\"ok\":", v)) e.ok = (uint8_t)v;
        otaJsonStr(ln, "\"src\"", e.src, sizeof(e.src));
        otaJsonStr(ln, "\"type\"", e.type, sizeof(e.type));
        otaJGetU32(ln, "\"bytes\":", e.bytes);
        if (otaJGetU32(ln, "\"dur_s\":", v)) e.dur_s = (uint16_t)v;
        otaJsonStr(ln, "\"err\"", e.err, sizeof(e.err));
        otaJsonStr(ln, "\"ver\"", e.ver, sizeof(e.ver));
        _historyHead = (uint8_t)((_historyHead + 1) % OTA_HISTORY_SIZE);
        if (_historyCount < OTA_HISTORY_SIZE) _historyCount++;
    }

    // Компакция: строк было больше кольца ИЛИ файл разросся за хвост —
    // переписываем хвостом (атомарно). Разбор завершён — буфер свободен.
    if (lines > OTA_HISTORY_SIZE || skip > 0) {
        size_t w = 0;
        for (uint8_t i = 0; i < _historyCount; ++i) {
            uint8_t idx = (_historyCount < OTA_HISTORY_SIZE)
                ? i : (uint8_t)((_historyHead + i) % OTA_HISTORY_SIZE);
            const OtaHistoryEntry& e = _history[idx];
            w += snprintf((char*)buf + w, sizeof(buf) - w,
                "{\"unix\":%lu,\"uptime_s\":%lu,\"ok\":%u,\"src\":\"%s\","
                "\"type\":\"%s\",\"bytes\":%lu,\"dur_s\":%u,\"err\":\"%s\","
                "\"ver\":\"%s\"}\n",
                (unsigned long)e.unix, (unsigned long)e.uptime_s,
                (unsigned)e.ok, e.src, e.type, (unsigned long)e.bytes,
                (unsigned)e.dur_s, e.err, e.ver);
        }
        fs.atomicWrite(OTA_HISTORY_PATH, (const char*)buf);
        log(LogLevel::Info, "ota history compacted: %u -> %u lines",
            (unsigned)lines, (unsigned)_historyCount);
    }
}

// ============================================================================
// СОСТОЯНИЕ ДЛЯ ПАНЕЛИ (/api/ota/info)
// ============================================================================
size_t UpdateService::otaInfoJson(char* buf, size_t n) const {
    if (n == 0) return 0;
    const_cast<UpdateService*>(this)->historyLoad();  // лениво, однажды

    char url[128];
    manifestUrl(url, sizeof(url));
    const char* rxState = (_rxState == OtaRxState::Receiving) ? "receiving"
                        : (_rxState == OtaRxState::Error)     ? "error"
                                                              : "idle";
    const char* dlStateStr = (_dlState == DlState::Fw)      ? "fw"
                           : (_dlState == DlState::Fs)      ? "fs"
                           : (_dlState == DlState::Done)    ? "done"
                           : (_dlState == DlState::Failed)  ? "failed"
                                                            : "idle";
    int w = snprintf(buf, n,
        "{\"fw\":\"%s\",\"build\":\"%s\",\"pending_verify\":%d,"
        "\"rx\":{\"state\":\"%s\",\"bytes\":%u,\"err\":\"%s\",\"ver\":\"%s\"},"
        "\"remote\":{\"checked\":%d,\"update_available\":%d,"
        "\"version\":\"%s\",\"notes\":\"%s\",\"fw_url\":\"%s\","
        "\"fs_url\":\"%s\",\"url\":\"%s\",\"md5\":%d},"
        "\"dl\":{\"state\":\"%s\",\"bytes\":%lu,\"total\":%ld,"
        "\"err\":\"%s\"},\"history\":[",
        _fwVersion, _fwBuild, _pendingValidation ? 1 : 0,
        rxState, (unsigned)_rxBytes, _rxError, _rxVersion,
        _lastCheckMs != 0 ? 1 : 0, _updateAvailable ? 1 : 0,
        _remoteVersion, _remoteNotes, _remoteFwUrl, _remoteFsUrl, url,
        (_remoteMd5[0] || _remoteFsMd5[0]) ? 1 : 0,
        dlStateStr, (unsigned long)_dlBytes, (long)_dlTotal, _dlErr);
    // История — новые первыми
    for (uint8_t i = 0; i < _historyCount && w > 0 && (size_t)w < n - 4; ++i) {
        int idx = (int)_historyHead - 1 - (int)i;
        while (idx < 0) idx += OTA_HISTORY_SIZE;
        const OtaHistoryEntry& e = _history[idx];
        w += snprintf(buf + w, n - w,
            "%s{\"unix\":%lu,\"ok\":%u,\"src\":\"%s\",\"type\":\"%s\","
            "\"bytes\":%lu,\"dur_s\":%u,\"err\":\"%s\",\"ver\":\"%s\"}",
            i ? "," : "",
            (unsigned long)e.unix, (unsigned)e.ok, e.src, e.type,
            (unsigned long)e.bytes, (unsigned)e.dur_s, e.err, e.ver);
    }
    if (w > 0 && (size_t)w < n - 3) w += snprintf(buf + w, n - w, "]}");
    return (w > 0) ? (size_t)w : 0;
}
