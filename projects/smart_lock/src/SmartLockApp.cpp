// ============================================================================
// SmartLockApp.cpp — политика доступа СКУД (сценарии монолита v2.5.0, ч. 6)
// ============================================================================
#include "SmartLockApp.h"
#include "SmartLockProfile.h"
#include "SmartLockConfig.h"
#include "SmartLockSoundPack.h"
#include "SmartLockTracks.h"
#include "CardStore.h"
#include "LockControl.h"
#include "SmartLockUi.h"
#include "SmartLockHealthChecks.h"
#include <core/ConformanceTest.h>
#include <services/AudioService.h>
#include <services/HttpService.h>
#include <services/ConfigService.h>
#include <services/TimeService.h>
#include <services/MqttTransport.h>
#include <services/UpdateService.h>
#include <services/NetworkManager.h>
#include <services/TelemetryService.h>
#include <services/DataLogService.h>
#include <services/StorageService.h>   // персистентный кэш погоды
#include <core/Events.h>
#include <core/EventBus.h>

// Хелпер HA-состояний: retained-публикация с учётом выключателя
// mqtt.ha_discovery (единая точка политики «объявлять/молчать»).
namespace {
void haPub(const char* suffix, const char* value) {
    if (!cfgGetBool("mqtt.ha_discovery", true)) return;
    MqttTransport::getInstance().publishStateSuffix(suffix, value, true);
}
}

SmartLockApp& SmartLockApp::getInstance() {
    static SmartLockApp instance;
    return instance;
}

// ============================================================================
// РАСШИРЕНИЯ (точки инжекции профиля)
// ============================================================================
void SmartLockApp::registerExtensions() {
    registerSmartLockConfig();                              // поля lock.*

    // 5.8.0, аккордеон «Служебные» (решение владельца 13.08.2026): группы
    // «Планировщик» и «Счётчики» — сироты на этом профиле (ни одного
    // SCHED_EVENT, CounterService не используется). Поля и сервисы живут
    // полной программой — уходят лишь в свёрнутый блок панели.
    ConfigService::getInstance().setHiddenGroups("Планировщик,Счётчики");

    static SmartLockSoundPack pack;                         // озвучка как данные
    AudioService::getInstance().setSoundPack(&pack);

    static SmartLockUi ui;                                  // веб-лицо устройства
    HttpService::getInstance().setUiProvider(&ui);

    registerSmartLockHealthChecks();                        // доменный ПАЗ
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void SmartLockApp::init() {
    _mode = LockMode::NORMAL;
    if (LockControl::getInstance().isLocalJumperSet()) {
        log(LogLevel::Warning,
            "джампер GPIO35: строгий локальный режим (сеть игнорируется)");
    }
    _initialized = true;
    log(LogLevel::Info, "access policy ready, cards=%u",
        CardStore::getInstance().count());
}

void SmartLockApp::start() {
    EventBus& bus = EventBus::getInstance();
    // Входные факты
    bus.subscribe(DRV_EVENT_WIEGAND_CARD, this);
    bus.subscribe(DRV_EVENT_WIEGAND_NOISE, this);
    bus.subscribe(sl_ev::exitButton(), this);
    bus.subscribe(sl_ev::doorOpen(), this);
    bus.subscribe(sl_ev::doorClosed(), this);
    bus.subscribe(ACCESS_EVENT_UNLOCKED, this);    // окно «ожидаемого» открытия
    // Статусы инфраструктуры — озвучка (монолит: NET_OK/MQTT_OK/OTA)
    bus.subscribe(NET_EVENT_IP_CHANGED, this);
    bus.subscribe(SH_EVENT_MQTT_CONNECTED, this);
    bus.subscribe(SH_EVENT_MQTT_MESSAGE, this);    // E2: команда от УД-хаба
    bus.subscribe(ACCESS_EVENT_LOCKED, this);      // E2: HA-состояние замка
    // E2: HA discovery — «open» от брокера -> remoteOpen (проброс ядра)
    MqttTransport::getInstance().setCmdHandler(&SmartLockApp::onMqttCmd);
    bus.subscribe(OTA_EVENT_STARTED, this);
    bus.subscribe(OTA_EVENT_SUCCESS, this);
    bus.subscribe(OTA_EVENT_FAILED, this);
    _started = true;
    log(LogLevel::Info, "SmartLockApp started, mode=NORMAL");

    // Погода от УД: HA (или своя метеостанция) публикует JSON в топик
    // lock.weather_topic. Пустой топик = карточка погоды выключена.
    loadWeatherCache();   // мгновенная карточка после ребута (кэш 5.0.x)
    char wxTopic[CFG_VALUE_LEN];
    cfgGetStr("lock.weather_topic", wxTopic, sizeof(wxTopic), "");
    if (wxTopic[0] != '\0') {
        MqttTransport::getInstance().subscribeExternal(
            wxTopic, &SmartLockApp::onWeatherMqtt);
    }

    // Даталоггер (п.5): каналы профиля. Сервис пассивен — кто и что пишет,
    // решает профиль (завтра контроллер света объявит lux/power по той же
    // механике — «по наследству» уйдёт только сервис, не данные).
    DataLogService& dlog = DataLogService::getInstance();
    _chCpu  = dlog.registerChannel("cpu_t", "Температура CPU", "°C");
    _chHeap = dlog.registerChannel("heap",  "Свободная память", "КБ");
    _chWx   = dlog.registerChannel("wx_t",  "Температура на улице", "°C");

    // D1: стенд соответствия платформе. Боевой профиль обязан проходить
    // ЧИСТО (в отличие от TestProfile с его умышленным конфликтом).
    SmartLockProfile self;
    HardwareManifest m;
    self.describeHardware(m);
    conformance::runAll(self.profileId(), m);
}

void SmartLockApp::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    _started = false;
}

bool SmartLockApp::canHandleEvent(int32_t id) const {
    return id == DRV_EVENT_WIEGAND_CARD || id == DRV_EVENT_WIEGAND_NOISE ||
           id == sl_ev::exitButton() || id == sl_ev::doorOpen() ||
           id == sl_ev::doorClosed() || id == ACCESS_EVENT_UNLOCKED ||
           id == ACCESS_EVENT_LOCKED ||
           id == NET_EVENT_IP_CHANGED || id == SH_EVENT_MQTT_CONNECTED ||
           id == SH_EVENT_MQTT_MESSAGE ||
           id == OTA_EVENT_STARTED || id == OTA_EVENT_SUCCESS ||
           id == OTA_EVENT_FAILED;
}

// ============================================================================
// TICK: таймеры (тревога двери, веб-чтение)
// ============================================================================
void SmartLockApp::tick() {
    // --- Тревога «дверь открыта долго» (строго 1 срабатывание) -------------
    LockControl& lock = LockControl::getInstance();
    if (lock.isDoorOpen() && !_doorAlarm) {
        uint32_t limitMin = cfgGetUInt("lock.door_alarm_min", 1);   // 1–60
        uint32_t since = lock.doorOpenSinceMs();
        if (since > 0 && millis() - since > limitMin * 60000UL) {
            _doorAlarm = true;
            postEvent(sl_ev::doorAlarm(), nullptr);
            haPub("ha/alarm", "alarm");
            // ADVERT-фраза: в белом списке тихого режима (критическая)
            AudioService::getInstance().say("sl.door.timeout");
            log(LogLevel::Warning, "DOOR ALARM: open > %lu min",
                (unsigned long)limitMin);
        }
    }

    // --- Веб-чтение карты (авто-закрытие через 10 с, фикс монолита) --------
    if (_webReadActive && millis() - _webReadStart > WEB_READ_TIMEOUT_MS) {
        _webReadActive = false;
        log(LogLevel::Info, "web read timeout");
    }

    // --- Даталоггер: сэмпл системных метрик раз в 60 с ----------------------
    // Источник — TelemetryService.snapshot(): метрики не дублируются
    // (принцип «всё из существующих владельцев»).
    if (millis() - _lastDlogMs >= DLOG_SAMPLE_MS) {
        _lastDlogMs = millis();
        const TelemetrySnapshot& s = TelemetryService::getInstance().snapshot();
        DataLogService& dlog = DataLogService::getInstance();
        if (_chCpu >= 0)  dlog.logPoint(_chCpu,  (float)s.cpuTenths / 10.0f);
        if (_chHeap >= 0) dlog.logPoint(_chHeap, (float)s.heapFree / 1024.0f);
    }

    // --- Авто-чистка просроченных временных ключей (раз в сутки) -----------
    // Решение владельца 03.09.2026: временный ключ живёт до конца срока;
    // на следующие сутки его нет в базе (исключаем «забыли удалить»).
    // Только при ДОСТОВЕРНОМ времени — Fail-Safe: недостоверное = не чистим.
    if (cfgGetBool("lock.purge_expired", true) &&
        millis() - _lastPurgeCheckMs >= PURGE_CHECK_MS) {
        _lastPurgeCheckMs = millis();
        struct tm t;
        if (TimeService::getInstance().getLocalTime(t)) {
            int day = t.tm_year * 400 + t.tm_yday;   // штамп суток
            if (day != _purgeDay) {
                _purgeDay = day;
                uint8_t n = CardStore::getInstance().purgeExpired(
                    (uint32_t)TimeService::getInstance().getUnixTime());
                if (n > 0) {
                    log(LogLevel::Info,
                        "purge: просроченных временных ключей удалено: %u", n);
                }
            }
        }
    }
}

// ============================================================================
// СОБЫТИЯ
// ============================================================================
void SmartLockApp::onEvent(int32_t eventId, const ShEventData* data) {
    // --- Карта от считывателя -------------------------------------------------
    if (eventId == DRV_EVENT_WIEGAND_CARD && data) {
        if (_webReadActive) {
            // Веб-режим перехватывает карту (монолит: web_read_mode)
            _webReadActive = false;
            safeStrCopy(_webReadCard, sizeof(_webReadCard), data->payload);
            ShEventData d; d.clear();
            safeStrCopy(d.payload, sizeof(d.payload), data->payload);
            postEvent(sl_ev::webReadCard(), &d);
            AudioService::getInstance().say("sl.beep");
            return;
        }
        handleCard(data->payload);
        return;
    }
    if (eventId == DRV_EVENT_WIEGAND_NOISE) {
        log(LogLevel::Warning, "wiegand noise, bits=%ld",
            data ? (long)data->code : 0L);
        return;
    }

    // --- Факты исполнителя -------------------------------------------------------
    if (eventId == sl_ev::exitButton()) { handleExitButton(); return; }
    if (eventId == sl_ev::doorOpen()) {
        handleDoorOpen();
        haPub("ha/door", "open");
        if (_forcedAlarm) haPub("ha/alarm", "alarm");
        return;
    }
    if (eventId == sl_ev::doorClosed()) {
        handleDoorClosed();
        haPub("ha/door", "closed");
        haPub("ha/alarm", (_doorAlarm || _forcedAlarm) ? "alarm" : "ok");
        return;
    }
    if (eventId == ACCESS_EVENT_UNLOCKED) {
        _lastUnlockMs = millis();
        haPub("ha/lock", "unlocked");
        return;
    }
    if (eventId == ACCESS_EVENT_LOCKED) { haPub("ha/lock", "locked"); return; }

    // --- Инфраструктура: озвучка статусов (монолит, ч. 3) -----------------------
    if (eventId == NET_EVENT_IP_CHANGED) {
        AudioService::getInstance().say("sl.net.ok");
        return;
    }
    if (eventId == SH_EVENT_MQTT_CONNECTED) {
        AudioService::getInstance().say("sl.mqtt.ok");
        // E2: устройство объявляет себя само (конфиги retained — брокер
        // раздаст их Home Assistant даже после рестарта любого из нас)
        publishHaDiscovery();
        publishHaStates();
        return;
    }
    if (eventId == SH_EVENT_MQTT_MESSAGE && data) {
        // E2: payload "originId/NAME|body"; УД-хаб шлёт событие SL_OPEN
        if (strstr(data->payload, "/SL_OPEN") != nullptr) {
            log(LogLevel::Info, "remote open via MQTT (%s)", data->payload);
            remoteOpen(SlOpenSource::MQTT);
        }
        return;
    }
    if (eventId == OTA_EVENT_STARTED) {
        AudioService::getInstance().say("sl.ota.start");   return;
    }
    if (eventId == OTA_EVENT_SUCCESS) {
        AudioService::getInstance().say("sl.ota.success"); return;
    }
    if (eventId == OTA_EVENT_FAILED) {
        AudioService::getInstance().say("sl.ota.failed");  return;
    }
}

// ============================================================================
// ГЛАВНАЯ МАШИНА ДОСТУПА (монолит: обработка card_available)
// ============================================================================
void SmartLockApp::handleCard(const char* cardHex) {
    CardStore& db = CardStore::getInstance();
    safeStrCopy(_lastCard, sizeof(_lastCard), cardHex);

    const SlUser* u = db.find(cardHex);
    safeStrCopy(_lastUser, sizeof(_lastUser), u ? u->name : "");

    // 1. Пустая база → первая карта становится мастером (монолит, ч. 6)
    if (db.count() == 0) {
        if (db.add(cardHex, "Администратор СКУД", KeyType::MASTER, 0, 0)) {
            postEvent(sl_ev::masterLearned(), nullptr);
            AudioService::getInstance().say("sl.key.added");
            log(LogLevel::Info, "MASTER learned on empty db: %s", cardHex);
        }
        return;
    }

    // 2. Заблокированная карта — жёсткий отказ ДО любой семантики (даже
    //    мастер-ключ: потерянный мастер не должен крутить режимы).
    //    Идея AccessManager.blockUser мёртвой ветки: гасим без удаления —
    //    имя/трек/статистика сохраняются для разбора полётов.
    if (u && u->blocked) {
        denyAccess(cardHex, SlDenyReason::BLOCKED_CARD, "sl.access.denied");
        return;
    }

    // 3. Мастер-ключ → цикл режимов NORMAL→ACCEPT→TRIGGER→NORMAL
    if (u && u->type == (uint8_t)KeyType::MASTER) {
        handleMasterCard();
        return;
    }

    // 4. Временный ключ: срок истёк ИЛИ недостоверное время (Fail-Secure
    //    для TEMPORARY: проверить срок невозможно — не пускать, в лог).
    if (u && u->type == (uint8_t)KeyType::TEMPORARY && u->expiry > 0) {
        time_t now = TimeService::getInstance().getUnixTime();
        if (now == 0 || (uint32_t)now > u->expiry) {
            log(LogLevel::Warning, "temporary key %s: %s", cardHex,
                now == 0 ? "time invalid — Fail-Secure deny" : "expired");
            denyAccess(cardHex, SlDenyReason::EXPIRED, "sl.access.denied");
            return;
        }
    }

    // 5. Режим ACCEPT — авто-запись новой карты (монолит, ч. 6)
    if (_mode == LockMode::ACCEPT) {
        if (!u) {
            if (!db.add(cardHex, "Новый жилец", KeyType::PERMANENT, 0, 0)) {
                AudioService::getInstance().say("sl.memory.full");
                denyAccess(cardHex, SlDenyReason::UNKNOWN_CARD,
                           "sl.access.denied");
                return;
            }
            AudioService::getInstance().say("sl.key.added");
            u = db.find(cardHex);
        } else {
            AudioService::getInstance().say("sl.key.exists");
        }
        grantAccess(cardHex, u);
        return;
    }

    // 6. Обычный проход
    if (!u) {
        denyAccess(cardHex, SlDenyReason::UNKNOWN_CARD, "sl.access.denied");
        return;
    }
    if (u->type == (uint8_t)KeyType::ONETIME) {
        // Одноразовый — сгорает. Данные копируем ДО remove (указатель
        // станет невалидным после сдвига массива).
        SlUser copy = *u;
        db.remove(cardHex);
        log(LogLevel::Info, "one-time key %s burned", cardHex);
        grantAccess(cardHex, &copy);
        return;
    }
    grantAccess(cardHex, u);
}

void SmartLockApp::handleMasterCard() {
    switch (_mode) {
        case LockMode::NORMAL:  setMode(LockMode::ACCEPT);  break;
        case LockMode::ACCEPT:  setMode(LockMode::TRIGGER); break;
        case LockMode::TRIGGER: setMode(LockMode::NORMAL);  break;
    }
}

// ============================================================================
// КНОПКА ВЫХОДА (расписание + Fail-Safe при мёртвом RTC — монолит)
// ============================================================================
void SmartLockApp::handleExitButton() {
    // Политика — isExitAllowedNow(); здесь только журналируем Fail-Safe
    if (cfgGetBool("lock.exit_restrict", false) &&
        cfgGetBool("lock.exit_restrict_active", false) &&
        !TimeService::getInstance().isTimeValid()) {
        log(LogLevel::Warning, "exit restrict: RTC dead — Fail-Safe allow");
    }
    bool allowed = isExitAllowedNow();

    // NB: sl_ev::exitButton() НЕ переиздаём — это входное событие, а мы на
    // него подписаны: переиздание = рекурсия. Вердикт выражают события
    // ACCESS_EVENT_UNLOCKED (от LockControl) / ACCESS_EVENT_DENIED.
    if (allowed) {
        LockControl::getInstance().openPulse(SlOpenSource::BUTTON, "EXIT_BTN");
        sayFiltered("sl.beep", sl_track::BEEP);
    } else {
        denyAccess("EXIT_BTN", SlDenyReason::SCHEDULE_BLOCK, "sl.blocked.admin");
    }
}

// ============================================================================
// ДВЕРЬ
// ============================================================================
void SmartLockApp::handleDoorOpen() {
    // Взлом: дверь открылась, а разблокировки не было (окно = импульс +
    // запас на физику 3 с). TRIGGER-удержание — легальное открытое состояние.
    uint32_t openMs = cfgGetUInt("lock.open_ms", 3000);
    bool expected = (millis() - _lastUnlockMs) < (openMs + 3000UL);
    if (!expected && !LockControl::getInstance().isTriggerHold()) {
        _forcedAlarm = true;
        postEvent(sl_ev::forcedEntry(), nullptr);
        AudioService& audio = AudioService::getInstance();
        audio.say("sl.alarm.forced");      // ADVERT, Alarm — прерывает всё
        audio.alarmOn("sys.alarm");        // зацикленная сирена ПАЗ
        log(LogLevel::Critical, "FORCED ENTRY ALARM");
    }
}

void SmartLockApp::handleDoorClosed() {
    if (_doorAlarm) {
        _doorAlarm = false;
        postEvent(sl_ev::doorAlarmOff(), nullptr);
    }
    if (_forcedAlarm) {
        _forcedAlarm = false;
        AudioService::getInstance().alarmOff();
        log(LogLevel::Info, "forced alarm cleared (door closed)");
    }
}

// ============================================================================
// ВЕРДИКТЫ
// ============================================================================
void SmartLockApp::grantAccess(const char* card, const SlUser* user) {
    LockControl::getInstance().openPulse(SlOpenSource::CARD, card);
    if (user) safeStrCopy(_lastUser, sizeof(_lastUser), user->name);
    playGreeting(user);

    // Статистика прохода (5.0.x). Единая воронка: сюда стекаются NORMAL,
    // ACCEPT (новая карта — первый проход сразу учтён) и сгоревший one-time
    // (его запись уже удалена — recordUse мягко вернёт false, не считаем).
    CardStore::getInstance().recordUse(
        card, (uint32_t)TimeService::getInstance().getUnixTime());

    ShEventData d; d.clear();
    safeStrCopy(d.payload, sizeof(d.payload), card);
    postEvent(ACCESS_EVENT_GRANTED, &d);   // -> аудит B3 + MQTT-зеркало E2
}

void SmartLockApp::denyAccess(const char* card, SlDenyReason reason,
                              const char* phrase) {
    // Отказ — вердикт безопасности: звучит даже в тихом режиме
    AudioService::getInstance().say(phrase);

    ShEventData d; d.clear();
    d.code = (int32_t)reason;
    safeStrCopy(d.payload, sizeof(d.payload), card);
    postEvent(ACCESS_EVENT_DENIED, &d);    // -> аудит B3 + MQTT-зеркало E2
    log(LogLevel::Info, "DENIED %s reason=%d", card, (int)reason);
}

// ============================================================================
// ОЗВУЧКА
// ============================================================================
bool SmartLockApp::sayFiltered(const char* phrase, uint8_t actionTrack) {
    // Тихий режим монолита: звучат только критические тревоги
    if (cfgGetBool("lock.quiet_mode", false) &&
        !sl_track::allowedInQuietMode(actionTrack)) {
        return false;
    }
    return AudioService::getInstance().say(phrase);
}

void SmartLockApp::playGreeting(const SlUser* user) {
    AudioService& audio = AudioService::getInstance();

    // Тихий режим: только вердикт (монолит: quiet → TRACK_ACCESS_GRANTED)
    if (cfgGetBool("lock.quiet_mode", false) || !user || user->track == 0) {
        audio.say("sl.access.granted");
        return;
    }
    // Цепочка монолита: время суток → имя → «доступ разрешён»
    struct tm t;
    uint8_t hour = 12;
    if (TimeService::getInstance().getLocalTime(t)) hour = (uint8_t)t.tm_hour;
    static const char* GREET[4] = {
        "sl.greet.morning", "sl.greet.day", "sl.greet.evening", "sl.greet.night"
    };
    uint8_t g = sl_track::timeOfDayTrack(hour);   // 1..4
    audio.say(GREET[g - 1]);
    // Имя жильца — динамический контент: прямой адрес (папка 02)
    audio.sayRaw(sl_track::FOLDER_NAMES, user->track, (uint8_t)SndPriority::Ambient);
    audio.say("sl.access.granted");
}

// ============================================================================
// КОМАНДЫ ИЗВНЕ (API / MQTT / мастер-ключ)
// ============================================================================
void SmartLockApp::remoteOpen(SlOpenSource source, const char* userName) {
    if (userName != nullptr && userName[0] != '\0') {
        safeStrCopy(_lastUser, sizeof(_lastUser), userName);
    }
    LockControl::getInstance().openPulse(source, "REMOTE");
    sayFiltered("sl.remote.open", sl_track::REMOTE_OPEN);
}

void SmartLockApp::logWebDeny(const char* userName, const char* reason) const {
    log(LogLevel::Warning, "web: доступ '%s' запрещён (%s, зеркало карты)",
        userName != nullptr ? userName : "?",
        reason != nullptr ? reason : "?");
}

bool SmartLockApp::isExitAllowedNow() const {
    if (!cfgGetBool("lock.exit_restrict", false) ||
        !cfgGetBool("lock.exit_restrict_active", false)) {
        return true;
    }
    TimeService& ts = TimeService::getInstance();
    if (!ts.isTimeValid()) return true;        // FAIL-SAFE: RTC мёртв
    char st[8], en[8];
    cfgGetStr("lock.exit_restrict_start", st, sizeof(st), "22:00");
    cfgGetStr("lock.exit_restrict_end",   en, sizeof(en), "06:00");
    return !ts.isTimeInInterval(st, en);
}

void SmartLockApp::resetAlarms() {
    bool was = _forcedAlarm || _doorAlarm;
    _forcedAlarm = false;
    _doorAlarm   = false;
    AudioService::getInstance().alarmOff();   // сирену — выкл, не ждём дверь
    if (was) log(LogLevel::Info, "PAZ flag reset by operator (web)");
}

// ============================================================================
// ПОГОДА ОТ УД (внешняя MQTT-подписка)
// ============================================================================
void SmartLockApp::onWeatherMqtt(const char* topic, const char* payload) {
    (void)topic;   // топик один — дискриминация не нужна
    getInstance().parseWeather(payload);
}

void SmartLockApp::parseWeather(const char* payload) {
    // Мини-JSON без библиотек: "temp":<float>, ощущается — "feels_like" или
    // краткий алиас "feel" (трафик дороже байта: монолит гонял короткие
    // ключи), состояние — "state" или алиас "text" (коды HA: rainy, sunny...
    // — перевод в русский делает ПАНЕЛЬ, карта weatherMap из монолита).
    // Незнакомые ключи игнорируем; битое значение — поле просто не
    // обновится (кэш сохраняет последнее хорошее).
    if (payload == nullptr) return;
    const char* t = strstr(payload, "\"temp\"");
    if (t != nullptr) {
        t = strchr(t, ':');
        if (t != nullptr) {
            float v = strtof(t + 1, nullptr);
            if (v > -80.0f && v < 80.0f) {
                _wxTemp = v;
                // Даталоггер: уличная температура — точка на каждое
                // сообщение (обычно 1/15 мин; ярусы сожмут в час/сутки)
                if (_chWx >= 0) DataLogService::getInstance().logPoint(_chWx, v);
            }
        }
    }
    // NB: "\"feel\"" с закрывающей кавычкой — подстроки в "\"feels_like\"" нет
    const char* f = strstr(payload, "\"feels_like\"");
    if (f == nullptr) f = strstr(payload, "\"feel\"");
    if (f != nullptr) {
        f = strchr(f, ':');
        if (f != nullptr) {
            float v = strtof(f + 1, nullptr);
            if (v > -80.0f && v < 80.0f) _wxFeel = v;
        }
    }
    const char* s = strstr(payload, "\"state\"");
    if (s == nullptr) s = strstr(payload, "\"text\"");
    if (s != nullptr) {
        s = strchr(s, ':');
        if (s != nullptr) {
            while (*s && *s != '"') ++s;
            if (*s == '"') {
                ++s;
                size_t i = 0;
                while (*s && *s != '"' && i < sizeof(_wxState) - 1) {
                    _wxState[i++] = *s++;
                }
                _wxState[i] = '\0';
            }
        }
    }
    _wxValid = true;
    saveWeatherCache();   // пережить ребут (см. шапку .h)
}

// Персистентный кэш погоды: компактный JSON, те же ключи, что у брокера
// (переиспользуем parseWeather при загрузке — один парсер на оба пути).
void SmartLockApp::saveWeatherCache() {
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"temp\":%.1f,\"feel\":%.1f,\"text\":\"%s\"}",
                     (double)_wxTemp, (double)_wxFeel, _wxState);
    if (n <= 0) return;
    if (!StorageService::getInstance().atomicWrite("/wx_cache.json", buf)) {
        log(LogLevel::Warning, "weather cache save failed");
    }
}

void SmartLockApp::loadWeatherCache() {
    uint8_t buf[96];
    size_t n = StorageService::getInstance().readFile("/wx_cache.json", buf,
                                                      sizeof(buf) - 1);
    if (n == 0) return;   // кэша нет — карточка честно ждёт брокера
    buf[n] = '\0';
    parseWeather((const char*)buf);   // тот же парсер, но это ещё и запись…
    // NB: parseWeather сам дёрнет saveWeatherCache — холостой rewrite того
    // же файла раз за загрузку; износом пренебрегаем (1 запись/ребут).
    if (_wxValid) log(LogLevel::Info, "weather restored from cache");
}

void SmartLockApp::setMode(LockMode mode) {
    if (_mode == mode) return;
    _mode = mode;

    const char* phrase = "sl.mode.normal";
    if (mode == LockMode::ACCEPT)  phrase = "sl.mode.accept";
    if (mode == LockMode::TRIGGER) phrase = "sl.mode.trigger";
    sayFiltered(phrase, mode == LockMode::ACCEPT  ? sl_track::MODE_ACCEPT  :
                        mode == LockMode::TRIGGER ? sl_track::MODE_TRIGGER :
                                                    sl_track::MODE_NORMAL);

    // TRIGGER — замок удерживается открытым (монолит, ч. 6)
    LockControl::getInstance().setTriggerHold(mode == LockMode::TRIGGER);

    ShEventData d; d.clear();
    d.code = (int32_t)mode;
    postEvent(sl_ev::modeChanged(), &d);
    log(LogLevel::Info, "mode -> %d", (int)mode);
}

void SmartLockApp::startWebRead() {
    _webReadActive = true;
    _webReadStart = millis();
    _webReadCard[0] = '\0';
}

// ============================================================================
// HOME ASSISTANT DISCOVERY (E2: устройство объявляет себя само)
// ============================================================================
// Модель сущностей — консервативная: действие «открыть» — кнопкой (импульс),
// состояние замка — read-only сенсором. НАМЕРЕННО не mqtt-lock: его действие
// «lock» присло бы LOCK в наш cmd/open и импульсно ОТКРЫЛО дверь —
// ловушка безопасности, замеченная при проектировании.
// Конфиги retained + availability на ядерном LWT (<prefix>/<id>/state):
// HA видит устройство offline при внезапной смерти контроллера.
// ============================================================================
void SmartLockApp::publishHaDiscovery() {
    if (!cfgGetBool("mqtt.ha_discovery", true)) return;
    MqttTransport& mqtt = MqttTransport::getInstance();

    const char* id = NetworkService::getInstance().deviceId();
    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");

    // Общий хвост: availability + карточка устройства.
    // sw — из UpdateService (там core/Version.h): карточка устройства в HA
    // показывает РЕАЛЬНУЮ версию прошивки (урок 5.0.10).
    char dev[288];
    snprintf(dev, sizeof(dev),
        ",\"avty_t\":\"%s/%s/state\",\"pl_avail\":\"online\","
        "\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"%s\"],"
        "\"name\":\"%s\",\"mf\":\"MicroOS\",\"mdl\":\"smart_lock\","
        "\"sw\":\"%s\"}",
        prefix, id, id, NetworkService::getInstance().hostname(),
        UpdateService::getInstance().firmwareVersion());

    char topic[MQTT_TOPIC_LEN];
    char cfg[768];

    // --- Кнопка «Открыть замок» ---------------------------------------------
    snprintf(topic, sizeof(topic), "homeassistant/button/%s_open/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Открыть замок\",\"uniq_id\":\"%s_open\","
        "\"cmd_t\":\"%s/%s/cmd/open\"%s}", id, prefix, id, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Состояние замка (read-only) -----------------------------------------
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/%s_lock/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Замок\",\"uniq_id\":\"%s_lock\",\"dev_cla\":\"lock\","
        "\"stat_t\":\"%s/%s/ha/lock\",\"pl_on\":\"unlocked\","
        "\"pl_off\":\"locked\"%s}", id, prefix, id, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Дверь -----------------------------------------------------------------
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/%s_door/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Дверь\",\"uniq_id\":\"%s_door\",\"dev_cla\":\"door\","
        "\"stat_t\":\"%s/%s/ha/door\",\"pl_on\":\"open\","
        "\"pl_off\":\"closed\"%s}", id, prefix, id, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Тревога (долго открыта / взлом) ---------------------------------------
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/%s_alarm/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Тревога\",\"uniq_id\":\"%s_alarm\",\"dev_cla\":\"problem\","
        "\"stat_t\":\"%s/%s/ha/alarm\",\"pl_on\":\"alarm\","
        "\"pl_off\":\"ok\"%s}", id, prefix, id, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Телеметрия: аптайм / память / температура CPU (из JSON-снимка) -------
    char tel[MQTT_TOPIC_LEN];
    snprintf(tel, sizeof(tel), "%s/%s/telemetry", prefix, id);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_uptime/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Аптайм\",\"uniq_id\":\"%s_uptime\",\"dev_cla\":\"duration\","
        "\"unit_of_meas\":\"s\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.uptime }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_heap/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Свободная память\",\"uniq_id\":\"%s_heap\","
        "\"unit_of_meas\":\"B\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.heap }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_temp/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Температура CPU\",\"uniq_id\":\"%s_temp\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.cpu_t }}\"%s}", id, tel, dev);
    mqtt.publishRaw(topic, cfg, true);

    // --- Перезагрузка ------------------------------------------------------------
    snprintf(topic, sizeof(topic), "homeassistant/button/%s_reboot/config", id);
    snprintf(cfg, sizeof(cfg),
        "{\"name\":\"Перезагрузка\",\"uniq_id\":\"%s_reboot\","
        "\"dev_cla\":\"restart\",\"cmd_t\":\"%s/%s/cmd/reboot\"%s}",
        id, prefix, id, dev);
    mqtt.publishRaw(topic, cfg, true);

    log(LogLevel::Info, "HA discovery: 8 entities announced");
}

void SmartLockApp::publishHaStates() {
    if (!cfgGetBool("mqtt.ha_discovery", true)) return;
    LockControl& lock = LockControl::getInstance();
    haPub("ha/door", lock.isDoorOpen() ? "open" : "closed");
    haPub("ha/lock",
          (lock.isRelayActive() || lock.isTriggerHold()) ? "unlocked" : "locked");
    haPub("ha/alarm", (_doorAlarm || _forcedAlarm) ? "alarm" : "ok");
}

bool SmartLockApp::onMqttCmd(const char* verb, const char* /*body*/) {
    if (strcmp(verb, "open") == 0) {
        getInstance().remoteOpen(SlOpenSource::MQTT);
        return true;
    }
    return false;
}
