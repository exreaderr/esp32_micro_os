// ============================================================================
// LogService.cpp — реализация сбора логов (два контура)
// ============================================================================
#include "LogService.h"
#include "ConfigService.h"
#include "StorageService.h"

// ============================================================================
// СПИНЛОК КОЛЬЦА
// ============================================================================
// portMUX, а не мьютекс: sink вызывается ДО init (мьютекса ещё нет) и из
// контекстов, где ждать нельзя. Критическая секция — копирование ~110 байт,
// микросекунды. Задачная (не ISR) версия примитива.
// ============================================================================
static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

// Трамплин sink -> экземпляр (файловая статика, урок IRAM)
static LogService* s_log = nullptr;
static void logSinkTrampoline(LogLevel level, const char* tag,
                              const char* body) {
    if (s_log != nullptr) s_log->onLog(level, tag, body);
}

LogService& LogService::getInstance() {
    static LogService instance;
    return instance;
}

// ============================================================================
// DESCRIBE: подключаемся самой ранней фазой
// ============================================================================
void LogService::describe() {
    s_log = this;
    ModuleBase::setLogSink(logSinkTrampoline);
}

// ============================================================================
// КОНФИГ-СХЕМА
// ============================================================================
void LogService::registerExtensions() {
    ConfigService::getInstance().addFields("Журнал", {
        { "log.level",   ConfigType::INT, "1", 0, 4, CFG_NONE,
          "Журнал", "Мин. уровень для файла (0=Debug..4=Critical)" },
        { "log.to_file", ConfigType::BOOL, "false", 0, 0, CFG_NONE,
          "Журнал", "Зеркалировать лог в /log.txt" },
    });
}

void LogService::init() {
    _initialized = true;
    // ВАЖНО: log() здесь не вызываем НИКОГДА (рекурсия). Старт — в Serial.
    Serial.println(F("[LOGSVC] init: ring ready, sink attached"));
}

void LogService::start()  { _started = true; }
void LogService::stop()   { _started = false; }

// ============================================================================
// ГОРЯЧИЙ КОНТУР: приём строки (контекст логирующего!)
// ============================================================================
void LogService::onLog(LogLevel level, const char* tag, const char* body) {
    portENTER_CRITICAL(&s_ringMux);

    // Кольцо переполнено относительно точки flush — файл не успевает.
    // Теряем САМУЮ СТАРУЮ не сброшенную запись (flushPos догоняет) + счётчик.
    uint8_t next = (uint8_t)((_head + 1) % LOG_RING_SIZE);
    if (next == _flushPos) {
        _flushPos = (uint8_t)((_flushPos + 1) % LOG_RING_SIZE);
        _dropped++;
    }

    Entry& e = _ring[_head];
    e.ms    = millis();
    e.level = (uint8_t)level;
    safeStrCopy(e.tag, sizeof(e.tag), tag);
    safeStrCopy(e.body, sizeof(e.body), body);
    _head = next;
    _total++;

    portEXIT_CRITICAL(&s_ringMux);
}

// ============================================================================
// ХОЛОДНЫЙ КОНТУР: слив кольца в файл (свой tick, своё время)
// ============================================================================
void LogService::tick() {
    if (!_started || !cfgGetBool("log.to_file", false)) {
        // Файл выключен — flushPos всё равно догоняет head, чтобы кольцо
        // не копило "долг" на случай включения (старьё в файл не нужно).
        portENTER_CRITICAL(&s_ringMux);
        _flushPos = _head;
        portEXIT_CRITICAL(&s_ringMux);
        return;
    }

    // После серии ошибок FS — пауза 60 с, чтобы не молотить битую flash.
    if (_fileFailStreak >= 5) {
        if (millis() - _fileRetryMs < 60000) return;
        _fileFailStreak = 0;
    }

    int minLevel = cfgGetInt("log.level", 1);

    for (uint8_t n = 0; n < LOG_FLUSH_PER_TICK; ++n) {
        Entry e;
        portENTER_CRITICAL(&s_ringMux);
        if (_flushPos == _head) {           // кольцо сброшено
            portEXIT_CRITICAL(&s_ringMux);
            break;
        }
        e = _ring[_flushPos];
        _flushPos = (uint8_t)((_flushPos + 1) % LOG_RING_SIZE);
        portEXIT_CRITICAL(&s_ringMux);

        if ((int)e.level < minLevel) continue;

        // Ротация ДО записи, если файл перерос (проверка раз в слив —
        // fileSize дёшев, но не на каждую строку)
        if (n == 0 && StorageService::getInstance().fileSize(LOG_FILE_PATH)
                       >= LOG_FILE_MAX) {
            StorageService::getInstance().rotate(LOG_FILE_PATH, LOG_FILE_KEEP);
        }

        static const char LVL[] = { 'D', 'I', 'W', 'E', 'C' };
        char line[LOG_TAG_LEN + LOG_BODY_LEN + 24];
        snprintf(line, sizeof(line), "[%08lu] [%c] [%s] %s\n",
                 (unsigned long)e.ms,
                 LVL[e.level <= 4 ? e.level : 1], e.tag, e.body);

        if (!StorageService::getInstance().appendFile(LOG_FILE_PATH, line)) {
            _fileFailStreak++;
            _fileRetryMs = millis();
            break;   // остаток доливаем в следующий tick
        }
        _fileFailStreak = 0;
    }
}

// ============================================================================
// ДОСТУП: последние записи (веб/serial)
// ============================================================================
size_t LogService::tail(char* buf, size_t bufSize, uint8_t maxLines) const {
    if (bufSize == 0) return 0;
    buf[0] = '\0';
    size_t used = 0;

    // Снимок индексов под спинлоком, чтение — вне его.
    // NB: валидность записей определяется ТОЛЬКО счётчиком _total, а не
    // flushPos — файловый курсор не имеет отношения к просмотру кольца.
    uint8_t head;
    uint32_t total;
    portENTER_CRITICAL(&s_ringMux);
    head  = _head;
    total = _total;
    portEXIT_CRITICAL(&s_ringMux);

    uint8_t count = total >= LOG_RING_SIZE ? LOG_RING_SIZE : (uint8_t)total;

    if (maxLines > count) maxLines = count;
    static const char LVL[] = { 'D', 'I', 'W', 'E', 'C' };

    for (uint8_t i = 0; i < maxLines; ++i) {
        // От старых к новым: (head - maxLines + i)
        uint8_t idx = (uint8_t)((head + LOG_RING_SIZE - maxLines + i)
                                % LOG_RING_SIZE);
        Entry e = _ring[idx];   // короткое чтение без лока: строка может
                                // устареть под нами — для просмотра допустимо
        char line[LOG_TAG_LEN + LOG_BODY_LEN + 24];
        int n = snprintf(line, sizeof(line), "[%08lu] [%c] [%s] %s\n",
                         (unsigned long)e.ms,
                         LVL[e.level <= 4 ? e.level : 1], e.tag, e.body);
        if (used + (size_t)n >= bufSize) break;
        memcpy(buf + used, line, (size_t)n);
        used += (size_t)n;
    }
    buf[used] = '\0';
    return used;
}
