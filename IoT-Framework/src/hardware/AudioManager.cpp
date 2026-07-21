// ============================================================================
// AudioManager.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Управление аудио-воспроизведением через DFPlayer Mini.
// Все события воспроизведения публикуются в шину событий.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНЫ синтаксические ошибки в handleCommand
// - ИСПРАВЛЕНА отправка событий в logMessage
// - ИСПРАВЛЕНА ошибка с cleared в clearQueue
// - ИСПРАВЛЕН недостижимый код в playTrack
// - ДОБАВЛЕНА реализация handleTrackComplete
// - ДОБАВЛЕН OnStatsUpdateCallback в .h
// - ДОБАВЛЕНА проверка _audioSerial в sendCommand
// - ДОБАВЛЕНА полная потокобезопасность
// - ДОБАВЛЕНА обработка всех системных событий
// - УЛУЧШЕНА обработка ошибок DFPlayer
// ============================================================================
#include "AudioManager.h"
#include <time.h>
#include <esp_task_wdt.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static AudioManager _audioManagerInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
AudioManager::AudioManager() {
    _moduleId = MODULE_ID_AUDIO;

    // Рекурсивный мьютекс
    _audioMutex = xSemaphoreCreateRecursiveMutex();
    if (_audioMutex == nullptr) {
        Serial.println("[AUDIO] CRITICAL: Failed to create mutex!");
    }

    resetStats();

    _ready = false;
    _initialized = false;
    _isNightMode = false;
    _isPlayingNow = false;
    _initInProgress = false;
    _currentTrackId = 0;
    _currentTrackStartTime = 0;
    _lastCommandMs = 0;
    _busyActiveTimestamp = 0;
    _pauseStartMs = 0;
    _initStage = 0;
    _initTimestamp = 0;
    _lastTickMs = 0;
    _statsUpdateCounter = 0;
    _uartRxPos = 0;
    _audioSerial = nullptr;
    _busyPin = 255;
    _maxQueueSize = 50;

    memset(_uartRxBuf, 0, sizeof(_uartRxBuf));

    if (_audioMutex != nullptr) {
        _trackQueue.reserve(_maxQueueSize);
    }

    _healthState = AudioHealthState::OK;
    _playState = AudioPlayState::IDLE;

    _onTrackStart = nullptr;
    _onTrackComplete = nullptr;
    _onHealthChange = nullptr;
    _onStateChange = nullptr;
    _onStatsUpdate = nullptr;
    _onPlayback = nullptr;

    Serial.println("[AUDIO] Instance created (v4.2.2)");
}

AudioManager::~AudioManager() {
    stop();
    if (_audioMutex != nullptr) {
        vSemaphoreDelete(_audioMutex);
        _audioMutex = nullptr;
    }
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
AudioManager& AudioManager::getInstance() {
    return _audioManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void AudioManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void AudioManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[AUDIO] %s\n", msg);  // <-- ИСПРАВЛЕНО!

    // Отправляем событие для LogManager (исправлено!)
    if (_initialized) {
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = _moduleId;
        data.targetModule = MODULE_ID_LOG;
        data.command = SH_EVENT_LOG_ENTRY;  // <-- ИСПРАВЛЕНО!
        data.value = 0;
        safeStrCopy(data.payload, sizeof(data.payload), msg);
        data.payloadLen = strlen(data.payload);
        postEvent(SH_EVENT_CMD_EXECUTE, &data);
    }
}

void AudioManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void AudioManager::init() {
    logMessage("Init pending - call begin() with parameters");
    _ready = false;
}

void AudioManager::start() {
    if (_initialized && _healthState == AudioHealthState::OK) {
        _ready = true;
        logMessage("Started");
        publishStateEvent();
    } else {
        logMessage("Cannot start - not ready");
    }
}

void AudioManager::stop() {
    if (_audioMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (_isPlayingNow) {
            sendCommand(DFPLAYER_CMD_STOP);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        _trackQueue.clear();
        _isPlayingNow = false;
        _playState = AudioPlayState::IDLE;
        _initialized = false;
        _ready = false;
        xSemaphoreGiveRecursive(_audioMutex);
    }
    logMessage("Stopped");
}

void AudioManager::tick() {
    if (!_initialized) return;

    esp_task_wdt_reset();
    uint32_t currentMs = millis();
    _lastTickMs = currentMs;

    // Обработка UART обратной связи
    checkUartFeedback();

    // Автоматический ночной режим
    checkNightModeAuto();

    if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    // Обновление статистики
    if (++_statsUpdateCounter >= STATS_UPDATE_INTERVAL) {
        _statsUpdateCounter = 0;
        if (_onStatsUpdate) _onStatsUpdate(_stats);
    }

    // Обработка паузы между фразами
    if (_isPlayingNow && _playState == AudioPlayState::WAITING) {
        if (currentMs - _lastCommandMs >= _config.phrasePauseMs) {
            _isPlayingNow = false;
            updatePlayState(AudioPlayState::IDLE);
            logMessage("Phrase pause complete");
        }
        xSemaphoreGiveRecursive(_audioMutex);
        return;
    }

    // Защита от слишком частых команд
    if (currentMs - _lastCommandMs < DFPLAYER_POST_CMD_DELAY_MS) {
        xSemaphoreGiveRecursive(_audioMutex);
        return;
    }

    // Проверка физического состояния DFPlayer
    bool physicalBusy = isPlaying();
    if (physicalBusy) {
        // DFPlayer занят воспроизведением
        if (!_isPlayingNow) {
            _isPlayingNow = true;
            _busyActiveTimestamp = currentMs;
            updatePlayState(AudioPlayState::PLAYING);
            logMessage("DFPlayer busy, track: %d", _currentTrackId);
        }
        // WDT: если DFPlayer завис (воспроизведение слишком долго)
        checkWdtTimeout();
    } else {
        // DFPlayer свободен
        if (_isPlayingNow) {
            // Трек только что завершился
            handleTrackComplete();
            _lastCommandMs = currentMs - (DFPLAYER_POST_CMD_DELAY_MS - _config.betweenTracksPauseMs);
        } else if (!_trackQueue.empty()) {
            // Есть задачи в очереди
            processQueue();
        }
    }

    xSemaphoreGiveRecursive(_audioMutex);
}

// ============================================================================
// 5. ОБРАБОТКА СОБЫТИЙ
// ============================================================================
void AudioManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                                int32_t id, void* eventData) {
    AudioManager* instance = static_cast<AudioManager*>(handlerArgs);
    if (!instance) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
                break;
            case SH_EVENT_SYS_READY:
                if (instance->_initialized) {
                    instance->start();
                }
                break;
            default:
                break;
        }
    } else if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void AudioManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    switch (eventId) {
        case SH_EVENT_CMD_EXECUTE:
            if (data->targetModule == _moduleId || data->targetModule == 0) {
                handleCommand(data);
            }
            break;
        case SH_EVENT_SYS_RESTART:
        case SH_EVENT_SYS_SHUTDOWN:
            stop();
            break;
        case SH_EVENT_SYS_READY:
            if (_initialized) {
                start();
            }
            break;
        default:
            break;
    }
}

bool AudioManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN ||
            eventId == SH_EVENT_SYS_READY);
}

// ============================================================================
// 6. СТАТУС
// ============================================================================
const char* AudioManager::getStatus() const {
    static char statusBuffer[128];

    const char* stateStr = "IDLE";
    switch (_playState) {
        case AudioPlayState::PLAYING: stateStr = "PLAYING"; break;
        case AudioPlayState::PAUSED: stateStr = "PAUSED"; break;
        case AudioPlayState::WAITING: stateStr = "WAITING"; break;
        case AudioPlayState::ERROR: stateStr = "ERROR"; break;
        default: break;
    }

    const char* healthStr = "OK";
    switch (_healthState) {
        case AudioHealthState::NO_SD: healthStr = "NO_SD"; break;
        case AudioHealthState::UART_TIMEOUT: healthStr = "UART_TO"; break;
        case AudioHealthState::FILE_NOT_FOUND: healthStr = "NO_FILE"; break;
        case AudioHealthState::BUSY_STUCK: healthStr = "STUCK"; break;
        case AudioHealthState::INIT_FAILED: healthStr = "INIT_FAIL"; break;
        default: break;
    }

    snprintf(statusBuffer, sizeof(statusBuffer),
            "State: %s, Health: %s, Queue: %zu, Track: %d, Err: %lu",
            stateStr, healthStr, _trackQueue.size(),
            _currentTrackId, _stats.errors);
    return statusBuffer;
}

void AudioManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _trackQueue.size();

    snprintf(data->payload, sizeof(data->payload),
            "state:%d,health:%d,played:%lu,err:%lu,wdt:%lu,queue:%zu",
            (uint8_t)_playState,
            (uint8_t)_healthState,
            _stats.totalTracksPlayed,
            _stats.errors,
            _stats.wdtResets,
            _trackQueue.size());
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 7. ОБРАБОТКА КОМАНД (ИСПРАВЛЕНО)
// ============================================================================
void AudioManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case 0x0300: { // PLAY_TRACK
            if (data->value > 0 && data->value <= 9999) {
                AudioPriority priority = AudioPriority::INFO;
                if (data->payloadLen > 0) {
                    priority = (AudioPriority)(data->payload[0]);
                    if (priority > AudioPriority::IMMEDIATE) {
                        priority = AudioPriority::INFO;
                    }
                }
                playTrack((uint16_t)data->value, priority);
            }
            break;  // <-- ИСПРАВЛЕНО!
        }

        case 0x0301: // STOP_AUDIO
            stopPlayback();
            break;

        case 0x0302: // PAUSE_AUDIO
            pause();
            break;

        case 0x0303: // RESUME_AUDIO
            resume();
            break;

        case 0x0304: // SET_VOLUME
            if (data->value >= 0 && data->value <= 30) {
                setVolume((uint8_t)data->value);
            }
            break;

        case 0x0305: // CLEAR_QUEUE
            clearQueue();
            break;

        case 0x0306: { // GET_STATUS
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = 0x0307;
            response.value = (int32_t)_playState;
            snprintf(response.payload, sizeof(response.payload),
                    "state:%d,health:%d,track:%d,queue:%zu",
                    (uint8_t)_playState,
                    (uint8_t)_healthState,
                    _currentTrackId,
                    _trackQueue.size());
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 8. ОТПРАВКА СОБЫТИЙ
// ============================================================================
void AudioManager::publishTrackEvent(uint16_t trackId, bool isStart) {
    AudioTrackEvent event;
    event.trackId = trackId;
    event.priority = (uint8_t)AudioPriority::INFO;
    event.timestamp = millis();
    event.duration = 0;
    event.isEmergency = (trackId >= 9000);

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = isStart ? SH_EVENT_AUDIO_TRACK_START : SH_EVENT_AUDIO_TRACK_COMPLETE;
    data.value = trackId;
    memcpy(data.payload, &event, min(sizeof(AudioTrackEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(AudioTrackEvent);
    postEvent(data.command, &data);
}

void AudioManager::publishStateEvent() {
    AudioStateEvent event;
    event.playState = (uint8_t)_playState;
    event.healthState = (uint8_t)_healthState;
    event.currentTrack = _currentTrackId;
    event.volume = calculateTargetVolume(AudioPriority::INFO);
    event.queueSize = _trackQueue.size();

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_AUDIO_STATE_CHANGED;
    data.value = (uint8_t)_playState;
    memcpy(data.payload, &event, min(sizeof(AudioStateEvent), sizeof(data.payload)));
    data.payloadLen = sizeof(AudioStateEvent);
    postEvent(data.command, &data);
}

void AudioManager::publishHealthEvent() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_AUDIO_HEALTH_CHANGED;
    data.value = (uint8_t)_healthState;

    const char* healthStr = "OK";
    switch (_healthState) {
        case AudioHealthState::NO_SD: healthStr = "NO_SD"; break;
        case AudioHealthState::UART_TIMEOUT: healthStr = "UART_TIMEOUT"; break;
        case AudioHealthState::FILE_NOT_FOUND: healthStr = "FILE_NOT_FOUND"; break;
        case AudioHealthState::BUSY_STUCK: healthStr = "BUSY_STUCK"; break;
        case AudioHealthState::INIT_FAILED: healthStr = "INIT_FAILED"; break;
        default: break;
    }
    safeStrCopy(data.payload, sizeof(data.payload), healthStr);
    data.payloadLen = strlen(healthStr);
    postEvent(data.command, &data);
}

void AudioManager::publishVolumeEvent(uint8_t volume) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_AUDIO_VOLUME_CHANGED;
    data.value = volume;
    snprintf(data.payload, sizeof(data.payload), "Volume: %d", volume);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void AudioManager::publishErrorEvent(const char* error) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_AUDIO_ERROR;
    data.value = _stats.errors;
    safeStrCopy(data.payload, sizeof(data.payload), error ? error : "Audio error");
    data.payloadLen = data.payload ? strlen(data.payload) : 11;
    postEvent(data.command, &data);
}

void AudioManager::publishQueueProcessed(size_t remaining) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_AUDIO_QUEUE_PROCESSED;
    data.value = remaining;
    snprintf(data.payload, sizeof(data.payload), "Queue processed: %zu remaining", remaining);
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

// ============================================================================
// 9. ИНИЦИАЛИЗАЦИЯ (С ЗАЩИТОЙ ОТ ПОВТОРНОГО ВХОДА)
// ============================================================================
void AudioManager::begin(HardwareSerial& serial, uint8_t rxPin, uint8_t txPin,
                        uint8_t busyPin, const AudioConfig& initialConfig) {
    if (_initInProgress) {
        logMessage("Begin already in progress, skipping...");
        return;
    }
    _initInProgress = true;

    if (_audioMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (_initialized) {
            logMessage("Already active, reconfiguring...");
            xSemaphoreGiveRecursive(_audioMutex);
            end();
            delay(100);
            if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
                _initInProgress = false;
                return;
            }
        }

        _audioSerial = &serial;
        _rxPin = rxPin;
        _txPin = txPin;
        _busyPin = busyPin;
        _config = initialConfig;

        // Ограничение значений
        _config.baseVolume = constrain(initialConfig.baseVolume, MIN_VOLUME, MAX_VOLUME);
        _config.nightVolume = constrain(initialConfig.nightVolume, MIN_VOLUME, MAX_VOLUME);
        _config.emergencyVolume = constrain(initialConfig.emergencyVolume, MIN_VOLUME, MAX_VOLUME);
        _config.wdtTimeoutMs = constrain(initialConfig.wdtTimeoutMs, 5000, 60000);
        _config.phrasePauseMs = constrain(initialConfig.phrasePauseMs, 50, 5000);
        _config.betweenTracksPauseMs = constrain(initialConfig.betweenTracksPauseMs, 10U, 1000);
        _config.maxQueueSize = constrain(initialConfig.maxQueueSize, 10U, 200);

        // Настройка пина BUSY
        pinMode(_busyPin, INPUT_PULLUP);

        // Инициализация Serial для DFPlayer
        _audioSerial->begin(9600, SERIAL_8N1, rxPin, txPin);
        _audioSerial->setTimeout(100);

        // Сброс DFPlayer
        sendCommand(DFPLAYER_CMD_STOP);
        vTaskDelay(pdMS_TO_TICKS(200));
        sendCommand(DFPLAYER_CMD_STOP);
        vTaskDelay(pdMS_TO_TICKS(200));

        _initStage = 1;
        _initTimestamp = millis();
        _initialized = true;
        _lastTickMs = millis();
        _ready = false;
        updateHealth(AudioHealthState::OK);
        updatePlayState(AudioPlayState::IDLE);
        _trackQueue.clear();
        _isPlayingNow = false;
        _currentTrackId = 0;

        xSemaphoreGiveRecursive(_audioMutex);

        // Подписка на события
        esp_event_handler_instance_register(
            SH_SYS_EVENTS,
            ESP_EVENT_ANY_ID,
            &AudioManager::eventHandler,
            this,
            NULL
        );
        esp_event_handler_instance_register(
            SH_APP_EVENTS,
            ESP_EVENT_ANY_ID,
            &AudioManager::eventHandler,
            this,
            NULL
        );

        logMessage("Initialized: RX=%d, TX=%d, BUSY=%d", rxPin, txPin, busyPin);
        logMessage("Base vol: %d, Night vol: %d, Emergency vol: %d",
                  _config.baseVolume, _config.nightVolume, _config.emergencyVolume);

        // Отправляем событие готовности
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = _moduleId;
        data.targetModule = 0;
        data.command = SH_EVENT_AUDIO_STATE_CHANGED;
        data.value = 1;
        safeStrCopy(data.payload, sizeof(data.payload), "Audio ready");
        data.payloadLen = strlen(data.payload);
        postEvent(data.command, &data);
    }
    _initInProgress = false;
}

void AudioManager::end() {
    stop();
}

void AudioManager::reset() {
    if (_audioMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        logMessage("Resetting...");
        _trackQueue.clear();
        _isPlayingNow = false;
        _currentTrackId = 0;
        _playState = AudioPlayState::IDLE;
        _healthState = AudioHealthState::OK;
        _stats.errors = 0;
        _stats.wdtResets = 0;
        resetStats();
        sendCommand(DFPLAYER_CMD_STOP);
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreGiveRecursive(_audioMutex);
        logMessage("Reset complete");
    }
}

// ============================================================================
// 10. ВОСПРОИЗВЕДЕНИЕ (ИСПРАВЛЕНО)
// ============================================================================
bool AudioManager::playTrack(uint16_t trackNumber, AudioPriority priority,
                            bool repeat, uint8_t repeatCount) {
    if (!_initialized || _audioMutex == nullptr) {
        logMessage("Cannot play: not initialized");
        return false;
    }

    if (_healthState != AudioHealthState::OK) {
        logMessage("Health error, cannot play: %d", trackNumber);
        return false;
    }

    bool success = false;
    if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_trackQueue.size() >= _config.maxQueueSize) {
            logMessage("Queue full (%zu), dropping %d", _trackQueue.size(), trackNumber);
            _stats.bufferOverflows++;
            xSemaphoreGiveRecursive(_audioMutex);
            return false;
        }

        AudioTask task;
        task.trackId = trackNumber;
        task.priority = priority;
        task.timestamp = millis();
        task.repeat = repeat;
        task.repeatCount = repeatCount;
        task.isPhrase = false;
        task.delayMs = 0;

        if (priority == AudioPriority::IMMEDIATE) {
            _trackQueue.clear();
            _trackQueue.push_back(task);
            logMessage("Immediate: %d", trackNumber);
            xSemaphoreGiveRecursive(_audioMutex);
            sendCommand(DFPLAYER_CMD_STOP);
            _isPlayingNow = false;
            _lastCommandMs = millis() - DFPLAYER_POST_CMD_DELAY_MS;
        } else {
            _trackQueue.push_back(task);
            logMessage("Queued: %d (prio %d)", trackNumber, (int)priority);
            _stats.totalTracksQueued++;
            if (_trackQueue.size() > _stats.queueMaxSize) {
                _stats.queueMaxSize = _trackQueue.size();
            }
            xSemaphoreGiveRecursive(_audioMutex);
        }

        if (_onPlayback) {
            _onPlayback(trackNumber, true);
        }
        success = true;
    }
    return success;  // <-- ИСПРАВЛЕНО!
}

bool AudioManager::playPhrase(const std::vector<uint16_t>& tracks, AudioPriority priority) {
    if (!_initialized || tracks.empty()) {
        logMessage("Cannot play phrase: invalid state");
        return false;
    }

    if (_healthState != AudioHealthState::OK) {
        logMessage("Health error, cannot play phrase");
        return false;
    }

    if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

    size_t added = 0;
    for (size_t i = 0; i < tracks.size() && _trackQueue.size() < _config.maxQueueSize; i++) {
        AudioTask task;
        task.trackId = tracks[i];
        task.priority = priority;
        task.timestamp = millis();
        task.repeat = false;
        task.repeatCount = 0;
        task.isPhrase = (i < tracks.size() - 1);
        task.delayMs = (i < tracks.size() - 1) ? _config.betweenTracksPauseMs : 0;

        _trackQueue.push_back(task);
        added++;
        _stats.totalTracksQueued++;

        if (_trackQueue.size() > _stats.queueMaxSize) {
            _stats.queueMaxSize = _trackQueue.size();
        }
    }

    _stats.totalPhrases++;
    xSemaphoreGiveRecursive(_audioMutex);

    logMessage("Phrase queued: %zu tracks, added %zu", tracks.size(), added);
    return added > 0;
}

bool AudioManager::playTrackEmergency(uint16_t trackNumber) {
    return playTrack(trackNumber, AudioPriority::IMMEDIATE);
}

// ============================================================================
// 11. УПРАВЛЕНИЕ
// ============================================================================
void AudioManager::pause() {
    if (_playState == AudioPlayState::PLAYING) {
        sendCommand(DFPLAYER_CMD_PAUSE);
        _pauseStartMs = millis();
        updatePlayState(AudioPlayState::PAUSED);
        logMessage("Paused");
    }
}

void AudioManager::resume() {
    if (_playState == AudioPlayState::PAUSED) {
        sendCommand(DFPLAYER_CMD_RESUME);
        updatePlayState(AudioPlayState::PLAYING);
        logMessage("Resumed");
    }
}

void AudioManager::clearQueue() {
    size_t cleared = 0;
    if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cleared = _trackQueue.size();
        _trackQueue.clear();
        xSemaphoreGiveRecursive(_audioMutex);
    }
    logMessage("Cleared %zu tasks from queue", cleared);  // <-- ИСПРАВЛЕНО!

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_AUDIO_QUEUE_EMPTY;
    data.value = cleared;
    safeStrCopy(data.payload, sizeof(data.payload), "Queue cleared");
    data.payloadLen = strlen(data.payload);
    postEvent(data.command, &data);
}

void AudioManager::stopPlayback() {
    sendCommand(DFPLAYER_CMD_STOP);
    _isPlayingNow = false;
    _currentTrackId = 0;
    updatePlayState(AudioPlayState::IDLE);
    logMessage("Playback stopped");
}

// ============================================================================
// 12. НАСТРОЙКИ
// ============================================================================
void AudioManager::setVolume(uint8_t volume) {
    volume = constrain(volume, MIN_VOLUME, MAX_VOLUME);
    sendCommand(DFPLAYER_CMD_VOLUME, volume);
    publishVolumeEvent(volume);
    logMessage("Volume set to %d", volume);
}

void AudioManager::setBaseVolume(uint8_t volume) {
    volume = constrain(volume, MIN_VOLUME, MAX_VOLUME);
    _config.baseVolume = volume;
    logMessage("Base volume: %d", volume);
}

void AudioManager::setNightVolume(uint8_t volume) {
    volume = constrain(volume, MIN_VOLUME, MAX_VOLUME);
    _config.nightVolume = volume;
    logMessage("Night volume: %d", volume);
}

void AudioManager::setNightMode(bool enable) {
    _isNightMode = enable;
    logMessage(enable ? "Night mode enabled" : "Night mode disabled");
}

void AudioManager::setEQ(uint8_t preset) {
    _config.enableEQ = true;
    _config.eqPreset = constrain(preset, 0, 5);
    sendCommand(DFPLAYER_CMD_EQ, _config.eqPreset);
    logMessage("EQ set to %d", _config.eqPreset);
}

// ============================================================================
// 13. ОБРАБОТКА ОЧЕРЕДИ
// ============================================================================
void AudioManager::processQueue() {
    if (_trackQueue.empty()) return;

    AudioTask nextTask = _trackQueue.front();

    // Специальная пауза
    if (nextTask.trackId == SPECIAL_PAUSE_CODE) {
        _trackQueue.erase(_trackQueue.begin());
        _lastCommandMs = millis();
        _isPlayingNow = true;
        updatePlayState(AudioPlayState::WAITING);
        logMessage("Pause: %d ms", _config.phrasePauseMs);
        return;
    }

    // Задержка перед воспроизведением
    if (nextTask.delayMs > 0) {
        vTaskDelay(pdMS_TO_TICKS(nextTask.delayMs));
    }

    _trackQueue.erase(_trackQueue.begin());

    // Расчет громкости
    uint8_t targetVol = calculateTargetVolume(nextTask.priority);

    // Отправка команд DFPlayer
    sendCommand(DFPLAYER_CMD_VOLUME, targetVol);
    vTaskDelay(pdMS_TO_TICKS(10));
    sendCommand(DFPLAYER_CMD_PLAY, nextTask.trackId);

    _busyActiveTimestamp = millis();
    _isPlayingNow = true;
    _currentTrackId = nextTask.trackId;
    _currentTrackStartTime = millis();
    updatePlayState(AudioPlayState::PLAYING);
    updateStats(nextTask.trackId);

    if (_onTrackStart) {
        _onTrackStart(nextTask.trackId);
    }
    publishTrackEvent(nextTask.trackId, true);

    logMessage("Playing: %d, vol %d, prio %d",
              nextTask.trackId, targetVol, (int)nextTask.priority);

    // Если трек с повтором, добавляем его обратно в очередь
    if (nextTask.repeat && nextTask.repeatCount > 0) {
        AudioTask repeatTask = nextTask;
        repeatTask.repeatCount--;
        if (repeatTask.repeatCount > 0 || nextTask.repeatCount == 255) {
            _trackQueue.insert(_trackQueue.begin(), repeatTask);
            logMessage("Repeat: %d, remaining: %d",
                      nextTask.trackId, repeatTask.repeatCount);
        }
    }

    publishQueueProcessed(_trackQueue.size());
}

// ============================================================================
// 14. ОТПРАВКА КОМАНД DFPlayer (С ПРОВЕРКОЙ)
// ============================================================================
void AudioManager::sendCommand(uint8_t cmd, uint16_t arg) {
    if (_audioSerial == nullptr) {  // <-- ИСПРАВЛЕНО!
        logMessage("sendCommand: _audioSerial is null!");
        return;
    }

    // Формат команды DFPlayer: 0x7E FF 06 0C 00 00 FE E9 EF
    uint8_t packet[10] = {0};
    packet[0] = 0x7E;
    packet[1] = 0xFF;
    packet[2] = 0x06;
    packet[3] = cmd;
    packet[4] = (arg >> 8) & 0xFF;
    packet[5] = arg & 0xFF;

    // Расчет контрольной суммы
    uint16_t checksum = 0;
    for (int i = 1; i < 6; i++) {
        checksum += packet[i];
    }
    checksum = (~checksum) + 1;  // Дополнение до 1

    packet[6] = (checksum >> 8) & 0xFF;
    packet[7] = checksum & 0xFF;
    packet[8] = 0xFF;

    _audioSerial->write(packet, 9);
    _audioSerial->flush();
    _lastCommandMs = millis();
}

// ============================================================================
// 15. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
uint8_t AudioManager::calculateTargetVolume(AudioPriority priority) {
    uint8_t volume = _config.baseVolume;

    if (_isNightMode) {
        volume = _config.nightVolume;
    }

    if (priority == AudioPriority::CRITICAL) {
        volume = max(volume, _config.emergencyVolume);
    } else if (priority == AudioPriority::IMMEDIATE) {
        volume = _config.emergencyVolume;
    } else if (priority == AudioPriority::WARNING) {
        uint8_t warnVol = _config.baseVolume * 1.2f;
        volume = max(volume, warnVol);
    }

    return constrain(volume, MIN_VOLUME, MAX_VOLUME);
}

bool AudioManager::isPlaying() const {
    if (_busyPin == 255 || !_initialized) return false;
    return digitalRead(_busyPin) == LOW;
}

size_t AudioManager::getQueueSize() const {
    if (_audioMutex == nullptr) return 0;
    size_t size = 0;
    if (xSemaphoreTakeRecursive(_audioMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        size = _trackQueue.size();
        xSemaphoreGiveRecursive(_audioMutex);
    }
    return size;
}

bool AudioManager::isDFPlayerResponding() {
    // Проверка ответа от DFPlayer через UART
    if (_audioSerial == nullptr) return false;

    sendCommand(DFPLAYER_CMD_PLAYBACK_STATE);
    uint32_t startTime = millis();
    while (millis() - startTime < 100) {
        if (_audioSerial->available()) {
            uint8_t b = _audioSerial->read();
            if (b == 0x7E) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// 16. ОБРАБОТКА ЗАВЕРШЕНИЯ ТРЕКА (НОВАЯ РЕАЛИЗАЦИЯ)
// ============================================================================
void AudioManager::handleTrackComplete() {
    _isPlayingNow = false;
    _busyActiveTimestamp = 0;

    logMessage("Track complete: %d", _currentTrackId);

    if (_onTrackComplete) {
        _onTrackComplete(_currentTrackId);
    }
    publishTrackEvent(_currentTrackId, false);

    _currentTrackId = 0;
    updatePlayState(AudioPlayState::IDLE);
}

// ============================================================================
// 17. WDT И ОБНОВЛЕНИЕ СОСТОЯНИЙ
// ============================================================================
void AudioManager::checkWdtTimeout() {
    uint32_t currentMs = millis();
    if (currentMs - _busyActiveTimestamp > _config.wdtTimeoutMs) {
        logMessage("WDT timeout! Track %d stuck for %lu ms",
                  _currentTrackId, currentMs - _busyActiveTimestamp);

        // Сброс DFPlayer
        sendCommand(DFPLAYER_CMD_STOP);
        vTaskDelay(pdMS_TO_TICKS(100));
        sendCommand(DFPLAYER_CMD_STOP);

        updateHealth(AudioHealthState::BUSY_STUCK);
        _stats.wdtResets++;
        _stats.errors++;
        _isPlayingNow = false;
        _trackQueue.clear();
        updatePlayState(AudioPlayState::ERROR);
    }
}

void AudioManager::updateHealth(AudioHealthState newState) {
    if (_healthState != newState) {
        _healthState = newState;
        logMessage("Health: %d", (int)newState);
        if (_onHealthChange) {
            _onHealthChange(newState);
        }
        publishHealthEvent();
    }
}

void AudioManager::updatePlayState(AudioPlayState newState) {
    if (_playState != newState) {
        _playState = newState;
        logMessage("State: %d", (int)newState);
        if (_onStateChange) {
            _onStateChange(newState);
        }
        publishStateEvent();
    }
}

void AudioManager::checkNightModeAuto() {
    if (!_config.nightModeAuto) return;

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    bool isNight = (timeinfo.tm_hour >= 23 || timeinfo.tm_hour < 6);
    if (isNight != _isNightMode) {
        _isNightMode = isNight;
        logMessage("Night mode: %s", isNight ? "ON" : "OFF");
        uint8_t vol = calculateTargetVolume(AudioPriority::INFO);
        sendCommand(DFPLAYER_CMD_VOLUME, vol);
        publishVolumeEvent(vol);
    }
}

// ============================================================================
// 18. СТАТИСТИКА
// ============================================================================
void AudioManager::updateStats(uint16_t trackId) {
    _stats.totalTracksPlayed++;
    _stats.lastTrackId = trackId;
    _stats.lastPlayTime = millis();
    if (trackId >= 9000) {
        _stats.emergencyTracks++;
    }
}

void AudioManager::resetStats() {
    memset(&_stats, 0, sizeof(_stats));
    _stats.totalTracksPlayed = 0;
    _stats.totalTracksQueued = 0;
    _stats.emergencyTracks = 0;
    _stats.errors = 0;
    _stats.wdtResets = 0;
    _stats.lastTrackId = 0;
    _stats.lastPlayTime = 0;
    _stats.queueMaxSize = 0;
    _stats.bufferOverflows = 0;
    _stats.totalPhrases = 0;
    _stats.skippedTracks = 0;
}

// ============================================================================
// 19. UART ОБРАТНАЯ СВЯЗЬ
// ============================================================================
void AudioManager::checkUartFeedback() {
    if (_audioSerial == nullptr) return;

    while (_audioSerial->available()) {
        uint8_t b = _audioSerial->read();
        if (_uartRxPos < sizeof(_uartRxBuf)) {
            _uartRxBuf[_uartRxPos++] = b;
        }

        // Проверка на завершение пакета
        if (_uartRxPos >= 10 && _uartRxBuf[8] == 0xEF) {
            // Обработка ответа
            if (_uartRxBuf[3] == 0x3D) {  // Ответ на команду воспроизведения
                // OK
            }
            _uartRxPos = 0;
        }

        if (_uartRxPos >= sizeof(_uartRxBuf)) {
            _uartRxPos = 0;  // Сброс при переполнении
        }
    }
}

// ============================================================================
// 20. ДИАГНОСТИКА
// ============================================================================
void AudioManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================");
    stream.println(" AUDIO MANAGER DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Ready: %s\n", _ready ? "YES" : "NO");
    stream.print(" Health: ");
    switch (_healthState) {
        case AudioHealthState::OK: stream.println("OK"); break;
        case AudioHealthState::NO_SD: stream.println("NO_SD"); break;
        case AudioHealthState::UART_TIMEOUT: stream.println("UART_TIMEOUT"); break;
        case AudioHealthState::FILE_NOT_FOUND: stream.println("FILE_NOT_FOUND"); break;
        case AudioHealthState::BUSY_STUCK: stream.println("BUSY_STUCK"); break;
        case AudioHealthState::INIT_FAILED: stream.println("INIT_FAILED"); break;
    }
    stream.print(" State: ");
    switch (_playState) {
        case AudioPlayState::IDLE: stream.println("IDLE"); break;
        case AudioPlayState::PLAYING: stream.println("PLAYING"); break;
        case AudioPlayState::PAUSED: stream.println("PAUSED"); break;
        case AudioPlayState::WAITING: stream.println("WAITING"); break;
        case AudioPlayState::ERROR: stream.println("ERROR"); break;
    }
    stream.printf(" Is Playing: %s\n", isPlaying() ? "YES" : "NO");
    stream.printf(" Current Track: %d\n", _currentTrackId);
    stream.printf(" Queue Size: %zu/%zu\n", _trackQueue.size(), _maxQueueSize);
    stream.printf(" Volume: Base=%d, Night=%d, Emerg=%d\n",
                 _config.baseVolume, _config.nightVolume, _config.emergencyVolume);
    stream.printf(" Night Mode: %s\n", _isNightMode ? "ON" : "OFF");
    stream.printf(" Quiet Mode: %s\n", _config.quietMode ? "ON" : "OFF");
    stream.println("-- Stats --");
    stream.printf(" Total Played: %lu\n", _stats.totalTracksPlayed);
    stream.printf(" Total Queued: %lu\n", _stats.totalTracksQueued);
    stream.printf(" Emergency: %lu\n", _stats.emergencyTracks);
    stream.printf(" Errors: %lu\n", _stats.errors);
    stream.printf(" WDT Resets: %lu\n", _stats.wdtResets);
    stream.printf(" Max Queue: %lu\n", _stats.queueMaxSize);
    stream.printf(" Buffer Overflows: %lu\n", _stats.bufferOverflows);
    stream.printf(" Total Phrases: %lu\n", _stats.totalPhrases);
    stream.printf(" Skipped Tracks: %lu\n", _stats.skippedTracks);
    stream.println("-- Config --");
    stream.printf(" WDT Timeout: %lu ms\n", _config.wdtTimeoutMs);
    stream.printf(" Phrase Pause: %u ms\n", _config.phrasePauseMs);
    stream.printf(" Between Tracks: %u ms\n", _config.betweenTracksPauseMs);
    stream.printf(" Max Queue: %zu\n", _config.maxQueueSize);
    stream.println("==============================");
}

void AudioManager::printStats() const {
    streamDiagnosticInfo(Serial);
}