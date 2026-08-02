// ============================================================================
// StorageService.h — ЕДИНАЯ МЕХАНИКА ХРАНЕНИЯ (LittleFS + NVS)
// ============================================================================
// Фаза 2. Устраняет ПЯТЬ дублирующих реализаций v4.2.2 (ConfigManager,
// LogManager, UserManager, DataLoggerManager, OTAManager — каждый со своей
// атомарной записью, ротацией и бэкапом). Здесь — одна реализация на всех.
//
// Принципы:
//   · АТОМАРНОСТЬ: запись всегда через *.tmp + rename — обесточка посередине
//     записи оставляет либо старый, либо новый файл, но не кашу;
//   · РЕЗЕРВ: backup/restore для критичных файлов (монолит: users.bak,
//     NVS-зеркало db_backup);
//   · РОТАЦИЯ: path -> path.1 -> path.2 (для логов и телеметрии);
//   · КОНТРОЛЬ МЕСТА: событие STORAGE_EVENT_LOW_SPACE при заполнении > 90%;
//   · NVS-namespace регистрируются в ResourceManager (A2).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include <FS.h>

// Порог предупреждения о заполнении FS (%) — из монолита (90%)
constexpr uint8_t  STORAGE_LOW_SPACE_PERCENT = 90;
constexpr uint32_t STORAGE_SPACE_CHECK_MS    = 60000;  // контроль раз в минуту

class StorageService : public ModuleBase {
public:
    static StorageService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "StorageService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0001; }

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return STORAGE_SPACE_CHECK_MS; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- ФАЙЛЫ (LittleFS) -----------------------------------------------------
    /// Атомарная запись: data -> path.tmp -> rename в path.
    /// false — ошибка FS (событие STORAGE_EVENT_CORRUPTED не публикуется —
    /// это не порча, а сбой записи; фиксируется в логе).
    bool atomicWrite(const char* path, const uint8_t* data, size_t len);
    bool atomicWrite(const char* path, const char* text) {
        return atomicWrite(path, (const uint8_t*)text, strlen(text));
    }

    /// Чтение файла в буфер. Возвращает число байт или 0 (нет файла/ошибка).
    size_t readFile(const char* path, uint8_t* buf, size_t bufSize);

    /// Добавление строки в конец файла (журналы: LogService, AuditService).
    /// НЕ атомарно по построению (append), поэтому журналы — не критичные
    /// данные: цена порчи хвоста файла при обесточке приемлема, а
    /// атомарная перезапись всего файла на каждую строку убила бы flash.
    bool appendFile(const char* path, const char* text);

    bool exists(const char* path);
    bool remove(const char* path);
    size_t fileSize(const char* path);

    /// Открыть файл на чтение (для потоковой отдачи больших файлов —
    /// HttpService отдаёт /audit.log через streamFile, не загружая в RAM).
    /// Пустой File (operator! == true) — файла нет/FS не готова.
    File openRead(const char* path);

    /// Файл существует и непуст (быстрая проверка целостности;
    /// глубокую валидацию JSON делает владелец данных).
    bool fileValid(const char* path);

    // --- БЭКАПЫ ------------------------------------------------------------------
    /// Копия path -> path.bak (атомарно).
    bool backup(const char* path);
    /// Восстановление path.bak -> path. true — восстановлено.
    bool restoreBackup(const char* path);

    // --- РОТАЦИЯ --------------------------------------------------------------------
    /// Ротация: path.keep-1 удаляется, path.i -> path.i+1, path -> path.1.
    void rotate(const char* path, uint8_t keep);

    // --- NVS (Preferences) ----------------------------------------------------------
    /// Зеркало в NVS (монолит: db_backup в namespace "db_backup").
    /// Namespace должен быть зарегистрирован в ResourceManager.
    bool nvsBackup(const char* ns, const char* key, const void* data, size_t len);
    size_t nvsRestore(const char* ns, const char* key, void* buf, size_t bufSize);
    /// Ключ существует и непуст? (для «есть ли бэкап» — без чтения blob'а)
    bool nvsExists(const char* ns, const char* key);

    // --- МЕСТО -------------------------------------------------------------------------
    size_t freeSpace() const;
    size_t totalSpace() const;
    uint8_t usedPercent() const;

private:
    StorageService() = default;

    bool     _fsReady = false;
    bool     _lowSpaceNotified = false;   // событие LOW_SPACE — один раз
};
