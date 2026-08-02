// ============================================================================
// StorageService.cpp — реализация единой механики хранения
// ============================================================================
#include "StorageService.h"
#include "../core/Events.h"
#include "../core/ResourceManager.h"
#include <LittleFS.h>
#include <Preferences.h>

StorageService& StorageService::getInstance() {
    static StorageService instance;
    return instance;
}

// ============================================================================
// INIT: монтирование LittleFS (с форматированием при повреждении)
// ============================================================================
void StorageService::init() {
    // formatOnFail=true: битый образ (обесточка при записи до появления
    // атомарности, глюк flash) не превращает устройство в кирпич — FS
    // форматируется, критичные данные восстановятся из NVS-зеркал.
    _fsReady = LittleFS.begin(true);
    if (_fsReady) {
        log(LogLevel::Info, "LittleFS mounted: %u/%u bytes used (%u%%)",
            (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(),
            usedPercent());
    } else {
        log(LogLevel::Critical, "LittleFS mount FAILED even after format");
    }
    _initialized = true;
}

void StorageService::start()  { _started = true; }
void StorageService::stop()   { _started = false; }

// ============================================================================
// TICK: контроль заполнения (событие — один раз, не спамим)
// ============================================================================
void StorageService::tick() {
    if (!_fsReady) return;
    uint8_t used = usedPercent();
    if (used >= STORAGE_LOW_SPACE_PERCENT && !_lowSpaceNotified) {
        _lowSpaceNotified = true;
        ShEventData d; d.clear();
        d.code = used;
        postEvent(STORAGE_EVENT_LOW_SPACE, &d);
        log(LogLevel::Warning, "LittleFS %u%% full", used);
    } else if (used < STORAGE_LOW_SPACE_PERCENT - 5) {
        _lowSpaceNotified = false;   // гистерезис 5%
    }
}

// ============================================================================
// АТОМАРНАЯ ЗАПИСЬ
// ============================================================================
bool StorageService::atomicWrite(const char* path, const uint8_t* data, size_t len) {
    if (!_fsReady) return false;

    // Путь временного файла: "<path>.tmp"
    char tmpPath[64];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

    File f = LittleFS.open(tmpPath, "w");
    if (!f) {
        log(LogLevel::Error, "atomicWrite: cannot open %s", tmpPath);
        return false;
    }
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        log(LogLevel::Error, "atomicWrite: short write %u/%u to %s",
            (unsigned)written, (unsigned)len, tmpPath);
        LittleFS.remove(tmpPath);
        return false;
    }

    // rename атомарен в LittleFS: старый файл замещается одной операцией
    LittleFS.remove(path);
    if (!LittleFS.rename(tmpPath, path)) {
        log(LogLevel::Error, "atomicWrite: rename failed %s -> %s", tmpPath, path);
        return false;
    }
    return true;
}

// ============================================================================
// ДОБАВЛЕНИЕ В КОНЕЦ (журналы)
// ============================================================================
bool StorageService::appendFile(const char* path, const char* text) {
    if (!_fsReady || text == nullptr) return false;
    File f = LittleFS.open(path, FILE_APPEND);
    if (!f) {
        log(LogLevel::Error, "appendFile: cannot open %s", path);
        return false;
    }
    size_t want = strlen(text);
    size_t written = f.write((const uint8_t*)text, want);
    f.close();
    if (written != want) {
        log(LogLevel::Error, "appendFile: short write %u/%u to %s",
            (unsigned)written, (unsigned)want, path);
        return false;
    }
    return true;
}

// ============================================================================
// ЧТЕНИЕ
// ============================================================================
size_t StorageService::readFile(const char* path, uint8_t* buf, size_t bufSize) {
    if (!_fsReady) return 0;
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t n = f.read(buf, bufSize);
    f.close();
    return n;
}

bool StorageService::exists(const char* path) {
    return _fsReady && LittleFS.exists(path);
}

bool StorageService::remove(const char* path) {
    return _fsReady && LittleFS.remove(path);
}

size_t StorageService::fileSize(const char* path) {
    if (!_fsReady) return 0;
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t n = f.size();
    f.close();
    return n;
}

File StorageService::openRead(const char* path) {
    if (!_fsReady) return File();
    return LittleFS.open(path, "r");   // пустой File, если файла нет
}

bool StorageService::fileValid(const char* path) {
    return exists(path) && fileSize(path) > 0;
}

// ============================================================================
// БЭКАПЫ
// ============================================================================
bool StorageService::backup(const char* path) {
    if (!fileValid(path)) return false;

    char bakPath[64];
    snprintf(bakPath, sizeof(bakPath), "%s.bak", path);

    File src = LittleFS.open(path, "r");
    if (!src) return false;
    // Бэкап тоже атомарно: через .tmp
    char tmpPath[72];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", bakPath);
    File dst = LittleFS.open(tmpPath, "w");
    if (!dst) { src.close(); return false; }

    uint8_t buf[256];
    size_t n;
    bool ok = true;
    while ((n = src.read(buf, sizeof(buf))) > 0) {
        if (dst.write(buf, n) != n) { ok = false; break; }
    }
    src.close();
    dst.close();

    if (!ok) { LittleFS.remove(tmpPath); return false; }
    LittleFS.remove(bakPath);
    return LittleFS.rename(tmpPath, bakPath);
}

bool StorageService::restoreBackup(const char* path) {
    char bakPath[64];
    snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
    if (!fileValid(bakPath)) return false;

    File src = LittleFS.open(bakPath, "r");
    if (!src) return false;
    char tmpPath[64];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    File dst = LittleFS.open(tmpPath, "w");
    if (!dst) { src.close(); return false; }

    uint8_t buf[256];
    size_t n;
    bool ok = true;
    while ((n = src.read(buf, sizeof(buf))) > 0) {
        if (dst.write(buf, n) != n) { ok = false; break; }
    }
    src.close();
    dst.close();

    if (!ok) { LittleFS.remove(tmpPath); return false; }
    LittleFS.remove(path);
    bool renamed = LittleFS.rename(tmpPath, path);
    if (renamed) {
        log(LogLevel::Warning, "%s restored from backup", path);
    }
    return renamed;
}

// ============================================================================
// РОТАЦИЯ
// ============================================================================
void StorageService::rotate(const char* path, uint8_t keep) {
    if (!_fsReady || keep == 0) return;

    char from[72], to[72];
    // Старший файл удаляется
    snprintf(from, sizeof(from), "%s.%u", path, keep);
    LittleFS.remove(from);
    // Сдвиг: .i -> .i+1
    for (int i = keep - 1; i >= 1; --i) {
        snprintf(from, sizeof(from), "%s.%d", path, i);
        snprintf(to, sizeof(to), "%s.%d", path, i + 1);
        if (LittleFS.exists(from)) {
            LittleFS.remove(to);
            LittleFS.rename(from, to);
        }
    }
    // Текущий -> .1
    if (LittleFS.exists(path)) {
        snprintf(to, sizeof(to), "%s.1", path);
        LittleFS.remove(to);
        LittleFS.rename(path, to);
    }
}

// ============================================================================
// NVS
// ============================================================================
bool StorageService::nvsBackup(const char* ns, const char* key,
                               const void* data, size_t len) {
    Preferences prefs;
    if (!prefs.begin(ns, false)) return false;
    size_t written = prefs.putBytes(key, data, len);
    prefs.end();
    return written == len;
}

size_t StorageService::nvsRestore(const char* ns, const char* key,
                                  void* buf, size_t bufSize) {
    Preferences prefs;
    if (!prefs.begin(ns, true)) return 0;   // read-only
    size_t n = prefs.getBytes(key, buf, bufSize);
    prefs.end();
    return n;
}

bool StorageService::nvsExists(const char* ns, const char* key) {
    Preferences prefs;
    if (!prefs.begin(ns, true)) return false;
    size_t len = prefs.getBytesLength(key);
    prefs.end();
    return len > 0;
}

// ============================================================================
// МЕСТО
// ============================================================================
size_t StorageService::freeSpace() const {
    if (!_fsReady) return 0;
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

size_t StorageService::totalSpace() const {
    return _fsReady ? LittleFS.totalBytes() : 0;
}

uint8_t StorageService::usedPercent() const {
    size_t total = totalSpace();
    if (total == 0) return 100;
    return (uint8_t)((LittleFS.usedBytes() * 100) / total);
}
