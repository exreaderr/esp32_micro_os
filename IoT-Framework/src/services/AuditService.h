// ============================================================================
// AuditService.h — АУДИТ-ЖУРНАЛ (B3, требование СКУД)
// ============================================================================
// Фаза 3, порция 1. В монолите v4.2.2 события безопасности (вход, доступ,
// смена настроек) терялись в общем логе и не переживали ротацию. Здесь —
// ОТДЕЛЬНЫЙ персистентный журнал: что касается "кто/когда/что", живёт
// в /audit.log (LittleFS) с ротацией и переживает перезагрузки.
//
// Чем аудит отличается от LogService:
//   · LogService — технический след (всё подряд, RAM + опциональный файл);
//   · AuditService — юридически значимые события (кураторский список:
//     AUTH_*, ACCESS_*, смена конфигурации, Safe Mode, OTA, потеря сети) —
//     всегда в файл, компактный JSON-lines.
//
// Те же два контура, что у LogService: onEvent (диспетчер шины) только
// ставит строку в очередь; tick() сливает в файл. Диспетчер шины НЕ
// блокируется FS-операциями.
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"

// Бюджеты
constexpr uint8_t  AUDIT_QUEUE_SIZE  = 12;    // строк в очереди
constexpr uint8_t  AUDIT_LINE_LEN    = 160;   // одна JSON-строка
constexpr uint8_t  AUDIT_FLUSH_PER_TICK = 4;  // строк за tick (бережём flash)
constexpr uint32_t AUDIT_FILE_MAX    = 65536; // ротация /audit.log, байт
constexpr uint8_t  AUDIT_FILE_KEEP   = 3;     // поколений (больше, чем у лога)
constexpr const char* AUDIT_FILE_PATH = "/audit.log";

class AuditService : public ModuleBase {
public:
    static AuditService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "AuditService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x000B; }

    void registerExtensions() override;   // схема audit.*
    void init() override;
    void start() override;                // подписки на кураторский список
    void stop() override;
    void tick() override;                 // холодный контур: очередь -> файл
    uint32_t getTickIntervalMs() const override { return 1000; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- ДОСТУП ------------------------------------------------------------
    /// Путь к файлу журнала (для веб-выгрузки, Фаза 3).
    const char* filePath() const { return AUDIT_FILE_PATH; }
    uint32_t totalAudited() const { return _total; }
    uint32_t queueOverflows() const { return _overflows; }

private:
    AuditService() = default;

    /// Короткое имя события для JSON ("AUTH_LOGIN" и т.п.).
    /// nullptr — событие вне кураторского списка (не аудируется).
    static const char* eventName(int32_t eventId);

    /// Постановка строки в очередь (горячий контур, из диспетчера шины).
    void enqueue(const char* line);

    char     _queue[AUDIT_QUEUE_SIZE][AUDIT_LINE_LEN];
    uint8_t  _qHead = 0, _qTail = 0;
    uint32_t _total = 0;          // записано в файл
    uint32_t _overflows = 0;      // потеряно при переполнении очереди
};
