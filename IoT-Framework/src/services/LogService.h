// ============================================================================
// LogService.h — ЕДИНАЯ ТОЧКА СБОРА ЛОГОВ (Фаза 3, порция 1)
// ============================================================================
// Заменяет LogManager монолита v4.2.2 (одна из 5 дублированных реализаций
// хранения) и закрывает главное: логи перестают существовать ТОЛЬКО в
// Serial — их можно посмотреть с веб-страницы и после перезагрузки.
//
// Архитектура (два контура, разведённые по контекстам):
//   1. ГОРЯЧИЙ: ModuleBase::log() любого модуля -> sink -> запись в
//      RAM-кольцо под спинлоком. Никаких FS/heap-операций из чужого
//      контекста — диспетчер шины и сетевые задачи не блокируются.
//   2. ХОЛОДНЫЙ: tick() самого LogService сливает накопленное в файл
//      /log.txt через StorageService::appendFile, с ротацией.
//
// Подключение: setLogSink() вызывается в describe() — самой ранней фазой
// жизненного цикла, чтобы поймать логи registerExtensions()/init()
// остальных модулей. До init() работает только RAM-контур (FS ещё не
// смонтирована) — и это нормально: кольцо переживает до flush.
//
// ВАЖНО: LogService НИКОГДА не вызывает log() — рекурсия исключена по
// построению. Свои (редкие) сообщения пишет прямо в Serial с тегом [LOGSVC].
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"

// Бюджеты
constexpr uint8_t  LOG_RING_SIZE     = 48;    // записей в RAM-кольце
constexpr uint8_t  LOG_TAG_LEN       = 12;    // имя модуля
constexpr uint8_t  LOG_BODY_LEN      = 96;    // текст записи
constexpr uint8_t  LOG_FLUSH_PER_TICK = 8;    // строк в файл за один tick
constexpr uint32_t LOG_FILE_MAX      = 32768; // ротация /log.txt, байт
constexpr uint8_t  LOG_FILE_KEEP     = 2;     // поколений ротации
constexpr const char* LOG_FILE_PATH  = "/log.txt";

class LogService : public ModuleBase {
public:
    static LogService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "LogService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x000A; }

    void describe() override;           // подключение sink — самая ранняя фаза
    void registerExtensions() override; // схема log.*
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;               // холодный контур: кольцо -> файл
    uint32_t getTickIntervalMs() const override { return 500; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- ТОЧКА ВХОДА SINK (НЕ для прикладного кода!) ---------------------
    /// Вызывается из ModuleBase::log() ЛЮБОГО модуля, в контексте
    /// логирующего. Только RAM-кольцо под спинлоком, никаких FS/heap.
    void onLog(LogLevel level, const char* tag, const char* body);

    // --- ДОСТУП (веб-UI Фазы 3, serial-команды) ---------------------------
    /// Последние maxLines записей кольца в буфер (по строке на запись).
    size_t tail(char* buf, size_t bufSize, uint8_t maxLines = 20) const;
    /// Всего записей принято / потеряно при переполнении кольца.
    uint32_t totalLogged() const { return _total; }
    uint32_t dropped() const { return _dropped; }

private:
    LogService() = default;

    struct Entry {
        uint32_t ms;                  // millis() записи
        uint8_t  level;               // LogLevel
        char     tag[LOG_TAG_LEN];
        char     body[LOG_BODY_LEN];
    };

    Entry    _ring[LOG_RING_SIZE];
    uint8_t  _head    = 0;            // куда писать следующую
    uint8_t  _flushPos = 0;           // откуда сливать в файл
    uint32_t _total   = 0;
    uint32_t _dropped = 0;            // не влезло в кольцо до flush
    uint8_t  _fileFailStreak = 0;     // подряд ошибок appendFile
    uint32_t _fileRetryMs = 0;        // когда можно снова писать в файл
};
