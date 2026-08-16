// ============================================================================
// DfPlayerDriver.cpp — реализация драйвера DFPlayer Mini / MP3-TF-16P
// ============================================================================
#include "DfPlayerDriver.h"
#include "../core/Events.h"

DfPlayerDriver& DfPlayerDriver::getInstance() {
    static DfPlayerDriver instance;
    return instance;
}

// ============================================================================
// КОНФИГУРАЦИЯ
// ============================================================================
void DfPlayerDriver::configure(uint8_t uartNum, uint8_t pinRx, uint8_t pinTx,
                               uint8_t pinBusy, const Config& cfg) {
    _uartNum = uartNum;
    _pinRx = pinRx;
    _pinTx = pinTx;
    _pinBusy = pinBusy;
    _cfg = cfg;
    _configured = true;
}

// ============================================================================
// INIT: UART -> прогрев клона -> reset -> volume
// ============================================================================
bool DfPlayerDriver::init() {
    if (!_configured) return false;

    // Статические экземпляры UART (номер из манифеста; UART0 — консоль
    // платформы, реестр ресурсов его уже защитил).
    static HardwareSerial ser1(1);
    static HardwareSerial ser2(2);
    if (_uartNum == 1)      _serial = &ser1;
    else if (_uartNum == 2) _serial = &ser2;
    else return false;

    _serial->begin(_cfg.baud, SERIAL_8N1, _pinRx, _pinTx);
    _serial->setTimeout(_cfg.ackTimeoutMs);

    pinMode(_pinBusy, INPUT);   // BUSY: выход плеера, чистый вход

    // (2) MP3-TF-16P: прогрев после подачи питания — до первой команды!
    delay(_cfg.powerOnDelayMs);

    if (_cfg.useAck) {
        // --- Оригинал: проверка живости по ACK ------------------------------
        bool alive = sendCommand(DF_CMD_RESET, 0, /*withAck*/ true);
        delay(_cfg.resetRecoveryMs);
        if (alive) alive = sendCommand(DF_CMD_VOLUME, _volume, /*withAck*/ true);
        if (!alive) {
            setState(DfState::Offline);
            ShEventData d; d.clear();
            EventBus::getInstance().post(DRV_EVENT_AUDIO_OFFLINE, &d);
            return false;
        }
    } else {
        // --- MP3-TF-16P: ACK не ждём (п.1). Reset вслепую, пауза на ---------
        // молчание клона, затем громкость. Живость покажет автомат BUSY;
        // если плеер мёртв — watchdog/Stuck доведёт до Offline.
        sendCommand(DF_CMD_RESET, 0);
        delay(_cfg.resetRecoveryMs);
        sendCommand(DF_CMD_VOLUME, _volume);
    }

    setState(DfState::Idle);
    return true;
}

// ============================================================================
// ПРОТОКОЛ
// ============================================================================
void DfPlayerDriver::commandSpacing() {
    // (4) зазор между кадрами: клон теряет команды, идущие вплотную
    uint32_t since = millis() - _lastCommandMs;
    if (since < _cfg.interCommandMs) {
        delay(_cfg.interCommandMs - since);
    }
    _lastCommandMs = millis();
}

bool DfPlayerDriver::sendCommand(uint8_t cmd, uint16_t param, bool withAck) {
    // У клона ACK отключён — withAck имеет силу только для оригинала
    bool wantAck = withAck && _cfg.useAck;

    commandSpacing();

    uint8_t frame[10];
    frame[0] = 0x7E;
    frame[1] = 0xFF;
    frame[2] = 0x06;
    frame[3] = cmd;
    frame[4] = wantAck ? 0x01 : 0x00;
    frame[5] = (uint8_t)(param >> 8);
    frame[6] = (uint8_t)(param & 0xFF);

    uint16_t sum = 0;
    for (uint8_t i = 1; i <= 6; ++i) sum += frame[i];
    uint16_t ck = (uint16_t)(0 - sum);
    frame[7] = (uint8_t)(ck >> 8);
    frame[8] = (uint8_t)(ck & 0xFF);
    frame[9] = 0xEF;

    if (wantAck) flushRx();
    _serial->write(frame, sizeof(frame));

    if (!wantAck) return true;
    return waitAck();
}

bool DfPlayerDriver::waitAck() {
    // (5) Клон мусорит в RX незапрошенными кадрами (0x3D/0x3F) — ищем
    // ACK-кадр по маркеру 0x7E с ресинхронизацией, чужие кадры игнорируем.
    uint32_t start = millis();
    while (millis() - start < _cfg.ackTimeoutMs) {
        while (_serial->available()) {
            uint8_t b = (uint8_t)_serial->read();
            if (b != 0x7E) continue;                 // ресинхронизация
            // Есть начало кадра — читаем заголовок ответа
            uint8_t hdr[3];
            uint32_t t0 = millis();
            uint8_t got = 0;
            while (got < 3 && millis() - t0 < 20) {
                if (_serial->available()) hdr[got++] = (uint8_t)_serial->read();
            }
            if (got == 3 && hdr[2] == 0x41) return true;  // ACK
            // Не ACK (уведомление плеера) — пропускаем остаток кадра и ждём дальше
            flushRx();
        }
        delay(2);
    }
    return false;
}

void DfPlayerDriver::flushRx() {
    while (_serial->available()) (void)_serial->read();
}

// ============================================================================
// КОМАНДЫ
// ============================================================================
void DfPlayerDriver::playFolder(uint8_t folder, uint8_t track) {
    if (_state == DfState::Offline) return;
    sendCommand(DF_CMD_PLAY_FOLDER, ((uint16_t)folder << 8) | track);
    _currentTrack = track;
    setState(DfState::Starting);

    ShEventData d; d.clear();
    d.code = track;
    EventBus::getInstance().post(DRV_EVENT_AUDIO_STARTED, &d);
}

void DfPlayerDriver::playRoot(uint16_t track) {
    if (_state == DfState::Offline) return;
    sendCommand(DF_CMD_PLAY_ROOT, track);
    _currentTrack = track;
    setState(DfState::Starting);

    ShEventData d; d.clear();
    d.code = (int32_t)track;
    EventBus::getInstance().post(DRV_EVENT_AUDIO_STARTED, &d);
}

void DfPlayerDriver::stop() {
    if (_state == DfState::Offline) return;
    sendCommand(DF_CMD_STOP, 0);
    setState(DfState::Idle);
}

void DfPlayerDriver::playAdvert(uint16_t track) {
    if (_state == DfState::Offline) return;
    // ADVERT НЕ меняет автомат: BUSY остаётся LOW (плеер продолжает
    // играть — сначала advert, затем прерванный трек). Сервис знает
    // семантику и не ждёт STARTED от этой команды.
    sendCommand(DF_CMD_ADVERT, track, _cfg.useAck);
    _currentTrack = track;
}

void DfPlayerDriver::stopAdvert() {
    if (_state == DfState::Offline) return;
    sendCommand(DF_CMD_STOP_ADVERT, 0, _cfg.useAck);
}

void DfPlayerDriver::setVolume(uint8_t vol) {
    if (vol > 30) vol = 30;
    _volume = vol;
    if (_state != DfState::Offline) {
        sendCommand(DF_CMD_VOLUME, vol);
    }
}

// ============================================================================
// POLL: автомат состояния воспроизведения
// ============================================================================
void DfPlayerDriver::poll() {
    if (!_configured) return;

    if (_state == DfState::Offline) {
        // Редкая проба восстановления (раз в 10 с). На клоне — вслепую:
        // плеер ожил <=> BUSY/команды снова работают; точную живость
        // покажет следующий реальный трек (settle-ветка).
        if (millis() - _stateSinceMs > 10000) {
            sendCommand(DF_CMD_VOLUME, _volume);
            if (!_cfg.useAck || _ackFails == 0) {
                // В клон-режиме оптимистично возвращаемся в Idle: мёртвый
                // плеер снова уйдёт в Offline через watchdog.
                setState(DfState::Idle);
                ShEventData d; d.clear();
                EventBus::getInstance().post(DRV_EVENT_AUDIO_RESTORED, &d);
            } else {
                _stateSinceMs = millis();
            }
        }
        return;
    }

    bool busy = (digitalRead(_pinBusy) == LOW);

    switch (_state) {
        case DfState::Starting:
            // Окно settle: у MP3-TF-16P шире (п.7, умолчание 500 мс)
            if (millis() - _stateSinceMs >= _cfg.busySettleMs) {
                if (busy) {
                    setState(DfState::Playing);
                } else {
                    // BUSY не упал: трек не стартовал (нет файла/SD).
                    // Для клона это НОРМАЛЬНЫЙ исход — VoiceAssistant
                    // должен взять следующий трек, очередь не встаёт.
                    setState(DfState::Idle);
                    ShEventData d; d.clear();
                    d.code = (int32_t)_currentTrack;
                    EventBus::getInstance().post(DRV_EVENT_AUDIO_FINISHED, &d);
                }
            }
            break;

        case DfState::Playing:
            if (!busy) {
                setState(DfState::Idle);
                ShEventData d; d.clear();
                d.code = (int32_t)_currentTrack;
                EventBus::getInstance().post(DRV_EVENT_AUDIO_FINISHED, &d);
            } else if (millis() - _stateSinceMs > _cfg.stuckTimeoutMs) {
                // WATCHDOG: BUSY не поднимается — плеер завис (монолит)
                setState(DfState::Stuck);
                ShEventData d; d.clear();
                d.code = (int32_t)_currentTrack;
                EventBus::getInstance().post(DRV_EVENT_AUDIO_FINISHED, &d);
            }
            break;

        case DfState::Stuck:
            // Лечим reset'ом. На клоне: вслепую + пауза молчания (п.3),
            // считаем попытки; исчерпали -> Offline.
            sendCommand(DF_CMD_RESET, 0, /*withAck*/ true);
            delay(_cfg.resetRecoveryMs);
            sendCommand(DF_CMD_VOLUME, _volume);
            if (++_ackFails >= _cfg.maxAckRetries) {
                _ackFails = 0;
                setState(DfState::Offline);
                ShEventData d; d.clear();
                EventBus::getInstance().post(DRV_EVENT_AUDIO_OFFLINE, &d);
            } else {
                setState(DfState::Idle);   // следующий трек покажет, ожил ли
            }
            break;

        case DfState::Idle:
        case DfState::Offline:
            break;
    }
}

// ============================================================================
// СМЕНА СОСТОЯНИЯ
// ============================================================================
void DfPlayerDriver::setState(DfState s) {
    _state = s;
    _stateSinceMs = millis();
}
