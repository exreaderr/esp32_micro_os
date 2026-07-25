// ============================================================================
// AudioManager.h - ULTIMATE MICRO-OS V5.0 (ADAPTED)
// ============================================================================
// Описание: Управление аудио-воспроизведением через DFPlayer Mini.
//           Адаптирован для новой событийной архитектуры МикроОС v5.0.
//
// ИЗМЕНЕНИЯ v5.0:
// - Сохранена 100% функциональность v4.2.2
// - Добавлена константа EVENT_AUDIO_TRACK_START
// - Добавлена константа EVENT_AUDIO_TRACK_COMPLETE
// - Добавлен метод publishAudioEvent() для публикации через новую шину
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика
// ============================================================================
#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <vector>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

#include "core/IModule.h"
#include "core/ShEventData.h" // НОВОЕ: для констант событий

// ============================================================================
// 1. КОНСТАНТЫ DFPlayer (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
#define DFPLAYER_CMD_PLAY 0x03
#define DFPLAYER_CMD_PAUSE 0x0E
#define DFPLAYER_CMD_RESUME 0x0D
#define DFPLAYER_CMD_STOP 0x0C
#define DFPLAYER_CMD_VOLUME 0x06
#define DFPLAYER_CMD_EQ 0x07
#define DFPLAYER_CMD_PLAYBACK_STATE 0x42

#define SPECIAL_PAUSE_CODE 0xFFFF

// ============================================================================
// 2. СОБЫТИЯ AUDIO MANAGER (ВАШИ, РАСШИРЕНЫ)
// ============================================================================
enum AudioEvents : int32_t {
    SH_EVENT_AUDIO_TRACK_START = SH_EVENT_USER_BASE + 0x0300,
    SH_EVENT_AUDIO_TRACK_COMPLETE = SH_EVENT_USER_BASE + 0x0301,
    SH_EVENT_AUDIO_QUEUE_EMPTY = SH_EVENT_USER_BASE + 0x0302,
    SH_EVENT_AUDIO_ERROR = SH_EVENT_USER_BASE + 0x0303,
    SH_EVENT_AUDIO_STATE_CHANGED = SH_EVENT_USER_BASE + 0x0304,
    SH_EVENT_AUDIO_HEALTH_CHANGED = SH_EVENT_USER_BASE + 0x0305,
    SH_EVENT_AUDIO_VOLUME_CHANGED = SH_EVENT_USER_BASE + 0x0306,
    SH_EVENT_AUDIO_QUEUE_PROCESSED = SH_EVENT_USER_BASE + 0x0307
};

// ============================================================================
// 3. ТИПЫ И СТРУКТУРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
enum class AudioPriority : uint8_t {
    INFO = 0,
    WARNING = 1,
    CRITICAL = 2,
    PAUSE = 3,
    IMMEDIATE = 4
};

enum class AudioHealthState : uint8_t {
    OK = 0,
    NO_SD = 1,
    UART_TIMEOUT = 2,
    FILE_NOT_FOUND = 3,
    BUSY_STUCK = 4,
    INIT_FAILED = 5
};

enum class AudioPlayState : uint8_t {
    IDLE = 0,
    PLAYING = 1,
    PAUSED = 2,
    WAITING = 3,
    ERROR = 4
};

/**
 * @brief Структура задачи воспроизведения (ВАША)
 */
struct AudioTask {
    uint16_t trackId;
    AudioPriority priority;
    uint32_t timestamp;
    bool repeat;
    uint8_t repeatCount;
    bool isPhrase;
    uint16_t delayMs;
};

/**
 * @brief Конфигурация аудио (ВАША)
 */
struct AudioConfig {
    uint8_t baseVolume = 20;
    uint8_t nightVolume = 8;
    uint8_t emergencyVolume = 30;
    uint16_t phrasePauseMs = 250;
    uint16_t betweenTracksPauseMs = 35;
    bool nightModeAuto = true;
    bool quietMode = false;
    bool enableEQ = false;
    uint8_t eqPreset = 0;
    uint32_t wdtTimeoutMs = 15000;
    uint32_t maxQueueSize = 50;
};

/**
 * @brief Статистика аудио (ВАША)
 */
struct AudioStats {
    uint32_t totalTracksPlayed = 0;
    uint32_t totalTracksQueued = 0;
    uint32_t emergencyTracks = 0;
    uint32_t errors = 0;
    uint32_t wdtResets = 0;
    uint32_t lastTrackId = 0;
    uint32_t lastPlayTime = 0;
    uint32_t queueMaxSize = 0;
    uint32_t bufferOverflows = 0;
    uint32_t totalPhrases = 0;
    uint32_t skippedTracks = 0;
};

/**
 * @brief Событие трека (ВАША)
 */
struct AudioTrackEvent {
    uint16_t trackId;
    uint8_t priority;
    uint32_t timestamp;
    uint32_t duration;
    bool isEmergency;
};

/**
 * @brief Событие состояния (ВАША)
 */
struct AudioStateEvent {
    uint8_t playState;
    uint8_t healthState;
    uint16_t currentTrack;
    uint8_t volume;
    size_t queueSize;
};

// ============================================================================
// 4. ОСНОВНОЙ КЛАСС (РАСШИРЕН)
// ============================================================================
/**
 * @brief Менеджер аудио-воспроизведения через DFPlayer Mini
 *
 * Синглтон. Обеспечивает:
 * - Воспроизведение треков с приоритетами
 * - Очередь задач
 * - Фразы (последовательность треков)
 * - Ночной режим
 * - Автоматическое восстановление
 * - Полную потокобезопасность
 */
class AudioManager : public IModule {
public:
    // === КОЛБЭКИ (ВАШИ) ===
    typedef std::function<void(uint16_t trackId)> OnTrackStartCallback;
    typedef std::function<void(uint16_t trackId)> OnTrackCompleteCallback;
    typedef std::function<void(AudioHealthState state)> OnHealthChangeCallback;
    typedef std::function<void(AudioPlayState state)> OnStateChangeCallback;
    typedef std::function<void(const AudioStats& stats)> OnStatsUpdateCallback;
    typedef std::function<void(uint16_t trackId, bool success)> OnPlaybackCallback;

    // === СИНГЛТОН (ВАШ) ===
    static AudioManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ) ===
    AudioManager();
    ~AudioManager();

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // === IModule (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    const char* getName() const override { return "AudioManager"; }
    const char* getVersion() const override { return "5.0.0"; } // ИЗМЕНЕНО: версия
    uint32_t getModuleId() const override { return MODULE_ID_AUDIO; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _initialized && _healthState == AudioHealthState::OK; }

    // === СТАТУС (ВАШ, БЕЗ ИЗМЕНЕНИЙ) ===
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === КОНФИГУРАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    void begin(HardwareSerial& serial, uint8_t rxPin, uint8_t txPin,
              uint8_t busyPin, const AudioConfig& initialConfig);
    void end();
    void reset();

    // === ВОСПРОИЗВЕДЕНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    bool playTrack(uint16_t trackNumber, AudioPriority priority = AudioPriority::INFO,
                  bool repeat = false, uint8_t repeatCount = 1);
    bool playPhrase(const std::vector<uint16_t>& tracks,
                   AudioPriority priority = AudioPriority::INFO);
    bool playTrackEmergency(uint16_t trackNumber);

    // === УПРАВЛЕНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ) ===
    void pause();
    void resume();
    void clearQueue();
    void stopPlayback();

    // === НАСТРОЙКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setVolume(uint8_t volume);
    void setBaseVolume(uint8_t volume);
    void setNightVolume(uint8_t volume);
    void setNightMode(bool enable);
    void setNightModeAuto(bool enable) { _config.nightModeAuto = enable; }
    void setPhrasePause(uint16_t ms) { _config.phrasePauseMs = ms; }
    void setPauseBetweenTracks(uint16_t ms) { _config.betweenTracksPauseMs = ms; }
    void setMaxQueueSize(size_t maxSize) { _config.maxQueueSize = maxSize; }
    void setEQ(uint8_t preset);

    // === ГЕТТЕРЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    uint8_t getBaseVolume() const { return _config.baseVolume; }
    uint8_t getNightVolume() const { return _config.nightVolume; }
    bool isNightMode() const { return _isNightMode; }
    bool isQuietMode() const { return _config.quietMode; }
    bool isPlaying() const;
    bool isPaused() const { return _playState == AudioPlayState::PAUSED; }
    size_t getQueueSize() const;
    AudioHealthState getHealthState() const { return _healthState; }
    AudioPlayState getPlayState() const { return _playState; }
    AudioStats getStats() const { return _stats; }
    uint16_t getCurrentTrack() const { return _currentTrackId; }
    AudioConfig getConfig() const { return _config; }

    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void setOnTrackStart(OnTrackStartCallback cb) { _onTrackStart = cb; }
    void setOnTrackComplete(OnTrackCompleteCallback cb) { _onTrackComplete = cb; }
    void setOnHealthChange(OnHealthChangeCallback cb) { _onHealthChange = cb; }
    void setOnStateChange(OnStateChangeCallback cb) { _onStateChange = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }
    void setOnPlayback(OnPlaybackCallback cb) { _onPlayback = cb; }

    // === НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЯ ЧЕРЕЗ НОВУЮ ШИНУ ===
    /**
     * @brief Публикует событие о воспроизведении трека через новую событийную шину
     * @param trackId ID трека
     * @param isStart true если трек начался, false если завершился
     */
    void publishAudioEvent(uint16_t trackId, bool isStart);

    // === ДИАГНОСТИКА (ВАША, РАСШИРЕНА) ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

private:
    // === ВНУТРЕННИЕ МЕТОДЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    void sendCommand(uint8_t cmd, uint16_t arg = 0);
    void processQueue();
    uint8_t calculateTargetVolume(AudioPriority priority);
    void checkUartFeedback();
    void updateHealth(AudioHealthState newState);
    void updatePlayState(AudioPlayState newState);
    void checkNightModeAuto();
    void updateStats(uint16_t trackId);
    void resetStats();
    void handleTrackComplete();
    void checkWdtTimeout();
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    bool isDFPlayerResponding();

    // === ОТПРАВКА СОБЫТИЙ (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    void publishTrackEvent(uint16_t trackId, bool isStart);
    void publishStateEvent();
    void publishHealthEvent();
    void publishVolumeEvent(uint8_t volume);
    void publishErrorEvent(const char* error);
    void publishQueueProcessed(size_t remaining);

    // === ОБРАБОТЧИКИ СОБЫТИЙ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === АППАРАТУРА (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    HardwareSerial* _audioSerial = nullptr;
    uint8_t _rxPin = 0;
    uint8_t _txPin = 0;
    uint8_t _busyPin = 255;

    // === КОНФИГУРАЦИЯ (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    AudioConfig _config;
    size_t _maxQueueSize = 50;

    // === ОЧЕРЕДЬ (ВАША, БЕЗ ИЗМЕНЕНИЙ) ===
    std::vector<AudioTask> _trackQueue;

    // === СОСТОЯНИЕ (ВАШЕ, РАСШИРЕНО) ===
    SemaphoreHandle_t _audioMutex = nullptr;
    volatile bool _isNightMode = false;
    volatile bool _initialized = false;
    volatile bool _ready = false;
    volatile bool _isPlayingNow = false;
    volatile bool _initInProgress = false;
    uint16_t _currentTrackId = 0;
    uint32_t _currentTrackStartTime = 0;
    uint32_t _lastCommandMs = 0;
    uint32_t _busyActiveTimestamp = 0;
    uint32_t _pauseStartMs = 0;
    uint8_t _initStage = 0;
    uint32_t _initTimestamp = 0;
    uint32_t _moduleId = MODULE_ID_AUDIO;

    // НОВОЕ: счетчик опубликованных событий
    uint32_t _totalEventsPublished = 0;

    AudioHealthState _healthState = AudioHealthState::OK;
    AudioPlayState _playState = AudioPlayState::IDLE;
    AudioStats _stats;
    uint32_t _lastTickMs = 0;
    uint32_t _statsUpdateCounter = 0;

    uint8_t _uartRxBuf[10];
    uint8_t _uartRxPos = 0;

    // === КОЛБЭКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    OnTrackStartCallback _onTrackStart = nullptr;
    OnTrackCompleteCallback _onTrackComplete = nullptr;
    OnHealthChangeCallback _onHealthChange = nullptr;
    OnStateChangeCallback _onStateChange = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;
    OnPlaybackCallback _onPlayback = nullptr;

    // === КОНСТАНТЫ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ) ===
    static constexpr uint32_t STATS_UPDATE_INTERVAL = 100;
    static constexpr uint32_t DFPLAYER_POST_CMD_DELAY_MS = 350;
    static constexpr uint8_t MAX_VOLUME = 30;
    static constexpr uint8_t MIN_VOLUME = 0;
    static constexpr uint8_t DEFAULT_EQ = 0;
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
    static constexpr uint32_t UART_TIMEOUT_MS = 100;
    static constexpr uint8_t MAX_INIT_RETRIES = 3;
};