// ============================================================================
// AudioService.h — ЗВУКОВОЙ ДИСПЕТЧЕР ЯДРА (ex-AudioManager v4.2.2)
// ============================================================================
// Фаза 3 (порция «голос экосистемы»). Единственный владелец «рта»
// устройства: арбитраж доступа к DFPlayer поверх DfPlayerDriver.
//
// Границы ответственности:
//   DfPlayerDriver (ядро, готов) — UART-кадры, клон-компат, автомат BUSY;
//   AudioService   (ядро, это)   — очередь, приоритеты, вытеснение, тихие
//                                  часы, анти-флуд, ADVERT-семантика,
//                                  стандартные слоты ядра, конфиг, события;
//   ISoundPack     (профиль)     — таблица фраз (данные!);
//   Профильные модули            — триггеры: say("access.granted").
//
// ADVERT (папка /ADVERT на SD): аппаратное «прервать и продолжить» на
// клоне MP3-TF-16P ненадёжно (возврат к прерванному через раз — наш опыт).
// Поэтому по умолчанию семантика ADVERT эмулируется ПРОГРАММНО: спец-фраза
// вытесняет текущую, а прерванная возвращается в голову очереди и
// продолжается после (SND_EVENT_RESUMED). Аппаратный путь — конфигом
// audio.use_advert=true (только для genuineDFPlayer()).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "AudioQueue.h"
#include "ISoundPack.h"

// Бюджеты
constexpr uint32_t SND_DEFAULT_REPEAT_WINDOW_S = 3;   // анти-флуд, сек

class AudioService : public ModuleBase {
public:
    static AudioService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "AudioService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x000D; }

    void registerExtensions() override;   // схема audio.*
    void init() override;
    void start() override;                // подписки + "sys.boot"
    void stop() override;
    void tick() override;                 // автомат очереди
    uint32_t getTickIntervalMs() const override { return 50; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- ИНЖЕКЦИЯ ЗВУКОВОГО НАПОЛНЕНИЯ (профиль, из registerExtensions) ---
    void setSoundPack(ISoundPack* pack) { _pack = pack; }

    // --- API ПРОФИЛЕЙ --------------------------------------------------------
    /// Произнести фразу по семантическому имени из SoundPack.
    /// false — звука нет/выключен/фраза не найдена/подавлена анти-флудом.
    bool say(const char* phraseName);
    /// Произнести трек по ПРЯМОМУ адресу на SD — для динамического контента,
    /// которого нет в SoundPack (имя жильца из базы СКУД, цифры температуры
    /// у климат-контроллера). Та же политика, что у say(): enabled, анти-флуд,
    /// тихие часы, приоритет. SoundPack не требуется (адрес самодостаточен).
    bool sayRaw(uint8_t folder, uint8_t track, uint8_t priority);
    /// Зацикленная сирена (фраза с SND_FLAG_LOOP), повторный вызов
    /// той же — идемпотентен.
    bool alarmOn(const char* phraseName);
    /// Снять сирену (любую активную). Идемпотентно.
    void alarmOff();
    /// Сирена звучит?
    bool isAlarmActive() const { return _alarmActive; }

    // --- МЕТРИКИ (для телеметрии/диагностики) --------------------------------
    uint32_t playedCount() const { return _played; }
    uint32_t droppedCount() const { return _dropped; }

private:
    AudioService() = default;

    // --- ВНУТРЕННЯЯ КУХНЯ ----------------------------------------------------
    bool enqueuePhrase(const SoundPhrase* ph);
    void tryStartNext();                  // взять следующую из очереди
    void applyVolume();                   // дневная/ночная (тихие часы)
    bool isQuietHours() const;            // audio.quiet_start/end
    void dropWithEvent(const char* name); // SND_EVENT_DROPPED + счётчик

    // --- ДАННЫЕ ----------------------------------------------------------------
    ISoundPack* _pack = nullptr;
    SndQueue  _queue;

    SndItem  _current = {};               // что играет сейчас
    bool     _playing  = false;           // ждём FINISHED от драйвера
    bool     _alarmActive = false;        // зацикленная сирена
    SndItem  _interrupted = {};           // прерванная (software ADVERT)
    bool     _hasInterrupted = false;

    // Анти-флуд: последняя сыгранная фраза
    char     _lastName[SND_NAME_LEN] = "";
    uint32_t _lastPlayedMs = 0;

    uint32_t _played  = 0;
    uint32_t _dropped = 0;
    uint8_t  _appliedVolume = 0xFF;       // чтобы не слать volume зря
};
