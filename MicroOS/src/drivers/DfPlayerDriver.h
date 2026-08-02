// ============================================================================
// DfPlayerDriver.h — MP3-ПЛЕЕР DFPLAYER MINI / КЛОН MP3-TF-16P
// ============================================================================
// Фаза 1. Проверяет цепочку "сервис -> команда -> железо -> событие":
//   VoiceAssistant -> playFolder(3, 9) -> UART-кадр -> BUSY LOW -> ... ->
//   BUSY HIGH -> DRV_EVENT_AUDIO_FINISHED -> VoiceAssistant берёт следующий
//   трек из СВОЕЙ очереди.
//
// === ОСОБЕННОСТИ КЛОНА MP3-TF-16P (MH2024K-24SS) =============================
// Отличия от оригинального DFPlayer (YX5200-24SS), учтённые в драйвере:
//
//   1. ACK (0x41) НЕНАДЁЖЕН: клон часто не отвечает на команды с флагом
//      feedback или отвечает мусором. => Режим useAck=false: живость
//      определяем по BUSY и автомату, а не по ACK. ACK — только опционально.
//   2. POWER-ON: клону нужно 1.5–3 с после подачи питания до приёма команд.
//      => powerOnDelayMs в init().
//   3. RESET (0x0C): после reset клон молчит 1–1.5 с, команды в это окно
//      теряются. => resetRecoveryMs после каждого reset.
//   4. МИНИМУМ ПАУЗЫ МЕЖДУ КОМАНДАМИ: два кадра подряд без паузы клон
//      переваривает через раз. => interCommandMs — принудительный зазор.
//   5. МУСОР В RX: клон шлёт незапрошенные кадры (0x3D "трек кончился",
//      0x3F "SD вставлена"). => waitAck ресинхронизируется по 0x7E и игнорирует
//      чужие кадры; в горячем пути RX вообще не читаем.
//   6. ГЛОБАЛЬНЫЙ ИНДЕКС (0x03 playRoot): порядок зависит от физической
//      очерёдности записи файлов на SD — на клоне ненадёжен.
//      => Рекомендуемый путь — playFolder (0x0F): папки "01".."99",
//      файлы "001.mp3".."255.mp3" — детерминировано и на клоне.
//      playRoot оставлен для совместимости с монолитом (сирена 99), но
//      помечен как ненадёжный на клоне.
//   7. BUSY-settle у клона шире: 350 мс монолита может не хватить,
//      умолчание компат-профиля — 500 мс (настраивается).
//
// Граница ответственности: драйвер — кадры, громкость, автомат BUSY,
//   watchdog, offline. Очередь/приоритеты/тихий режим — VoiceAssistant.
// ============================================================================
#pragma once

#include "../core/IDeviceDriver.h"
#include "../core/EventBus.h"
#include <Arduino.h>

// Команды протокола (10-байтный кадр: 7E FF 06 CMD FB PH LH CKH CKL EF)
constexpr uint8_t DF_CMD_NEXT        = 0x01;
constexpr uint8_t DF_CMD_PLAY_ROOT   = 0x03;   // глобальный индекс (см. п.6)
constexpr uint8_t DF_CMD_VOLUME      = 0x06;   // 0–30
constexpr uint8_t DF_CMD_PLAY_FOLDER = 0x0F;   // (folder, track) — НАДЁЖНО
constexpr uint8_t DF_CMD_ADVERT      = 0x13;   // ADVERT: прервать и продолжить
constexpr uint8_t DF_CMD_STOP_ADVERT = 0x15;   // завершить ADVERT досрочно
constexpr uint8_t DF_CMD_STOP        = 0x16;
constexpr uint8_t DF_CMD_RESET       = 0x0C;

/// Состояния автомата воспроизведения
enum class DfState : uint8_t {
    Offline,     // плеер не отвечает (после серии неудач)
    Idle,        // готов к командам
    Starting,    // play отправлен, ждём падения BUSY (окно settle)
    Playing,     // BUSY LOW — идёт воспроизведение
    Stuck        // BUSY LOW дольше watchdog — зависание
};

class DfPlayerDriver : public IDeviceDriver {
public:
    // --- КОНФИГУРАЦИЯ --------------------------------------------------------
    struct Config {
        // --- Базовые (монолит v2.5.0) ---
        uint32_t baud           = 9600;
        uint32_t busySettleMs   = 350;    // задержка падения BUSY
        uint32_t stuckTimeoutMs = 12000;  // watchdog автомата
        uint32_t ackTimeoutMs   = 150;    // ожидание ACK
        uint8_t  maxAckRetries  = 3;      // неудач подряд -> offline

        // --- Компат-поля MP3-TF-16P (см. шапку) ---
        uint32_t powerOnDelayMs   = 2000; // (2) прогрев клона после power-on
        uint32_t resetRecoveryMs  = 1500; // (3) молчание клона после reset
        uint32_t interCommandMs   = 100;  // (4) минимальный зазор между кадрами
        bool     useAck           = false;// (1) ACK-контроль (у клона — false)
    };

    /// Пресет для клона MP3-TF-16P: ACK выключен, паузы увеличены.
    /// Профиль берёт его и правит точечно (пины/тайминги монолита).
    static Config cloneMP3TF16P() {
        Config c;
        c.busySettleMs    = 500;   // (7) у клона settle шире
        c.useAck          = false;
        c.powerOnDelayMs  = 2000;
        c.resetRecoveryMs = 1500;
        c.interCommandMs  = 100;
        return c;
    }

    /// Пресет для оригинального YX5200 (ACK работает, паузы минимальные).
    static Config genuineDFPlayer() {
        Config c;
        c.useAck          = true;
        c.powerOnDelayMs  = 1000;
        c.resetRecoveryMs = 500;
        c.interCommandMs  = 40;
        return c;
    }

    static DfPlayerDriver& getInstance();

    /// Привязка к пинам/UART из манифеста профиля.
    void configure(uint8_t uartNum, uint8_t pinRx, uint8_t pinTx,
                   uint8_t pinBusy, const Config& cfg);

    // --- IDeviceDriver ---------------------------------------------------
    const char* driverName() const override { return "dfplayer"; }
    bool init() override;
    void poll() override;
    uint32_t getPollIntervalMs() const override { return 20; }
    bool isHealthy() const override { return _state != DfState::Offline; }

    // --- КОМАНДЫ (вызывает VoiceAssistant) -----------------------------------
    /// Трек из папки: playFolder(3, 9) -> /03/009.mp3. НАДЁЖНО на клоне.
    void playFolder(uint8_t folder, uint8_t track);
    /// Глобальный индекс (совместимость с монолитом: сирена 99).
    /// ВНИМАНИЕ: на MP3-TF-16P порядок индексов зависит от очерёдности
    /// записи файлов на SD — предпочитайте playFolder.
    void playRoot(uint16_t track);
    void stop();
    /// Громкость 0–30, запоминается драйвером (восстановление после reset).
    void setVolume(uint8_t vol);
    /// ADVERT (0x13): трек из папки /ADVERT — аппаратное «прервать и
    /// продолжить». ВНИМАНИЕ: на клоне MP3-TF-16P возврат к прерванному
    /// треку ненадёжен (наш опыт) — AudioService по умолчанию эмулирует
    /// ADVERT-семантику программно, эта команда — для genuineDFPlayer().
    void playAdvert(uint16_t track);
    /// Досрочно завершить ADVERT (0x15) — плеер вернётся к прерванному.
    void stopAdvert();

    // --- СОСТОЯНИЕ --------------------------------------------------------------
    /// Профиль привязал драйвер к пинам/UART (false — звука на устройстве нет).
    bool isConfigured() const { return _configured; }
    DfState getState() const { return _state; }
    bool isPlaying() const { return _state == DfState::Playing ||
                                    _state == DfState::Starting; }

private:
    DfPlayerDriver() = default;

    // --- ПРОТОКОЛ ------------------------------------------------------------
    bool sendCommand(uint8_t cmd, uint16_t param, bool withAck = false);
    bool waitAck();          // с ресинхронизацией по 0x7E (п.5)
    void flushRx();
    /// Пауза между кадрами (п.4) + учёт молчания после reset (п.3).
    void commandSpacing();

    // --- АВТОМАТ -----------------------------------------------------------------
    void setState(DfState s);

    // --- ДАННЫЕ ------------------------------------------------------------------
    Config  _cfg;
    uint8_t _uartNum = 0xFF;
    uint8_t _pinRx = 0xFF;
    uint8_t _pinTx = 0xFF;
    uint8_t _pinBusy = 0xFF;
    bool    _configured = false;

    HardwareSerial* _serial = nullptr;

    DfState  _state = DfState::Idle;
    uint32_t _stateSinceMs = 0;
    uint32_t _lastCommandMs = 0;  // для interCommandMs
    uint8_t  _volume = 20;
    uint8_t  _ackFails = 0;
    uint16_t _currentTrack = 0;
};
