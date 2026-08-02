// ============================================================================
// AudioService.cpp — реализация звукового диспетчера ядра
// ============================================================================
#include "AudioService.h"
#include "ConfigService.h"
#include "TimeService.h"
#include "../core/Events.h"
#include "../core/DriverRegistry.h"
#include "../drivers/DfPlayerDriver.h"

AudioService& AudioService::getInstance() {
    static AudioService instance;
    return instance;
}

// ============================================================================
// КОНФИГ-СХЕМА (авто-UI админки, группа «Звук»)
// ============================================================================
void AudioService::registerExtensions() {
    ConfigService::getInstance().addFields("Звук", {
        { "audio.enabled",    ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Звук", "Звуковая подсистема включена" },
        { "audio.volume",     ConfigType::UINT, "18", 0, 30, CFG_NONE,
          "Звук", "Громкость днём (0–30)" },
        { "audio.volume_night", ConfigType::UINT, "10", 0, 30, CFG_NONE,
          "Звук", "Громкость в тихие часы (0–30)" },
        { "audio.quiet_start", ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Звук", "Тихие часы от (ЧЧ:ММ, пусто — выкл)" },
        { "audio.quiet_end",  ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Звук", "Тихие часы до (ЧЧ:ММ)" },
        { "audio.repeat_window_s", ConfigType::UINT, "3", 0, 60, CFG_NONE,
          "Звук", "Анти-флуд: окно подавления повтора фразы, с" },
        { "audio.use_advert", ConfigType::BOOL, "false", 0, 0, CFG_CRITICAL,
          "Звук", "Аппаратный ADVERT (только genuine DFPlayer!)" },
    });
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void AudioService::init() {
    _initialized = true;
    auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>("dfplayer");
    log(LogLevel::Info, "init: %s, pack=%s",
        (df && df->isConfigured()) ? "hardware ready" : "NO hardware",
        _pack ? _pack->packName() : "none");
}

void AudioService::start() {
    EventBus& bus = EventBus::getInstance();
    bus.subscribe(DRV_EVENT_AUDIO_STARTED, this);
    bus.subscribe(DRV_EVENT_AUDIO_FINISHED, this);
    bus.subscribe(DRV_EVENT_AUDIO_OFFLINE, this);
    bus.subscribe(SH_EVENT_SAFE_MODE_ENTERED, this);
    _started = true;

    // Стандартный слот ядра: приветствие после загрузки (если пак его
    // определил — иначе молчим, это не ошибка).
    say("sys.boot");
}

void AudioService::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    _started = false;
}

bool AudioService::canHandleEvent(int32_t eventId) const {
    return eventId == DRV_EVENT_AUDIO_STARTED ||
           eventId == DRV_EVENT_AUDIO_FINISHED ||
           eventId == DRV_EVENT_AUDIO_OFFLINE ||
           eventId == SH_EVENT_SAFE_MODE_ENTERED;
}

// ============================================================================
// API ПРОФИЛЕЙ
// ============================================================================
bool AudioService::say(const char* phraseName) {
    if (!cfgGetBool("audio.enabled", true)) return false;
    auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>("dfplayer");
    if (df == nullptr || !df->isConfigured()) return false;
    if (_pack == nullptr) return false;

    const SoundPhrase* ph = _pack->find(phraseName);
    if (ph == nullptr) {
        // Неизвестная фраза — один раз в лог, без падения
        log(LogLevel::Warning, "phrase '%s' not in pack", phraseName);
        return false;
    }

    // Анти-флуд: тот же текст в окне — тишина (кроме Alarm: сирене можно)
    uint32_t windowMs = cfgGetUInt("audio.repeat_window_s",
                                   SND_DEFAULT_REPEAT_WINDOW_S) * 1000UL;
    if (ph->priority < (uint8_t)SndPriority::Alarm &&
        strcmp(_lastName, ph->name) == 0 &&
        snd::isRepeat(millis(), _lastPlayedMs, windowMs)) {
        return false;
    }

    return enqueuePhrase(ph);
}

bool AudioService::sayRaw(uint8_t folder, uint8_t track, uint8_t priority) {
    if (!cfgGetBool("audio.enabled", true)) return false;
    auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>("dfplayer");
    if (df == nullptr || !df->isConfigured()) return false;

    // Синтетическое имя — для анти-флуда, событий и логов (копируется
    // в SndItem внутри enqueuePhrase, локальный буфер безопасен).
    char name[SND_NAME_LEN];
    snprintf(name, sizeof(name), "raw.%u.%u", folder, track);
    SoundPhrase ph{ name, folder, track, priority, 0 };

    uint32_t windowMs = cfgGetUInt("audio.repeat_window_s",
                                   SND_DEFAULT_REPEAT_WINDOW_S) * 1000UL;
    if (priority < (uint8_t)SndPriority::Alarm &&
        strcmp(_lastName, name) == 0 &&
        snd::isRepeat(millis(), _lastPlayedMs, windowMs)) {
        return false;
    }
    return enqueuePhrase(&ph);
}

bool AudioService::alarmOn(const char* phraseName) {
    if (_alarmActive && strcmp(_current.name, phraseName) == 0) {
        return true;   // та же сирена уже звучит — идемпотентно
    }
    bool ok = say(phraseName);
    if (ok) {
        _alarmActive = true;
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), phraseName);
        postEvent(SND_EVENT_ALARM_ON, &d);
    }
    return ok;
}

void AudioService::alarmOff() {
    if (!_alarmActive) return;
    _alarmActive = false;
    auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>("dfplayer");
    if (df && df->isConfigured()) df->stop();
    _playing = false;
    // Если сирена ещё ждала в очереди (не дошла до воспроизведения) —
    // выбрасываем её, чтобы не взвыла ПОСЛЕ снятия тревоги.
    SndItem queued;
    SndQueue kept;
    while (_queue.pop(queued)) {
        if (!(queued.flags & SND_FLAG_LOOP)) kept.enqueue(queued);
    }
    _queue = kept;

    ShEventData d; d.clear();
    safeStrCopy(d.payload, sizeof(d.payload), _current.name);
    postEvent(SND_EVENT_ALARM_OFF, &d);
    tryStartNext();   // жизнь после сирены продолжается
}

// ============================================================================
// ОЧЕРЕДЬ + ВЫТЕСНЕНИЕ
// ============================================================================
bool AudioService::enqueuePhrase(const SoundPhrase* ph) {
    // Тихие часы: играет только Alarm (сирена не спрашивает расписания)
    if (isQuietHours() && ph->priority < (uint8_t)SndPriority::Alarm) {
        dropWithEvent(ph->name);
        return false;
    }

    SndItem item;
    safeStrCopy(item.name, sizeof(item.name), ph->name);
    item.folder   = ph->folder;
    item.track    = ph->track;
    item.priority = ph->priority;
    item.flags    = ph->flags;
    item.enqueuedMs = millis();

    // ADVERT-семантика: спец-фраза прерывает текущую, та продолжится после.
    // Два пути (конфиг audio.use_advert):
    //   false (умолчание, клон MP3-TF-16P): stop текущей -> playAdvert ->
    //     прерванная возвращается программно по FINISHED (SND_EVENT_RESUMED);
    //   true (genuine DFPlayer): playAdvert на ходу — железо само прерывает
    //     и само возвращается; программного resume нет (иначе двойное).
    if ((item.flags & SND_FLAG_ADVERT) && _playing &&
        !(item.flags & SND_FLAG_LOOP)) {
        bool hwAdvert = cfgGetBool("audio.use_advert", false);
        auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>(
            "dfplayer");
        if (!hwAdvert) {
            if (!_hasInterrupted) {
                _interrupted = _current;
                _hasInterrupted = true;
            }
            df->stop();
        }
        _current = item;
        _playing = true;
        // folder у ADVERT-фразы не используется: папка /ADVERT адресуется
        // самой командой 0x13, track — индекс файла внутри неё.
        df->playAdvert(item.track);
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), item.name);
        postEvent(SND_EVENT_STARTED, &d);
        return true;
    }

    // Вытеснение по приоритету: новый строго выше текущего
    if (_playing && snd::shouldPreempt(item.priority, _current.priority)) {
        _playing = false;   // FINISHED от stop() не придёт — снимаем сами
        auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>(
            "dfplayer");
        df->stop();
        _current = item;
        _playing = true;
        df->playFolder(item.folder, item.track);
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), item.name);
        postEvent(SND_EVENT_STARTED, &d);
        return true;
    }

    if (!_queue.enqueue(item)) {
        dropWithEvent(item.name);
        return false;
    }
    if (!_playing) tryStartNext();
    return true;
}

void AudioService::tryStartNext() {
    if (_playing) return;
    auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>("dfplayer");
    if (df == nullptr || !df->isConfigured()) return;   // очередь подождёт

    SndItem next;
    if (!_queue.pop(next)) return;

    applyVolume();
    _current = next;
    _playing = true;
    // ADVERT-фраза и вне прерывания играется из папки /ADVERT — иначе
    // folder=0 у таких фраз ломал бы playFolder.
    if (next.flags & SND_FLAG_ADVERT) {
        df->playAdvert(next.track);
    } else {
        df->playFolder(next.folder, next.track);
    }

    ShEventData d; d.clear();
    safeStrCopy(d.payload, sizeof(d.payload), next.name);
    postEvent(SND_EVENT_STARTED, &d);
}

// ============================================================================
// СОБЫТИЯ ШИНЫ (автомат воспроизведения)
// ============================================================================
void AudioService::onEvent(int32_t eventId, const ShEventData* /*data*/) {
    if (eventId == DRV_EVENT_AUDIO_OFFLINE) {
        // Плеер умер: очередь бессмысленна, честно фиксируем потери
        _dropped += _queue.count();
        _queue.clear();
        _playing = false;
        publishError("AUDIO_OFFLINE");
        return;
    }

    if (eventId == SH_EVENT_SAFE_MODE_ENTERED) {
        say("sys.safe_mode");   // слот ядра, если пак определил
        return;
    }

    if (eventId == DRV_EVENT_AUDIO_STARTED) {
        return;   // информационное; текущее состояние у нас и так есть
    }

    if (eventId == DRV_EVENT_AUDIO_FINISHED) {
        if (!_playing) return;   // дубликат/не наше

        // Метрики и анти-флуд по завершении (фраза реально прозвучала)
        _played++;
        safeStrCopy(_lastName, sizeof(_lastName), _current.name);
        _lastPlayedMs = millis();

        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), _current.name);
        postEvent(SND_EVENT_FINISHED, &d);

        // Зацикленная сирена: клон не умеет честный loop через ACK —
        // подаём трек заново, пока alarmOff() не снимет.
        if (_alarmActive && (_current.flags & SND_FLAG_LOOP)) {
            auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>(
                "dfplayer");
            if (df && df->isConfigured()) {
                df->playFolder(_current.folder, _current.track);
                return;   // _playing остаётся — круг продолжается
            }
        }

        _playing = false;

        // Software-resume после ADVERT: прерванная продолжается первой
        if (_hasInterrupted) {
            _hasInterrupted = false;
            _queue.pushFront(_interrupted);
            ShEventData r; r.clear();
            safeStrCopy(r.payload, sizeof(r.payload), _interrupted.name);
            postEvent(SND_EVENT_RESUMED, &r);
        }
        tryStartNext();
    }
}

// ============================================================================
// TICK: периодика (громкость тихих часов применяется на лету)
// ============================================================================
void AudioService::tick() {
    applyVolume();
}

// ============================================================================
// ТИХИЕ ЧАСЫ И ГРОМКОСТЬ
// ============================================================================
bool AudioService::isQuietHours() const {
    char start[CFG_VALUE_LEN], end[CFG_VALUE_LEN];
    cfgGetStr("audio.quiet_start", start, sizeof(start), "");
    cfgGetStr("audio.quiet_end", end, sizeof(end), "");
    if (start[0] == '\0' || end[0] == '\0') return false;
    // Недостоверное время -> НЕ тихие часы (Fail-Operational: лучше
    // лишний раз сказать, чем промолчать тревогу)
    return TimeService::getInstance().isTimeInInterval(start, end);
}

void AudioService::applyVolume() {
    uint8_t want = (uint8_t)cfgGetUInt(
        isQuietHours() ? "audio.volume_night" : "audio.volume", 18);
    if (want == _appliedVolume) return;
    auto* df = DriverRegistry::getInstance().findAs<DfPlayerDriver>("dfplayer");
    if (df == nullptr || !df->isConfigured()) return;
    df->setVolume(want);
    _appliedVolume = want;
}

void AudioService::dropWithEvent(const char* name) {
    _dropped++;
    ShEventData d; d.clear();
    safeStrCopy(d.payload, sizeof(d.payload), name);
    postEvent(SND_EVENT_DROPPED, &d);
}
