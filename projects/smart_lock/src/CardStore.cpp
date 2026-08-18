// ============================================================================
// CardStore.cpp — реализация базы карт (см. шапку .h)
// ============================================================================
#include "CardStore.h"
#include "SmartLockEvents.h"
#include <services/StorageService.h>
#include <core/ResourceManager.h>
#include <core/Version.h>   // MICROOS_VERSION_STR — мета зеркала (5.5.13)
#include <ctime>            // time(nullptr) — возраст запечатывания
#include <cstdlib>          // strtoul — разбор меты зеркала
#include <new>          // placement nothrow: heap-кэш/буфер в init()

CardStore& CardStore::getInstance() {
    static CardStore instance;
    return instance;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void CardStore::init() {
    // 5.2.0: кэш 250 записей (25 КБ) — ОДИН heap-блок на всю жизнь (BSS
    // полон: уроки outbox/HTTP_JSON). Буфер сериализации не нужен вовсе:
    // save/load потоковые по записи. nothrow: провал -> карман 32 записи.
    _users = new (std::nothrow) SlUser[SL_MAX_USERS]();
    if (!_users) {
        log(LogLevel::Error, "heap alloc FAILED — fallback %u records",
            SL_FALLBACK_USERS);
    }

    // NVS-namespace зеркала — в реестр ресурсов (A2: два сервиса не должны
    // писать в один namespace Preferences).
    if (!ResourceManager::getInstance().claimNvsNamespace(SL_DB_NVS_NS,
                                                          "smart_lock.cards")) {
        log(LogLevel::Warning, "NVS ns '%s' уже занят — зеркало под вопросом",
            SL_DB_NVS_NS);
    }

    bool ok = load();
    log(LogLevel::Info, "card db: %u/%u records, load=%d%s%s", _count,
        capacity(), (int)ok,
        _loadedFromBackup ? " (FROM BACKUP)" : "",
        _users ? "" : " [FALLBACK heap]");
    _initialized = true;
}

void CardStore::start() {
    _started = true;
}

void CardStore::stop() {
    _started = false;
}

void CardStore::tick() {
    // Авто-бэкап раз в сутки (монолит: checkLittleFSHealth — бэкап не старше
    // 24 ч). Пишем редко: flash бережём.
    if (millis() - _lastBackupMs < SL_BACKUP_PERIOD_MS) return;
    _lastBackupMs = millis();
    if (_count > 0 && backupNow()) {
        log(LogLevel::Info, "daily backup: %u records", _count);
        // 5.5.13: и зеркало — ежесуточно (инцидент 12.08: ОТА с ФС стёрла
        // users.json + .bak, а зеркало оказалось старым — только ручное
        // запечатывание было, и оператор про него забыл).
        nvsBackupNow();
    }
}

// ============================================================================
// ЗАПРОСЫ
// ============================================================================
int CardStore::findIndex(const char* normalizedId) const {
    for (uint8_t i = 0; i < _count; ++i) {
        if (strcmp(users()[i].id, normalizedId) == 0) return i;
    }
    return -1;
}

const SlUser* CardStore::find(const char* id) const {
    char norm[9];
    if (!carddb::normalizeId(id, norm)) return nullptr;
    int i = findIndex(norm);
    return i >= 0 ? &users()[i] : nullptr;
}

bool CardStore::masterExists() const {
    for (uint8_t i = 0; i < _count; ++i) {
        if (users()[i].type == (uint8_t)KeyType::MASTER) return true;
    }
    return false;
}

// ============================================================================
// МУТАЦИИ
// ============================================================================
const SlUser* CardStore::findByPin(const char* pin) const {
    if (pin == nullptr || pin[0] == '\0') return nullptr;
    for (uint8_t i = 0; i < _count; ++i) {
        if (users()[i].pin[0] != '\0' && strcmp(users()[i].pin, pin) == 0) {
            return &users()[i];
        }
    }
    return nullptr;
}

bool CardStore::pinTaken(const char* pin, const char* exceptId) const {
    if (pin == nullptr || pin[0] == '\0') return false;   // пустой не занят
    for (uint8_t i = 0; i < _count; ++i) {
        if (exceptId != nullptr && strcmp(users()[i].id, exceptId) == 0) continue;
        if (users()[i].pin[0] != '\0' && strcmp(users()[i].pin, pin) == 0) {
            return true;
        }
    }
    return false;
}

bool CardStore::add(const char* id, const char* name, KeyType type,
                    uint8_t track, uint32_t expiry, const char* pin) {
    if (isFull()) return false;

    SlUser u{};
    if (!carddb::normalizeId(id, u.id)) return false;
    if (findIndex(u.id) >= 0) return false;          // дубликат
    carddb::sanitizeName(name, u.name, sizeof(u.name));
    if (!carddb::normalizePin(pin, u.pin)) return false;   // ПИН битый
    if (u.pin[0] != '\0' && pinTaken(u.pin, nullptr)) return false;  // занят
    u.type   = (uint8_t)type;
    u.track  = track > 99 ? 99 : track;
    u.expiry = expiry;

    users()[_count++] = u;
    if (!save()) {                                   // откат при сбое FS
        --_count;
        return false;
    }
    nvsBackupNow();   // 5.5.13: зеркало не должно тихо устаревать
    postCardEvent(sl_ev::cardAdded(), u.id, (int32_t)type);
    return true;
}

bool CardStore::remove(const char* id) {
    char norm[9];
    if (!carddb::normalizeId(id, norm)) return false;
    int i = findIndex(norm);
    if (i < 0) return false;

    // Сдвиг хвоста — порядок записей не важен (find линейный)
    for (uint8_t j = (uint8_t)i; j + 1 < _count; ++j) users()[j] = users()[j + 1];
    --_count;
    if (!save()) return false;
    postCardEvent(sl_ev::cardRemoved(), norm, 0);
    return true;
}

bool CardStore::update(const char* id, const char* name, uint8_t track,
                       uint32_t expiry, const char* pin) {
    char norm[9];
    if (!carddb::normalizeId(id, norm)) return false;
    int i = findIndex(norm);
    if (i < 0) return false;

    // Сентинелы «не трогаем» (5.0.13): пустые поля формы редактирования
    // не должны затирать имя/трек/срок — только явно переданные значения.
    if (name != nullptr)
        carddb::sanitizeName(name, users()[i].name, sizeof(users()[i].name));
    if (track != TRACK_KEEP)
        users()[i].track  = track > 99 ? 99 : track;
    if (expiry != EXPIRY_KEEP)
        users()[i].expiry = expiry;
    if (pin != nullptr) {                    // nullptr — ПИН не трогаем
        char np[7];
        if (!carddb::normalizePin(pin, np)) return false;        // битый
        if (np[0] != '\0' && pinTaken(np, norm)) return false;   // занят
        safeStrCopy(users()[i].pin, sizeof(users()[i].pin), np);
    }
    return save();
}

bool CardStore::recordUse(const char* id, uint32_t unixTime) {
    char norm[9];
    if (!carddb::normalizeId(id, norm)) return false;
    int i = findIndex(norm);
    if (i < 0) return false;              // сгоревший one-time — не считаем

    ++users()[i].uses;
    if (unixTime > 0) users()[i].lastUse = unixTime;
    // NB: каждый проход = перезапись users.json. Flash бережёт LittleFS
    // (wear-leveling), а потеря счётчика при сбое питания — приемлемая
    // цена (монолит: recordUse — тоже синхронное сохранение).
    if (!save()) {
        log(LogLevel::Warning, "recordUse %s: save failed (счётчик в RAM)",
            norm);
        return false;
    }
    return true;
}

bool CardStore::setBlocked(const char* id, bool blocked) {
    char norm[9];
    if (!carddb::normalizeId(id, norm)) return false;
    int i = findIndex(norm);
    if (i < 0) return false;

    users()[i].blocked = blocked ? 1 : 0;
    if (!save()) return false;
    nvsBackupNow();   // 5.5.13: зеркало не должно тихо устаревать
    log(LogLevel::Info, "card %s %s", norm,
        blocked ? "BLOCKED" : "unblocked");
    return true;
}

void CardStore::clear() {
    backupNow();            // последний шанс перед полной зачисткой
    _count = 0;
    if (save()) nvsBackupNow();   // 5.5.13: и зачистка — тоже состояние БД
}

// ============================================================================
// СТОЙКОСТЬ
// ============================================================================
bool CardStore::save() {
    // 5.2.0: ПОТОКОВАЯ запись по записи (serializeOne в стековый кусок) —
    // буфер «на всю базу» (55 КБ) не поднялся бы в RAM, поэтому его нет
    // вовсе: 250 карт пишутся ~250 print'ами. Атомарность прежняя:
    // .tmp + rename (StorageService openTemp/commitTemp).
    StorageService& st = StorageService::getInstance();
    File f = st.openTemp(SL_DB_PATH);
    if (!f) {
        log(LogLevel::Error, "db save FAILED (FS)");
        return false;
    }
    bool ok = f.print("{\"users\":[");
    char chunk[320];   // ~215 Б/запись worst-case (ПИН+статистика) + запас
    for (uint8_t i = 0; ok && i < _count; ++i) {
        size_t w = carddb::serializeOne(users()[i], i == 0,
                                        chunk, sizeof(chunk));
        if (w == 0 || f.write((const uint8_t*)chunk, w) != w) ok = false;
    }
    if (ok) ok = f.print("]}");
    f.close();
    if (!ok || !st.commitTemp(SL_DB_PATH)) {
        log(LogLevel::Error, "db save FAILED (stream)");
        return false;
    }
    ++_saveCount;
    return true;
}

// ============================================================================
// ПОТОКОВЫЙ ЗАГРУЗЧИК (5.2.0): файл читается ПО ОБЪЕКТАМ, целиком в RAM
// не поднимается. Строгость скелета — ровно как у carddb::parse: наш
// сериализатор + пробелы; ручные правки с перестановкой скелета — порча
// (эскалация .bak -> NVS, как прежде). Скобки в именах исключены
// sanitizeName — подсчёт глубины скобок безопасен.
// Возврат: >= 0 записей / -1 порча / -2 файла нет (первый старт — НЕ порча).
// ============================================================================
int CardStore::loadFromFile() {
    File f = StorageService::getInstance().openRead(SL_DB_PATH);
    if (!f) return -2;

    auto skipWsF = [&f]() -> int {   // первый НЕ-пробельный символ (consumes)
        int c;
        do { c = f.read(); } while (c == ' ' || c == '\t' ||
                                    c == '\r' || c == '\n');
        return c;
    };
    // Точная строка из потока (скелет строгий, как у parse)
    auto expect = [&f, &skipWsF](const char* s) -> bool {
        for (; *s; ++s) {
            int c = (*s == '"' || *s == '{' || *s == '[' || *s == ':' ||
                     *s == ']' || *s == '}') ? skipWsF() : f.read();
            if (c != *s) return false;
        }
        return true;
    };

    if (!expect("{\"users\":")) { f.close(); return -1; }
    int c = skipWsF();
    if (c != '[') { f.close(); return -1; }

    uint8_t count = 0;
    c = skipWsF();
    if (c == ']') {                     // пустая база — легально
        bool okEnd = (skipWsF() == '}');
        f.close();
        return okEnd ? 0 : -1;
    }
    if (c != '{') { f.close(); return -1; }

    for (;;) {
        // Вырезать один объект: глубина скобок от первой '{'
        char obj[640];   // ~215 Б сериализованная запись + ручные пробелы
        size_t len = 0;
        int depth = 0;
        int ch = c;
        do {
            if (ch == '{') ++depth;
            if (ch == '}') --depth;
            if (len + 1 >= sizeof(obj)) { f.close(); return -1; }
            obj[len++] = (char)ch;
            if (depth == 0) break;
            ch = f.read();
        } while (ch >= 0);
        obj[len] = '\0';
        if (depth != 0) { f.close(); return -1; }   // обрыв посреди объекта

        if (count >= capacity()) { f.close(); return -1; }  // больше кэша
        if (carddb::parseOne(obj, users()[count]) != 0) {
            log(LogLevel::Error, "users.json: record %u corrupted", count);
            f.close();
            return -1;
        }
        ++count;

        c = skipWsF();
        if (c == ',') { c = skipWsF(); if (c != '{') { f.close(); return -1; } continue; }
        if (c == ']') break;
        f.close();
        return -1;
    }
    bool okEnd = (skipWsF() == '}');
    f.close();
    return okEnd ? (int)count : -1;
}

bool CardStore::load() {
    StorageService& st = StorageService::getInstance();
    _loadedFromBackup = false;

    int n = loadFromFile();
    if (n == -2) {
        // Файла нет — честная пустая база (первый старт), НЕ порча
        log(LogLevel::Info, "users.json absent — empty db (first boot?)");
        return true;
    }
    if (n < 0) {
        // Порча: пробуем .bak, затем NVS-зеркало (эскалация стойкости)
        ++_loadErrors;
        log(LogLevel::Error, "users.json CORRUPTED — trying backup");
        if (st.restoreBackup(SL_DB_PATH)) {
            n = loadFromFile();
            if (n >= 0) {
                _loadedFromBackup = true;
                _count = (uint8_t)n;
                postCardEvent(sl_ev::dbIntegrityFail(), "BAK", 1);
                return true;
            }
        }
        if (nvsRestoreNow()) {
            _loadedFromBackup = true;
            postCardEvent(sl_ev::dbIntegrityFail(), "NVS", 2);
            return true;
        }
        postCardEvent(sl_ev::dbIntegrityFail(), "LOST", 0);
        return false;
    }
    _count = (uint8_t)n;
    return true;
}

bool CardStore::backupNow() {
    return StorageService::getInstance().backup(SL_DB_PATH);
}

bool CardStore::nvsBackupNow() {
    // Буфер зеркала — ВРЕМЕННЫЙ alloc/free на время редкой операции
    // (кнопка в панели), не постоянный расход RAM. Потолок SL_NVS_MIRROR_MAX
    // (раздел NVS 20 КБ делят конфиг и бэкапы): крупная база — ПОЛИТИКА
    // пропуска, а не сбой: users.json + .bak остаются полноценными рубежами.
    char* tmp = new (std::nothrow) char[SL_NVS_TMP_SIZE];
    if (!tmp) {
        log(LogLevel::Error, "NVS mirror: temp alloc failed");
        return false;
    }
    size_t len = carddb::serialize(users(), _count, tmp, SL_NVS_TMP_SIZE);
    if (len == 0) {
        delete[] tmp;
        log(LogLevel::Error, "NVS mirror: serialize overflow (%u records)",
            _count);
        return false;
    }
    if (len + 1 > SL_NVS_MIRROR_MAX) {
        _mirrorSkipped = true;
        log(LogLevel::Info,
            "NVS mirror SKIPPED: %u B > %u B (крупная база — рубеж FS)",
            (unsigned)len, (unsigned)SL_NVS_MIRROR_MAX);
        delete[] tmp;
        return true;
    }
    _mirrorSkipped = false;
    bool ok = StorageService::getInstance().nvsBackup(
        SL_DB_NVS_NS, SL_DB_NVS_KEY, tmp, len + 1);
    delete[] tmp;
    log(ok ? LogLevel::Info : LogLevel::Error,
        "NVS mirror %s (%u bytes)", ok ? "sealed" : "FAILED", (unsigned)len);
    // 5.5.13: мета запечатывания (возраст + fw) — отдельным ключом, чтобы
    // формат db_raw остался монолит-совместимым. unix=0 — время недостоверно.
    if (ok) {
        char meta[64];
        int mn = snprintf(meta, sizeof(meta), "{\"u\":%lu,\"fw\":\"%s\"}",
                          (unsigned long)time(nullptr), MICROOS_VERSION_STR);
        if (mn > 0) {
            StorageService::getInstance().nvsBackup(
                SL_DB_NVS_NS, SL_DB_NVS_META, meta, (size_t)mn + 1);
        }
    }
    return ok;
}

bool CardStore::nvsMirrorInfo(uint32_t& unixOut, char* fwOut, size_t fwSize) {
    unixOut = 0;
    if (fwOut && fwSize) fwOut[0] = '\0';
    char meta[72];
    size_t n = StorageService::getInstance().nvsRestore(
        SL_DB_NVS_NS, SL_DB_NVS_META, meta, sizeof(meta) - 1);
    if (n == 0) return false;      // зеркало старого формата — меты нет
    meta[n] = '\0';
    const char* u = strstr(meta, "\"u\":");
    if (u == nullptr) return false;
    unixOut = (uint32_t)strtoul(u + 4, nullptr, 10);
    const char* f = strstr(meta, "\"fw\":\"");
    if (f != nullptr && fwOut && fwSize) {
        f += 6;
        size_t i = 0;
        while (f[i] && f[i] != '"' && i + 1 < fwSize) { fwOut[i] = f[i]; ++i; }
        fwOut[i] = '\0';
    }
    return true;
}

bool CardStore::nvsRestoreNow() {
    char* tmp = new (std::nothrow) char[SL_NVS_TMP_SIZE];
    if (!tmp) {
        log(LogLevel::Error, "NVS restore: temp alloc failed");
        return false;
    }
    size_t len = StorageService::getInstance().nvsRestore(
        SL_DB_NVS_NS, SL_DB_NVS_KEY, tmp, SL_NVS_TMP_SIZE - 1);
    if (len == 0) { delete[] tmp; return false; }
    tmp[len] = '\0';
    int n = carddb::parse(tmp, users(), capacity());
    delete[] tmp;
    if (n < 0) return false;
    _count = (uint8_t)n;
    return save();   // восстановленное — сразу в файл
}

bool CardStore::hasNvsBackup() {
    return StorageService::getInstance().nvsExists(SL_DB_NVS_NS, SL_DB_NVS_KEY);
}

// ============================================================================
// СОБЫТИЯ
// ============================================================================
void CardStore::postCardEvent(int32_t eventId, const char* id, int32_t code) {
    ShEventData d;
    d.clear();
    d.code = code;
    safeStrCopy(d.payload, sizeof(d.payload), id);
    postEvent(eventId, &d);
}
