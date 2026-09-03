// ============================================================================
// SmartLockUi.cpp — веб-лицо СКУД (см. шапку .h)
// ============================================================================
// Эндпоинты (tail после "/api/dev/"):
//   ПУБЛИЧНЫЕ (жилец):
//     GET  lock/status            — дверь/режим/замок/приветствие/погода/ПАЗ
//     POST lock/open?utok=        — открыть (токен сессии жильца; фолбэк pin)
//     POST lock/user/auth?pin=    — вход жильца: {ok, role, name, utok}
//     POST lock/user/auth?utok=   — проверка/продление сессии: {ok, name}
//     POST lock/user/logout?utok= — отзыв сессии жильца
//   АДМИНСКИЕ (токен ядерной сессии):
//     POST lock/mode?mode=0|1|2   — режим СКУД
//     POST lock/read/start        — режим чтения карты (10 с)
//     GET  lock/read/result       — пойманная карта
//     GET  lock/keys?from=N       — список карт (страницы по 25; has_pin,
//                                   uses, last_use, blocked — статистика 5.0.x)
//     POST lock/keys/add?id=&name=&type=&track=&expiry_days=&pin=
//     POST lock/keys/update?id=&name=&track=&expiry=&pin=
//                                   (все поля опциональны: нет аргумента —
//                                   параметр не меняется; pin="" — снять ПИН)
//     POST lock/keys/remove?id=
//     POST lock/keys/block?id=&blocked=1|0 — блокировка без удаления
//     POST lock/keys/clear?confirm=DELETE
//     POST lock/master/delete?confirm=DELETE
//     POST lock/db/backup | lock/db/restore — NVS-зеркало
//     POST lock/audio/test?track=N — прослушать трек имени (кнопка ▶ монолита)
//     POST lock/paz/reset         — сброс флага ПАЗ (снять тревоги/сирену)
//     GET  lock/dlog/channels     — каналы даталоггера (п.5: графики)
//     GET  lock/dlog?ch=N&range=6h|24h|7d|30d|1y — точки для uPlot
// ============================================================================
#include "SmartLockUi.h"
#include "SmartLockApp.h"
#include "SmartLockConfig.h"
#include "SmartLockTracks.h"
#include "CardStore.h"
#include "LockControl.h"
#include <services/HttpService.h>
#include <services/AuthService.h>
#include <services/TimeService.h>
#include <services/ConfigService.h>
#include <services/AudioService.h>
#include <services/AudioQueue.h>     // SndPriority
#include <services/MqttTransport.h>
#include <services/DataLogService.h>
#include <core/Version.h>          // MICROOS_VERSION — версия в lock/status (5.0.15)
#include <esp_random.h>            // esp_random — токены сессий жильца

// ============================================================================
// АВТОРИЗАЦИЯ
// ============================================================================
bool SmartLockUi::isAdmin(const ShUiRequest& req) const {
    return HttpService::getInstance().isAdminToken(req.token);
}

const SlUser* SmartLockUi::identifyByPin(const ShUiRequest& req,
                                         const char** denyReason) {
    if (denyReason != nullptr) *denyReason = nullptr;
    AuthService& auth = AuthService::getInstance();
    if (auth.isRateLimited("sl_web")) return nullptr;

    const char* got = req.getArg("pin");
    if (got == nullptr || got[0] == '\0') {
        auth.noteFailure("sl_web");
        return nullptr;
    }

    // Per-user ПИН — основной путь v5.0 (монолит: идентичность жильца)
    const SlUser* u = CardStore::getInstance().findByPin(got);
    if (u != nullptr) {
        // Правило зеркала (03.09.2026, репорт владельца: временный ключ
        // Ирины открыл замок через веб ПОСЛЕ истечения срока): веб-доступ
        // подчиняется ТЕМ ЖЕ запретам, что и карточный путь
        // (SmartLockApp::onCardPresented, шаги 2 и 4) — заблокированный и
        // истёкший TEMPORARY не входят и не открывают. ПИН верный — это НЕ
        // brute-force, noteFailure не трогаем; отказ с явной причиной.
        if (u->blocked) {
            SmartLockApp::getInstance().logWebDeny(u->name, "blocked");
            if (denyReason != nullptr) *denyReason = "blocked";
            return nullptr;
        }
        if (u->type == (uint8_t)KeyType::TEMPORARY && u->expiry > 0) {
            time_t now = TimeService::getInstance().getUnixTime();
            if (now == 0 || (uint32_t)now > u->expiry) {
                SmartLockApp::getInstance().logWebDeny(
                    u->name, now == 0 ? "expired_notime" : "expired");
                if (denyReason != nullptr) *denyReason = "expired";
                return nullptr;
            }
        }
        auth.noteSuccess("sl_web");
        return u;
    }

    // Legacy: общий ПИН lock.user_pin — переходный период, имени не знает
    char legacy[16];
    cfgGetStr("lock.user_pin", legacy, sizeof(legacy), "");
    if (legacy[0] != '\0' && strcmp(got, legacy) == 0) {
        auth.noteSuccess("sl_web");
        static const SlUser guest = { "", "Гость",
            (uint8_t)KeyType::PERMANENT, 0, 0, "" };
        return &guest;
    }

    auth.noteFailure("sl_web");
    return nullptr;
}

// ============================================================================
// СЕССИИ ЖИЛЬЦА (RAM-токены, скользящее окно — зеркало политики админки ядра)
// ============================================================================
uint32_t SmartLockUi::userSessionMs() {
    return (uint32_t)cfgGetUInt("lock.user_session_min", 30) * 60000UL;
}

const char* SmartLockUi::issueUserToken(const char* name) {
    const uint32_t now = millis();
    ResidentSession* slot = &_usess[0];
    for (uint8_t i = 0; i < SL_UTOK_SLOTS; ++i) {
        if (_usess[i].token[0] == '\0' ||
            (int32_t)(_usess[i].expiresMs - now) <= 0) {
            slot = &_usess[i];
            break;
        }
        if ((int32_t)(_usess[i].expiresMs - slot->expiresMs) < 0) slot = &_usess[i];
    }
    // Уникальность в пределах таблицы (коллизия 1/2^32 — перегенерируем)
    for (uint8_t attempt = 0; attempt < 4; ++attempt) {
        snprintf(slot->token, sizeof(slot->token), "%08lx",
                 (unsigned long)esp_random());
        bool dup = false;
        for (uint8_t i = 0; i < SL_UTOK_SLOTS; ++i) {
            if (&_usess[i] != slot && _usess[i].token[0] != '\0' &&
                strcmp(_usess[i].token, slot->token) == 0) { dup = true; break; }
        }
        if (!dup) break;
    }
    snprintf(slot->name, sizeof(slot->name), "%s", name);   // '\0' гарантирован
    slot->expiresMs = now + userSessionMs();
    return slot->token;
}

bool SmartLockUi::userTokenValid(const char* tok, char* nameOut,
                                 size_t nameSize) {
    if (tok == nullptr || strlen(tok) != 8) return false;
    const uint32_t now = millis();
    for (uint8_t i = 0; i < SL_UTOK_SLOTS; ++i) {
        if (_usess[i].token[0] != '\0' &&
            strcmp(_usess[i].token, tok) == 0 &&
            (int32_t)(_usess[i].expiresMs - now) > 0) {
            _usess[i].expiresMs = now + userSessionMs();   // скольжение
            if (nameOut != nullptr) {
                snprintf(nameOut, nameSize, "%s", _usess[i].name);
            }
            return true;
        }
    }
    return false;
}

void SmartLockUi::dropUserToken(const char* tok) {
    if (tok == nullptr) return;
    for (uint8_t i = 0; i < SL_UTOK_SLOTS; ++i) {
        if (_usess[i].token[0] != '\0' &&
            strcmp(_usess[i].token, tok) == 0) {
            _usess[i].token[0] = '\0';
            return;
        }
    }
}

// ============================================================================
// ПУБЛИЧНЫЙ ФРАГМЕНТ (панель жильца; бюджет ~2 КБ)
// ============================================================================
size_t SmartLockUi::renderPublicHtml(char* buf, size_t bufSize) {
    // Компактный статус + вход в полную панель СКУД (/web/lock.html из
    // LittleFS — монолитный дизайн v2.5.0, API МикроОС 5.0).
    int n = snprintf(buf, bufSize,
        "<div id=slGreet style='text-align:center;font-size:1.15em;margin:6px'>—</div>"
        "<div style='text-align:center;margin:6px'>"
        "<span id=slDoor style='border:1px solid #666;border-radius:8px;padding:3px 10px;margin:2px'>Дверь: —</span> "
        "<span id=slMode style='border:1px solid #666;border-radius:8px;padding:3px 10px;margin:2px'>Режим: —</span></div>"
        "<div style='text-align:center;margin:10px'>"
        "<a href='/web/lock.html' style='display:inline-block;padding:16px 44px;border-radius:14px;"
        "background:#2e7d32;color:#fff;text-decoration:none;font-size:1.25em'>Панель управления</a></div>"
        "<script>"
        "fetch('/api/dev/lock/status').then(r=>r.json()).then(j=>{"
        "document.getElementById('slGreet').textContent=j.greet;"
        "document.getElementById('slDoor').textContent='Дверь: '+(j.door?'открыта':'закрыта');"
        "document.getElementById('slMode').textContent='Режим: '+j.mode;});"
        "</script>");
    return n > 0 ? (size_t)n : 0;
}

// ============================================================================
// ДИСПЕТЧЕР ЭНДПОИНТОВ
// ============================================================================
bool SmartLockUi::handleApi(const char* tail, const ShUiRequest& req,
                            char* buf, size_t size, int& status) {
    // --- Публичные ------------------------------------------------------
    if (strcmp(tail, "lock/status") == 0) return apiStatus(buf, size);
    if (strcmp(tail, "lock/open") == 0 && strcmp(req.method, "POST") == 0) {
        return apiOpen(req, buf, size, status);
    }
    // Вход жильца БЕЗ открытия замка: ПИН → имя + токен сессии (utok).
    // Вариант ?utok= — проверка/скользящее продление сессии (resume панели).
    if (strcmp(tail, "lock/user/auth") == 0 && strcmp(req.method, "POST") == 0) {
        const char* utok = req.getArg("utok");
        if (utok != nullptr && utok[0] != '\0') {
            char uname[65];
            if (userTokenValid(utok, uname, sizeof(uname))) {
                snprintf(buf, size,
                         "{\"ok\":1,\"role\":\"user\",\"name\":\"%s\"}", uname);
            } else {
                status = 401;
                snprintf(buf, size, "{\"err\":\"session\"}");
            }
            return true;
        }
        if (!cfgGetBool("lock.user_pin_enabled", true)) {
            snprintf(buf, size, "{\"ok\":1,\"role\":\"user\",\"name\":\"Гость\"}");
            return true;
        }
        const char* deny = nullptr;
        const SlUser* u = identifyByPin(req, &deny);
        if (u == nullptr) {
            status = (deny != nullptr) ? 403 : 401;
            snprintf(buf, size, "{\"err\":\"%s\"}",
                     deny != nullptr ? deny : "pin");
        }
        else snprintf(buf, size,
                      "{\"ok\":1,\"role\":\"user\",\"name\":\"%s\",\"utok\":\"%s\"}",
                      u->name, issueUserToken(u->name));
        return true;
    }
    // Отзыв сессии жильца (кнопка «Выйти из сессии» в панели)
    if (strcmp(tail, "lock/user/logout") == 0 && strcmp(req.method, "POST") == 0) {
        dropUserToken(req.getArg("utok"));
        snprintf(buf, size, "{\"ok\":1}");
        return true;
    }

    // --- Дальше — только админ -------------------------------------------
    if (strncmp(tail, "lock/", 5) == 0 && !isAdmin(req)) {
        status = 401;
        snprintf(buf, size, "{\"err\":\"admin_required\"}");
        return true;
    }

    if (strcmp(tail, "lock/mode") == 0 && strcmp(req.method, "POST") == 0)
        return apiSetMode(req, buf, size);
    if (strcmp(tail, "lock/read/start") == 0 && strcmp(req.method, "POST") == 0)
        return apiReadStart(buf, size);
    if (strcmp(tail, "lock/read/result") == 0)
        return apiReadResult(buf, size);
    if (strcmp(tail, "lock/keys") == 0)
        return apiKeysList(req, buf, size);
    if (strcmp(tail, "lock/keys/add") == 0 && strcmp(req.method, "POST") == 0)
        return apiKeysAdd(req, buf, size, status);
    if (strcmp(tail, "lock/keys/update") == 0 && strcmp(req.method, "POST") == 0)
        return apiKeysUpdate(req, buf, size, status);
    if (strcmp(tail, "lock/keys/remove") == 0 && strcmp(req.method, "POST") == 0)
        return apiKeysRemove(req, buf, size, status);
    if (strcmp(tail, "lock/keys/block") == 0 && strcmp(req.method, "POST") == 0)
        return apiKeysBlock(req, buf, size, status);
    if (strcmp(tail, "lock/keys/clear") == 0 && strcmp(req.method, "POST") == 0)
        return apiKeysClear(req, buf, size, status);
    if (strcmp(tail, "lock/master/delete") == 0 && strcmp(req.method, "POST") == 0)
        return apiMasterDelete(req, buf, size, status);
    if (strcmp(tail, "lock/db/backup") == 0 && strcmp(req.method, "POST") == 0)
        return apiDbBackup(buf, size, status);
    if (strcmp(tail, "lock/db/restore") == 0 && strcmp(req.method, "POST") == 0)
        return apiDbRestore(buf, size, status);
    if (strcmp(tail, "lock/audio/test") == 0 && strcmp(req.method, "POST") == 0)
        return apiAudioTest(req, buf, size);
    if (strcmp(tail, "lock/paz/reset") == 0 && strcmp(req.method, "POST") == 0)
        return apiPazReset(buf, size);
    if (strcmp(tail, "lock/dlog/channels") == 0)
        return apiDlogChannels(buf, size);
    if (strcmp(tail, "lock/dlog") == 0)
        return apiDlog(req, buf, size, status);
    return false;   // 404 ядра
}

// ============================================================================
// ПУБЛИЧНЫЕ
// ============================================================================
bool SmartLockUi::apiStatus(char* buf, size_t size) {
    SmartLockApp& app = SmartLockApp::getInstance();
    LockControl& lock = LockControl::getInstance();
    CardStore& db = CardStore::getInstance();

    // Канонические коды режима (API-контракт; русская метка — забота UI)
    const char* mode = app.getMode() == LockMode::NORMAL  ? "NORMAL"  :
                       app.getMode() == LockMode::ACCEPT  ? "ACCEPT"  :
                                                           "TRIGGER";
    const char* lockState = lock.isTriggerHold()  ? "trigger" :
                            lock.isRelayActive()  ? "unlocked" : "locked";

    // Приветствие по RTC устройства (не по часам браузера)
    const char* greet = "Здравствуйте";
    struct tm t;
    if (TimeService::getInstance().getLocalTime(t)) {
        uint8_t tr = sl_track::timeOfDayTrack((uint8_t)t.tm_hour);
        greet = tr == 1 ? "Доброе утро" : tr == 2 ? "Добрый день" :
                tr == 3 ? "Добрый вечер" : "Здравствуйте";
    }

    // ID мастер-карты (подвал монолита: «Мастер: AA22DD33»)
    char masterId[9] = "";
    for (uint8_t i = 0; i < db.count(); ++i) {
        const SlUser* u = db.at(i);
        if (u != nullptr && u->type == (uint8_t)KeyType::MASTER) {
            snprintf(masterId, sizeof(masterId), "%s", u->id);
            break;
        }
    }

    // Погода — блок в конце (кэш MQTT-подписки SmartLockApp)
    char wx[160] = "";
    if (app.weatherValid()) {
        snprintf(wx, sizeof(wx),
            ",\"wx\":1,\"w_temp\":%.1f,\"w_feel\":%.1f,\"w_text\":\"%s\"",
            (double)app.weatherTemp(), (double)app.weatherFeelsLike(),
            app.weatherState());
    }

    // 5.5.13: возраст зеркала БД (u=0/fw="" — старого формата, возраст
    // неизвестен): панель показывает перед восстановлением, сколько лет
    // тому, что собираешься накатить.
    uint32_t mu = 0; char mfw[16];
    db.nvsMirrorInfo(mu, mfw, sizeof(mfw));

    snprintf(buf, size,
        "{\"door\":%d,\"mode\":\"%s\",\"lock\":\"%s\","
        "\"door_alarm\":%d,\"forced\":%d,\"greet\":\"%s\",\"pin\":%d,"
        "\"local\":%d,\"last_key\":\"%s\",\"last_user\":\"%s\","
        "\"master\":%d,\"master_id\":\"%s\",\"cards\":%u,\"quiet\":%d,"
        "\"db_empty\":%d,\"nvs_bak\":%d,\"mirror_u\":%lu,\"mirror_fw\":\"%s\","
        "\"exit\":%d,\"mqtt\":%d,\"cycles\":%lu,\"open_ms\":%lu,"
        "\"uptime\":%lu,\"unix\":%lu,\"version\":\"%s\",\"wread\":%d%s}",
        lock.isDoorOpen() ? 1 : 0, mode, lockState,
        app.isDoorAlarm() ? 1 : 0, app.isForcedAlarm() ? 1 : 0, greet,
        cfgGetBool("lock.user_pin_enabled", true) ? 1 : 0,
        lock.isLocalJumperSet() ? 1 : 0,
        app.getLastCard(), app.getLastUser(),
        db.masterExists() ? 1 : 0, masterId, db.count(),
        cfgGetBool("lock.quiet_mode", false) ? 1 : 0,
        db.count() == 0 ? 1 : 0, db.hasNvsBackup() ? 1 : 0,
        (unsigned long)mu, mfw,
        app.isExitAllowedNow() ? 1 : 0,
        MqttTransport::getInstance().isConnected() ? 1 : 0,
        (unsigned long)cfgGetUInt("lock.cycle_count", 0),
        (unsigned long)cfgGetUInt("lock.open_ms", 3000),
        (unsigned long)(millis() / 1000),
        // unix=0 — время недостоверно: панель честно покажет это в подвале
        // (урок 5.0.x: часы из браузера выглядели как «время синхронно»)
        (unsigned long)TimeService::getInstance().getUnixTime(),
        // Версия — из Version.h, а НЕ литералом: урок 5.0.15 (подвал панели
        // «застыл на 5.0.0» — литерал забыли обновить при релизах)
        MICROOS_VERSION,
        // wread — в конец и отдельным аргументом: урок 5.0.x о съехавших
        // на позицию аргументах apiStatus (фантомная модалка восстановления)
        app.isWebReadActive() ? 1 : 0, wx);
    return true;
}

bool SmartLockUi::apiOpen(const ShUiRequest& req, char* buf, size_t size,
                          int& status) {
    SmartLockApp& app = SmartLockApp::getInstance();
    if (isAdmin(req)) {                       // админ — без ПИНа
        app.remoteOpen(SlOpenSource::WEB, "Админ (веб)");
        snprintf(buf, size, "{\"ok\":1}");
        return true;
    }
    if (cfgGetBool("lock.user_pin_enabled", true)) {
        // Основной путь — токен сессии жильца (скользящее окно бездействия);
        // ?pin= — серверный фолбэк для прямых API-вызовов (панель ПИН
        // больше не хранит: компрометация sessionStorage ≠ открытая дверь).
        char uname[65];
        const char* utok = req.getArg("utok");
        if (utok != nullptr && utok[0] != '\0') {
            if (!userTokenValid(utok, uname, sizeof(uname))) {
                status = 401;
                snprintf(buf, size, "{\"err\":\"session\"}");
                return true;
            }
            app.remoteOpen(SlOpenSource::WEB, uname);   // персонифицировано
            snprintf(buf, size, "{\"ok\":1}");
            return true;
        }
        const char* deny = nullptr;
        const SlUser* u = identifyByPin(req, &deny);   // per-user ПИН → имя
        if (u == nullptr) {
            status = (deny != nullptr) ? 403 : 401;
            snprintf(buf, size, "{\"err\":\"%s\"}",
                     deny != nullptr ? deny : "pin");
            return true;
        }
        app.remoteOpen(SlOpenSource::WEB, u->name);   // персонифицировано
    } else {
        app.remoteOpen(SlOpenSource::WEB);
    }
    snprintf(buf, size, "{\"ok\":1}");
    return true;
}

// ============================================================================
// АДМИНСКИЕ
// ============================================================================
bool SmartLockUi::apiSetMode(const ShUiRequest& req, char* buf, size_t size) {
    const char* m = req.getArg("mode");
    if (!m || m[1] != '\0' || m[0] < '0' || m[0] > '2') {
        snprintf(buf, size, "{\"err\":\"mode 0..2\"}");
        return true;
    }
    SmartLockApp::getInstance().setMode((LockMode)(m[0] - '0'));
    snprintf(buf, size, "{\"ok\":1,\"mode\":%c}", m[0]);
    return true;
}

bool SmartLockUi::apiReadStart(char* buf, size_t size) {
    SmartLockApp::getInstance().startWebRead();
    snprintf(buf, size, "{\"ok\":1,\"timeout_s\":10}");
    return true;
}

bool SmartLockUi::apiReadResult(char* buf, size_t size) {
    SmartLockApp& app = SmartLockApp::getInstance();
    snprintf(buf, size, "{\"active\":%d,\"card\":\"%s\"}",
             app.isWebReadActive() ? 1 : 0, app.webReadCard());
    return true;
}

bool SmartLockUi::apiKeysList(const ShUiRequest& req, char* buf, size_t size) {
    CardStore& db = CardStore::getInstance();
    const char* fromArg = req.getArg("from");
    uint8_t from = fromArg ? (uint8_t)atoi(fromArg) : 0;
    constexpr uint8_t PAGE = 25;   // ~60 Б/запись — влезает в буфер ядра

    // 5.1.2: поиск q — регистронезависимая подстрока по имени/HEX
    // (кириллица UTF-8 — containsCI). Пагинация работает ПО ОТФИЛЬТРОВАННОМУ
    // списку: from — позиция среди совпавших, matched — их полное число.
    const char* q = req.getArg("q");
    if (q && !*q) q = nullptr;
    uint8_t matched = 0;
    if (q) {
        for (uint8_t i = 0; i < db.count(); ++i) {
            const SlUser* u = db.at(i);
            if (carddb::containsCI(u->name, q) ||
                carddb::containsCI(u->id, q)) ++matched;
        }
    } else {
        matched = db.count();
    }

    size_t pos = 0;
    int n = snprintf(buf, size, "{\"count\":%u,\"capacity\":%u,\"users\":[",
                     db.count(), db.capacity());
    if (n < 0) return true;
    pos = (size_t)n;

    uint8_t shown = 0;
    uint8_t idx = 0;   // позиция В ОТФИЛЬТРОВАННОМ списке
    for (uint8_t i = 0; i < db.count() && shown < PAGE; ++i) {
        const SlUser* u = db.at(i);
        if (q && !carddb::containsCI(u->name, q) &&
                 !carddb::containsCI(u->id, q)) continue;
        if (idx++ < from) continue;
        n = snprintf(buf + pos, size - pos,
            "%s{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"track\":%u,"
            "\"expiry\":%lu,\"has_pin\":%d,\"uses\":%lu,\"last_use\":%lu,"
            "\"blocked\":%d}", shown ? "," : "",
            u->id, u->name, carddb::typeStr(u->type),
            (unsigned)u->track, (unsigned long)u->expiry,
            u->pin[0] != '\0' ? 1 : 0,    // ПИН не отдаём — только факт
            (unsigned long)u->uses, (unsigned long)u->lastUse,
            u->blocked ? 1 : 0);
        if (n < 0 || (size_t)n >= size - pos) break;   // не влезло — страница короче
        pos += (size_t)n;
        ++shown;
    }
    snprintf(buf + pos, size - pos, "],\"from\":%u,\"shown\":%u,\"matched\":%u}",
             from, shown, matched);
    return true;
}

bool SmartLockUi::apiKeysAdd(const ShUiRequest& req, char* buf, size_t size,
                             int& status) {
    const char* id    = req.getArg("id");
    const char* name  = req.getArg("name");
    const char* type  = req.getArg("type");
    const char* track = req.getArg("track");
    const char* days  = req.getArg("expiry_days");
    if (!id || !type) {
        status = 400;
        snprintf(buf, size, "{\"err\":\"id,type required\"}");
        return true;
    }
    KeyType kt = (KeyType)carddb::typeFromStr(type);

    // 5.5.13: первая карта ПУСТОЙ базы — всегда МАСТЕР. Физический путь так
    // и живёт (handleCard: пустая база → первая поднесённая карта = мастер),
    // а веб-форма молча позволяла завести первую карту «постоянной» —
    // инцидент 12.08: оператор вручную восстанавливал базу и внёс мастера
    // типом permanent; система честно не видела мастера. Канал закрыт:
    // принуждаем, а панель докладывает явно (first_master).
    bool firstMaster = false;
    if (CardStore::getInstance().count() == 0 && kt != KeyType::MASTER) {
        kt = KeyType::MASTER;
        firstMaster = true;
    }

    // Личный веб-ПИН (опционально): строго 4..6 цифр и строго уникальный
    const char* pin = req.getArg("pin");
    char np[7] = "";
    if (pin != nullptr && pin[0] != '\0') {
        CardStore& db = CardStore::getInstance();
        if (!carddb::normalizePin(pin, np)) {
            status = 400;
            snprintf(buf, size, "{\"err\":\"pin_format\"}");   // 4..6 цифр
            return true;
        }
        if (db.pinTaken(np, nullptr)) {
            status = 409;
            snprintf(buf, size, "{\"err\":\"pin_not_unique\"}");
            return true;
        }
    }

    // Временный ключ: срок — в днях от текущего RTC (0/нет = бессрочно)
    uint32_t expiry = 0;
    if (kt == KeyType::TEMPORARY) {
        uint32_t d = days ? (uint32_t)atoi(days) : 0;
        time_t now = TimeService::getInstance().getUnixTime();
        if (d == 0 || now == 0) {
            status = 400;
            snprintf(buf, size, "{\"err\":\"expiry_days>0 + valid RTC\"}");
            return true;
        }
        expiry = (uint32_t)now + d * 86400UL;
    }

    // 5.1.1: РАЗЛИЧИМЫЕ ошибки. Раньше add() возвращал bool, и любой провал
    // отвечал размытым "full/dup/bad_id" — панель цеплялась за подстроку
    // «dup» и врала про дубликат, когда HEX был просто короткий (6 цифр
    // с W26-считывателя против требования ровно 8). Проверяем причины
    // по очереди — оператор должен видеть НАСТОЯЩУЮ.
    CardStore& db = CardStore::getInstance();
    char norm[9];
    if (!carddb::normalizeId(id, norm)) {
        status = 400;
        snprintf(buf, size, "{\"err\":\"bad_id\"}");   // 4..8 HEX-символов
        return true;
    }
    if (db.find(norm) != nullptr) {
        status = 409;
        snprintf(buf, size, "{\"err\":\"dup\"}");       // HEX уже в базе
        return true;
    }
    if (db.isFull()) {
        status = 400;
        snprintf(buf, size, "{\"err\":\"full\"}");      // база полна
        return true;
    }

    bool ok = db.add(id, name ? name : "Без имени", kt,
                     track ? (uint8_t)atoi(track) : 0,
                     expiry, np);
    if (!ok) status = 400;
    if (ok) {
        snprintf(buf, size, "{\"ok\":1%s}", firstMaster ? ",\"first_master\":1" : "");
    } else {
        snprintf(buf, size, "{\"err\":\"save_failed\"}");   // гонка/сбой FS — не маскируем
    }
    return true;
}

bool SmartLockUi::apiKeysUpdate(const ShUiRequest& req, char* buf, size_t size,
                                int& status) {
    const char* id = req.getArg("id");
    if (!id) { status = 400; snprintf(buf, size, "{\"err\":\"id\"}"); return true; }
    const char* name  = req.getArg("name");
    const char* track = req.getArg("track");
    const char* expiry = req.getArg("expiry");
    // Все поля опциональны: аргумент отсутствует — параметр НЕ меняется
    // (5.0.13: раньше name="" / track=0 / expiry=0 затирали запись).
    // track/expiry — строго цифры: мусор это 400, а не тихий сброс в 0.
    uint8_t tr = CardStore::TRACK_KEEP;
    if (track != nullptr) {
        bool digits = track[0] != '\0';
        for (const char* p = track; *p; ++p)
            if (*p < '0' || *p > '9') digits = false;
        if (!digits || atoi(track) > 99) {
            status = 400;
            snprintf(buf, size, "{\"err\":\"track_format\"}");
            return true;
        }
        tr = (uint8_t)atoi(track);
    }
    uint32_t exp = CardStore::EXPIRY_KEEP;
    if (expiry != nullptr) {
        bool digits = expiry[0] != '\0';
        for (const char* p = expiry; *p; ++p)
            if (*p < '0' || *p > '9') digits = false;
        if (!digits) {
            status = 400;
            snprintf(buf, size, "{\"err\":\"expiry_format\"}");
            return true;
        }
        exp = (uint32_t)strtoul(expiry, nullptr, 10);
    }
    // ПИН: аргумента нет — не менять; "" — снять веб-доступ; 4..6 цифр —
    // задать (уникальность проверяем СРАЗУ, исключая самого себя)
    const char* pin = req.getArg("pin");
    if (pin != nullptr && pin[0] != '\0') {
        char np[7];
        CardStore& db = CardStore::getInstance();
        if (!carddb::normalizePin(pin, np)) {
            status = 400;
            snprintf(buf, size, "{\"err\":\"pin_format\"}");
            return true;
        }
        char norm[9];
        if (carddb::normalizeId(id, norm) && db.pinTaken(np, norm)) {
            status = 409;
            snprintf(buf, size, "{\"err\":\"pin_not_unique\"}");
            return true;
        }
    }
    bool ok = CardStore::getInstance().update(id, name, tr, exp, pin);
    if (!ok) status = 404;
    snprintf(buf, size, ok ? "{\"ok\":1}" : "{\"err\":\"not_found\"}");
    return true;
}

bool SmartLockUi::apiKeysRemove(const ShUiRequest& req, char* buf, size_t size,
                                int& status) {
    const char* id = req.getArg("id");
    bool ok = id && CardStore::getInstance().remove(id);
    if (!ok) status = 404;
    snprintf(buf, size, ok ? "{\"ok\":1}" : "{\"err\":\"not_found\"}");
    return true;
}

bool SmartLockUi::apiKeysBlock(const ShUiRequest& req, char* buf, size_t size,
                               int& status) {
    const char* id = req.getArg("id");
    const char* b  = req.getArg("blocked");
    if (!id || !b || (b[0] != '0' && b[0] != '1')) {
        status = 400;
        snprintf(buf, size, "{\"err\":\"id,blocked=1|0 required\"}");
        return true;
    }
    bool blocked = (b[0] == '1');
    bool ok = CardStore::getInstance().setBlocked(id, blocked);
    if (!ok) status = 404;
    snprintf(buf, size, ok ? "{\"ok\":1}" : "{\"err\":\"not_found\"}");
    return true;
}

bool SmartLockUi::apiKeysClear(const ShUiRequest& req, char* buf, size_t size,
                               int& status) {
    const char* c = req.getArg("confirm");
    if (!c || strcmp(c, "DELETE") != 0) {
        status = 400;
        snprintf(buf, size, "{\"err\":\"confirm=DELETE\"}");
        return true;
    }
    CardStore::getInstance().clear();
    snprintf(buf, size, "{\"ok\":1}");
    return true;
}

bool SmartLockUi::apiMasterDelete(const ShUiRequest& req, char* buf,
                                  size_t size, int& status) {
    const char* c = req.getArg("confirm");
    if (!c || strcmp(c, "DELETE") != 0) {
        status = 400;
        snprintf(buf, size, "{\"err\":\"confirm=DELETE\"}");
        return true;
    }
    CardStore& db = CardStore::getInstance();
    for (uint8_t i = 0; i < db.count(); ++i) {
        const SlUser* u = db.at(i);
        if (u->type == (uint8_t)KeyType::MASTER) {
            char id[9];
            strncpy(id, u->id, sizeof(id) - 1);
            id[8] = '\0';
            bool ok = db.remove(id);
            if (!ok) status = 500;
            snprintf(buf, size, ok ? "{\"ok\":1}" : "{\"err\":\"fs\"}");
            return true;
        }
    }
    status = 404;
    snprintf(buf, size, "{\"err\":\"no_master\"}");
    return true;
}

bool SmartLockUi::apiDbBackup(char* buf, size_t size, int& status) {
    CardStore& db = CardStore::getInstance();
    bool ok = db.nvsBackupNow();
    if (!ok) { status = 500; snprintf(buf, size, "{\"err\":\"nvs\"}"); return true; }
    // 5.2.0: крупная база зеркалу не по силам (NVS 20 КБ) — это политика,
    // а не отказ: кнопка честно пишет FS-бэкап и докладывает режим панели.
    if (db.mirrorSkipped()) {
        bool fsOk = db.backupNow();
        snprintf(buf, size, "{\"ok\":1,\"mirror\":\"skipped\",\"fs\":%d}",
                 fsOk ? 1 : 0);
        return true;
    }
    snprintf(buf, size, "{\"ok\":1,\"mirror\":\"sealed\"}");
    return true;
}

bool SmartLockUi::apiDbRestore(char* buf, size_t size, int& status) {
    CardStore& db = CardStore::getInstance();
    // 5.5.13: возраст зеркала — в ответ, панель покажет ЧТО восстановила
    uint32_t mu = 0; char mfw[16];
    db.nvsMirrorInfo(mu, mfw, sizeof(mfw));
    bool ok = db.nvsRestoreNow();
    if (!ok) { status = 500;
        snprintf(buf, size, "{\"err\":\"nvs_empty/corrupt\"}"); return true; }
    snprintf(buf, size, "{\"ok\":1,\"mirror_u\":%lu,\"mirror_fw\":\"%s\"}",
             (unsigned long)mu, mfw);
    return true;
}

bool SmartLockUi::apiAudioTest(const ShUiRequest& req, char* buf, size_t size) {
    // Кнопка ▶ из формы ключа монолита: прослушать трек имени (папка /02)
    const char* t = req.getArg("track");
    uint8_t track = t ? (uint8_t)atoi(t) : 0;
    if (track < 1 || track > 99) {
        snprintf(buf, size, "{\"err\":\"track 1..99\"}");
        return true;
    }
    bool ok = AudioService::getInstance().sayRaw(
        sl_track::FOLDER_NAMES, track, (uint8_t)SndPriority::Normal);
    snprintf(buf, size, "{\"ok\":%d}", ok ? 1 : 0);
    return true;
}

bool SmartLockUi::apiPazReset(char* buf, size_t size) {
    // Монолит: «Сбросить флаг ПАЗ» (после проверки реле оператором)
    SmartLockApp::getInstance().resetAlarms();
    snprintf(buf, size, "{\"ok\":1}");
    return true;
}

// ============================================================================
// ДАТАЛОГГЕР (п.5: графики uPlot)
// ============================================================================
// Рабочие буферы чтения — BSS файла, НЕ стек (HTTP-задача 8 КБ стека:
// сотни записей на стеке = гарантированная паника). UNION: сырые точки и
// агрегаты в одном запросе не встречаются — платим один раз (постмортем:
// два раздельных буфера +11.9 КБ BSS уронили линковку DRAM-региона).
// Ярус читается хвостом до 320 записей — децимация 320 -> 240 форму
// графика сохраняет, а буфер вдвое меньше полного месячного файла.
constexpr uint16_t SL_DLOG_AGGR_READ = 320;
union DlogQueryBuf {
    DlogPoint raw[DLOG_RAW_CAP];
    DlogAggr  aggr[SL_DLOG_AGGR_READ];
};
static DlogQueryBuf s_dlogQ;

bool SmartLockUi::apiDlogChannels(char* buf, size_t size) {
    DataLogService& dl = DataLogService::getInstance();
    size_t pos = 0;
    int n = snprintf(buf, size, "{\"channels\":[");
    if (n < 0) return true;
    pos = (size_t)n;
    for (uint8_t i = 0; i < dl.channelCount(); ++i) {
        char id[12], name[28], unit[8];
        if (!dl.channelInfo(i, id, sizeof(id), name, sizeof(name),
                            unit, sizeof(unit))) continue;
        n = snprintf(buf + pos, size - pos,
            "%s{\"i\":%u,\"id\":\"%s\",\"name\":\"%s\",\"unit\":\"%s\"}",
            i ? "," : "", i, id, name, unit);
        if (n < 0 || (size_t)n >= size - pos) break;
        pos += (size_t)n;
    }
    snprintf(buf + pos, size - pos, "]}");
    return true;
}

bool SmartLockUi::apiDlog(const ShUiRequest& req, char* buf, size_t size,
                          int& status) {
    DataLogService& dl = DataLogService::getInstance();
    const char* chArg = req.getArg("ch");
    uint8_t ch = chArg ? (uint8_t)atoi(chArg) : 0;
    if (ch >= dl.channelCount()) {
        status = 404;
        snprintf(buf, size, "{\"err\":\"no_channel\"}");
        return true;
    }

    // Диапазон -> ярус и фильтр времени. 6ч — сырые точки (RAM);
    // 24ч/7д/30д — часовые агрегаты; год — суточные.
    const char* range = req.getArg("range");
    uint32_t now = (uint32_t)TimeService::getInstance().getUnixTime();
    uint32_t fromTs = 0;
    bool raw = true, daily = false;
    if (range == nullptr || strcmp(range, "6h") == 0) {
        fromTs = now > 6UL * 3600 ? now - 6UL * 3600 : 0;
    } else if (strcmp(range, "24h") == 0) {
        raw = false; fromTs = now > 24UL * 3600 ? now - 24UL * 3600 : 0;
    } else if (strcmp(range, "7d") == 0) {
        raw = false; fromTs = now > 7UL * 86400 ? now - 7UL * 86400 : 0;
    } else if (strcmp(range, "30d") == 0) {
        raw = false; fromTs = 0;              // ярус и есть ~31 сутки
    } else if (strcmp(range, "1y") == 0) {
        raw = false; daily = true; fromTs = 0;
    } else {
        status = 400;
        snprintf(buf, size, "{\"err\":\"range: 6h|24h|7d|30d|1y\"}");
        return true;
    }

    size_t pos = 0;
    int n;
    if (raw) {
        uint16_t cnt = dl.getRaw(ch, s_dlogQ.raw, DLOG_RAW_CAP, fromTs);
        cnt = dlog::decimateRaw(s_dlogQ.raw, cnt, s_dlogQ.raw,
                                DLOG_JSON_POINTS);
        n = snprintf(buf, size, "{\"fmt\":\"raw\",\"n\":%u,\"ts\":[", cnt);
        if (n < 0) return true;
        pos = (size_t)n;
        for (uint16_t i = 0; i < cnt; ++i) {
            n = snprintf(buf + pos, size - pos, "%s%lu", i ? "," : "",
                         (unsigned long)s_dlogQ.raw[i].ts);
            if (n < 0 || (size_t)n >= size - pos) { pos = size - 8; break; }
            pos += (size_t)n;
        }
        n = snprintf(buf + pos, size - pos, "],\"v\":[");
        if (n > 0) pos += (size_t)n;
        for (uint16_t i = 0; i < cnt; ++i) {
            n = snprintf(buf + pos, size - pos, "%s%.1f", i ? "," : "",
                         (double)s_dlogQ.raw[i].v);
            if (n < 0 || (size_t)n >= size - pos) { pos = size - 8; break; }
            pos += (size_t)n;
        }
        snprintf(buf + pos, size - pos, "]}");
        return true;
    }

    uint16_t cnt = dl.getTier(ch, daily, s_dlogQ.aggr, SL_DLOG_AGGR_READ,
                              fromTs);
    cnt = dlog::decimateAggr(s_dlogQ.aggr, cnt, s_dlogQ.aggr,
                             DLOG_JSON_POINTS);
    n = snprintf(buf, size, "{\"fmt\":\"aggr\",\"n\":%u,\"ts\":[", cnt);
    if (n < 0) return true;
    pos = (size_t)n;
    for (uint16_t i = 0; i < cnt; ++i) {
        n = snprintf(buf + pos, size - pos, "%s%lu", i ? "," : "",
                     (unsigned long)s_dlogQ.aggr[i].ts);
        if (n < 0 || (size_t)n >= size - pos) { pos = size - 16; break; }
        pos += (size_t)n;
    }
    static const char* KEYS[3] = { "\"avg\":[", "\"min\":[", "\"max\":[" };
    for (uint8_t k = 0; k < 3; ++k) {
        n = snprintf(buf + pos, size - pos, "],%s", KEYS[k]);
        if (n > 0) pos += (size_t)n;
        for (uint16_t i = 0; i < cnt; ++i) {
            float v = k == 0 ? s_dlogQ.aggr[i].avg :
                      k == 1 ? s_dlogQ.aggr[i].mn  : s_dlogQ.aggr[i].mx;
            n = snprintf(buf + pos, size - pos, "%s%.1f", i ? "," : "",
                         (double)v);
            if (n < 0 || (size_t)n >= size - pos) { pos = size - 16; break; }
            pos += (size_t)n;
        }
    }
    snprintf(buf + pos, size - pos, "]}");
    return true;
}
