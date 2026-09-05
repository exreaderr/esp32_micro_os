// ============================================================================
// ConfigService.cpp — реализация конфигурации с инжекцией схем
// ============================================================================
// Хранение значений: ВСЕ значения — строки фиксированной длины (как в
// монолите NVS). Тип применяется на границах: парсинг в get*(), валидация
// в set(). JSON-файл — плоский {"key":"value"} без вложенности: для
// 64 полей это самый дешёвый и надёжный формат (парсер — ручной, без
// ArduinoJson на этом уровне; тяжёлые JSON — дело API-слоя Phase 3).
// ============================================================================
#include "ConfigService.h"
#include "StorageService.h"
#include "../core/Events.h"
#include "../core/ResourceManager.h"
#include <Preferences.h>
#include <cstdlib>   // malloc/free — транзиентные буферы снимков

ConfigService& ConfigService::getInstance() {
    static ConfigService instance;
    return instance;
}

// ============================================================================
// INIT: namespace + миграция + загрузка значений
// ============================================================================
void ConfigService::init() {
    // A2: namespace NVS регистрируем в реестре ресурсов
    ResourceManager::getInstance().claimNvsNamespace(CFG_NVS_NS, "config");

    // Миграция NVS монолита v2.5.0 — один раз (флаг "migrated" в NVS)
    migrateFromLegacyNvs();

    loadFromJson();
    _initialized = true;
    log(LogLevel::Info, "init: %u fields registered", _fieldCount);
}

void ConfigService::start()  { _started = true; }
void ConfigService::stop()   { _started = false; }

// ============================================================================
// TICK: отложенная запись (дебаунс 1 с — не долбим flash на каждое поле)
// ============================================================================
void ConfigService::tick() {
    if (_dirty && millis() - _dirtySinceMs > CFG_SAVE_DEBOUNCE_MS) {
        saveToJson();
        _dirty = false;
    }
}

// ============================================================================
// РЕГИСТРАЦИЯ СХЕМ
// ============================================================================
bool ConfigService::addFields(const char* group, const ConfigField* fields,
                              uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
        if (_fieldCount >= CFG_MAX_FIELDS) {
            log(LogLevel::Error, "field table full, '%s' rejected",
                fields[i].key);
            return false;
        }
        // Дубликат ключа — ошибка регистранта (неймспейсы должны
        // исключать это; ловим на всякий случай)
        if (findField(fields[i].key) != nullptr) {
            log(LogLevel::Error, "duplicate config key '%s'", fields[i].key);
            continue;
        }

        ConfigField& dst = _fields[_fieldCount];
        dst = fields[i];
        dst.group = group;   // группа — от пакета (единая для всех полей)

        // Стартовое значение = умолчание (перезапишется из JSON/NVS)
        safeStrCopy(_values[_fieldCount], CFG_VALUE_LEN, fields[i].defValue);

        // SECRET-поля читаются не из JSON, а из NVS
        if (fields[i].flags & CFG_SECRET) {
            Preferences prefs;
            if (prefs.begin(CFG_NVS_NS, true)) {
                String v = prefs.getString(fields[i].key, "");
                prefs.end();
                if (v.length() > 0) {
                    safeStrCopy(_values[_fieldCount], CFG_VALUE_LEN, v.c_str());
                }
            }
        }
        _fieldCount++;
    }
    return true;
}

// 5.8.0, аккордеон «Служебные» (см. ConfigService.h): профильный список
// групп, которые панель прячет в свёрнутый блок. Только отображение.
void ConfigService::setHiddenGroups(const char* csv) {
    safeStrCopy(_hiddenGroups, sizeof(_hiddenGroups), csv ? csv : "");
    if (_hiddenGroups[0] != '\0') {
        log(LogLevel::Info, "config: служебные группы (аккордеон): \"%s\"",
            _hiddenGroups);
    }
}

bool ConfigService::groupHidden(const char* group) const {
    if (group == nullptr || group[0] == '\0' || _hiddenGroups[0] == '\0') {
        return false;
    }
    const size_t gl = strlen(group);
    const char* p = _hiddenGroups;
    while (*p != '\0') {
        const char* comma = strchr(p, ',');
        size_t tl = comma ? (size_t)(comma - p) : strlen(p);
        while (tl > 0 && *p == ' ') { ++p; --tl; }        // "А, Б" — пробелы
        while (tl > 0 && p[tl - 1] == ' ') --tl;          // по краям не значат
        if (tl == gl && strncmp(p, group, gl) == 0) return true;
        if (comma == nullptr) break;
        p = comma + 1;
    }
    return false;
}

// ============================================================================
// ДОСТУП К ЗНАЧЕНИЯМ
// ============================================================================
int8_t ConfigService::findFieldIndex(const char* key) const {
    for (uint8_t i = 0; i < _fieldCount; ++i) {
        const char* k = _fields[i].key;
        size_t j = 0;
        while (k[j] && key[j]) { if (k[j] != key[j]) goto next; ++j; }
        if (k[j] == key[j]) return (int8_t)i;
        next:;
    }
    return -1;
}

const ConfigField* ConfigService::findField(const char* key) const {
    int8_t i = findFieldIndex(key);
    return i >= 0 ? &_fields[i] : nullptr;
}

bool ConfigService::getBool(const char* key, bool def) const {
    int8_t i = findFieldIndex(key);
    if (i < 0) return def;
    const char* v = _values[i];
    return v[0] == '1' || v[0] == 't' || v[0] == 'T' ||
           (v[0] == 'o' && v[1] == 'n');   // "1","true","on"
}

int32_t ConfigService::getInt(const char* key, int32_t def) const {
    int8_t i = findFieldIndex(key);
    if (i < 0) return def;
    return (int32_t)atol(_values[i]);
}

uint32_t ConfigService::getUInt(const char* key, uint32_t def) const {
    int8_t i = findFieldIndex(key);
    if (i < 0) return def;
    long v = atol(_values[i]);
    return v < 0 ? def : (uint32_t)v;
}

float ConfigService::getFloat(const char* key, float def) const {
    int8_t i = findFieldIndex(key);
    if (i < 0) return def;
    return (float)atof(_values[i]);
}

void ConfigService::getStr(const char* key, char* buf, size_t bufSize,
                           const char* def) const {
    int8_t i = findFieldIndex(key);
    safeStrCopy(buf, bufSize, i >= 0 ? _values[i] : def);
}

// ============================================================================
// SET: валидация + событие + отложенная запись
// ============================================================================
bool ConfigService::validate(const ConfigField& f, const char* value) const {
    if (value == nullptr || value[0] == '\0') return false;
    switch (f.type) {
        case ConfigType::INT:
        case ConfigType::UINT: {
            // Только цифры (и '-' для INT)
            for (const char* p = value; *p; ++p) {
                if (*p == '-' && p == value && f.type == ConfigType::INT) continue;
                if (*p < '0' || *p > '9') return false;
            }
            long v = atol(value);
            if (f.min != 0 || f.max != 0) {   // диапазон задан
                if (v < f.min || v > f.max) return false;
            }
            return true;
        }
        case ConfigType::FLOAT: {
            char* end = nullptr;
            strtof(value, &end);
            return end != value;
        }
        case ConfigType::BOOL:
            return strlen(value) <= 5;   // "0","1","true","false","on","off"
        case ConfigType::IP: {
            // Пустое = «не задан» — легитимно: изолированная сеть без
            // шлюза/DNS (урок бенча 5.5.5: пустой net.gateway отклонялся,
            // конфигурацию «сети без выхода» было не сохранить).
            if (value[0] == '\0') return true;
            // Грубая проверка: 4 числа через точку (точную делает сеть)
            uint8_t dots = 0;
            for (const char* p = value; *p; ++p) if (*p == '.') dots++;
            return dots == 3;
        }
        case ConfigType::STRING:
        case ConfigType::SECRET:
            return strlen(value) < CFG_VALUE_LEN;
    }
    return false;
}

bool ConfigService::set(const char* key, const char* value) {
    int8_t i = findFieldIndex(key);
    if (i < 0) {
        log(LogLevel::Warning, "set: unknown key '%s'", key);
        return false;
    }
    const ConfigField& f = _fields[i];

    if (f.flags & CFG_READONLY) {
        log(LogLevel::Warning, "set: '%s' is readonly", key);
        return false;
    }
    if (!validate(f, value)) {
        log(LogLevel::Warning, "set: invalid value '%s' for '%s'", value, key);
        return false;
    }

    safeStrCopy(_values[i], CFG_VALUE_LEN, value);

    // SECRET — сразу в NVS (в JSON не попадают никогда)
    if (f.flags & CFG_SECRET) {
        Preferences prefs;
        if (prefs.begin(CFG_NVS_NS, false)) {
            prefs.putString(key, value);
            prefs.end();
        }
    } else {
        scheduleSave();
    }

    // Событие изменения: подписчики (NetworkManager на net.*, модули на
    // свои поля) применяют на лету; CFG_CRITICAL — пометят "нужен рестарт".
    ShEventData d; d.clear();
    safeStrCopy(d.payload, sizeof(d.payload), key);
    d.code = (f.flags & CFG_CRITICAL) ? 1 : 0;
    postEvent(CFG_EVENT_CHANGED, &d);
    return true;
}

bool ConfigService::setInternal(const char* key, const char* value) {
    int8_t i = findFieldIndex(key);
    if (i < 0) {
        log(LogLevel::Warning, "setInternal: unknown key '%s'", key);
        return false;
    }
    // READONLY намеренно обходим (это и есть смысл вызова), но тип/диапазон
    // проверяем так же строго — программная запись не повод для мусора.
    if (!validate(_fields[i], value)) {
        log(LogLevel::Warning, "setInternal: invalid '%s' for '%s'", value, key);
        return false;
    }
    safeStrCopy(_values[i], CFG_VALUE_LEN, value);
    scheduleSave();          // персистентность (монолит: cycle_count в NVS)
    // Событие НЕ издаём — см. контракт в .h (аудит не для счётчиков).
    return true;
}

void ConfigService::scheduleSave() {
    if (!_dirty) {
        _dirty = true;
        _dirtySinceMs = millis();
    }
}

// ============================================================================
// JSON: плоский {"key":"value",...} — ручной парсер/сериализатор
// ============================================================================
size_t ConfigService::snapshotJson(char* buf, size_t bufSize,
                                   bool withSecrets) const {
    size_t pos = 0;
    buf[pos++] = '{';
    bool first = true;
    for (uint8_t i = 0; i < _fieldCount; ++i) {
        // Секреты: в файл — никогда, в NVS-снимок — да (не покидает устройство)
        if ((_fields[i].flags & CFG_SECRET) && !withSecrets) continue;
        // READONLY — ХРАНИМ (урок 5.0.x: счётчик циклов замка писался только
        // в RAM и «считал и сбрасывался» на каждом ребуте; монолит держал
        // cycle_count в NVS). Износ флеша не аргумент: запись debounced
        // scheduleSave, счётчики меняются редко (цикл замка, не милисекунды).
        int n = snprintf(buf + pos, bufSize - pos, "%s\"%s\":\"%s\"",
                         first ? "" : ",", _fields[i].key, _values[i]);
        // Дисциплина буфера: при усечении snprintf возвращает ЖЕЛАЕМУЮ
        // длину (> остатка) — pos ушёл бы за границу. Откатываем
        // незавершённое поле: JSON остаётся валидным.
        if (n < 0 || (size_t)n >= bufSize - pos) {
            if (!first) buf[pos] = '\0';
            else pos = 1;
            break;
        }
        pos += (size_t)n;
        first = false;
    }
    if (pos < bufSize - 2) buf[pos++] = '}';
    buf[pos] = '\0';
    return pos;
}

// Буфер снимка — из КУЧИ на время операции. Два урока: (1) стек —
// цепочка HTTP-обработчик -> restoreFromNvs -> saveToJson вложила бы
// 2×5.5 КБ в loop-задачу (8 КБ) = переполнение; (2) статика — 11 КБ BSS
// не влезли в dram0_0_seg (урок линковки: сегмент не резиновый, запас
// был ~700 байт). Куча держит 80+ КБ свободными — транзиентным буферам туда.
constexpr size_t CFG_SNAPSHOT_CAP =
    CFG_MAX_FIELDS * (CFG_KEY_LEN + CFG_VALUE_LEN + 6);

void ConfigService::saveToJson() {
    char* buf = (char*)malloc(CFG_SNAPSHOT_CAP);
    if (buf == nullptr) {
        log(LogLevel::Error, "config save: no heap for snapshot");
        return;
    }
    snapshotJson(buf, CFG_SNAPSHOT_CAP, false);

    if (!StorageService::getInstance().atomicWrite(CFG_FILE_PATH, buf)) {
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), CFG_FILE_PATH);
        postEvent(CFG_EVENT_SAVE_FAILED, &d);
        log(LogLevel::Error, "config save failed");
    }
    free(buf);
}

// ============================================================================
// NVS-БЭКАП ПОЛНОГО СНИМКА (паттерн БД пользователей CardStore, 5.0.9)
// ============================================================================
// Зачем: перепрошивка FS стирает /config.json — оператор шёл по ВСЕМ полям
// и восстанавливал значения руками. NVS-раздел перепрошивкой FS/app не
// затрагивается: снимок переживает и новую FS, и смену версии. После
// перепрошивки: прошивка -> FS -> «Восстановить из NVS» -> ребут -> готово.
// ============================================================================
bool ConfigService::backupToNvs(uint32_t unixNow, const char* fwVer,
                                size_t* outSize) {
    char* buf = (char*)malloc(CFG_SNAPSHOT_CAP);
    if (buf == nullptr) {
        log(LogLevel::Error, "config backup: no heap for snapshot");
        return false;
    }
    size_t n = snapshotJson(buf, CFG_SNAPSHOT_CAP, true);  // ПОЛНЫЙ, с секретами

    StorageService& fs = StorageService::getInstance();
    if (!fs.nvsBackup(CFG_NVS_NS, CFG_BAK_KEY, buf, n)) {
        free(buf);
        log(LogLevel::Error, "config backup to NVS failed");
        return false;
    }
    free(buf);
    // Метаданные отдельным ключом: панель показывает «когда/сколько/какая
    // прошивка», не вычитывая весь снимок (он с секретами — не для HTTP).
    char inf[96];
    int m = snprintf(inf, sizeof(inf),
                     "{\"unix\":%lu,\"size\":%u,\"fw\":\"%s\"}",
                     (unsigned long)unixNow, (unsigned)n,
                     (fwVer != nullptr) ? fwVer : "?");
    if (m > 0) fs.nvsBackup(CFG_NVS_NS, CFG_BAK_INFO, inf, (size_t)m);

    if (outSize != nullptr) *outSize = n;
    log(LogLevel::Info, "config backup: %u bytes to NVS",
        (unsigned)n);
    return true;
}

// Общий парсер-применятор снимка (5.8.5: вынесен из restoreFromNvs —
// та же механика нужна /api/config/import для M3.3 BackupAggregator).
// Ручной парсер, как в loadFromJson: битый/чужой blob просто не даст
// совпадений — applied останется нулём и ничего не изменится.
// Неизвестные текущей схеме ключи (снимок с другой прошивки/профиля)
// пропускаются — как и при загрузке файла.
int ConfigService::applySnapshotJson(const char* json) {
    uint8_t applied = 0;
    const char* p = json;
    while ((p = strchr(p, '"')) != nullptr) {
        p++;
        const char* keyEnd = strchr(p, '"');
        if (keyEnd == nullptr) break;
        char key[CFG_KEY_LEN];
        size_t klen = (size_t)(keyEnd - p);
        if (klen >= sizeof(key)) { p = keyEnd + 1; continue; }
        memcpy(key, p, klen);
        key[klen] = '\0';

        p = keyEnd + 1;
        if (p[0] != ':' || p[1] != '"') continue;
        p += 2;
        const char* valEnd = strchr(p, '"');
        if (valEnd == nullptr) break;

        int8_t i = findFieldIndex(key);
        if (i >= 0) {
            size_t vlen = (size_t)(valEnd - p);
            if (vlen < CFG_VALUE_LEN) {
                memcpy(_values[i], p, vlen);
                _values[i][vlen] = '\0';
                if (_fields[i].flags & CFG_SECRET) {
                    // Секреты живут в NVS поштучно (как в set/addFields)
                    Preferences prefs;
                    if (prefs.begin(CFG_NVS_NS, false)) {
                        prefs.putString(key, _values[i]);
                        prefs.end();
                    }
                }
                applied++;
            }
        }
        p = valEnd + 1;
    }

    if (applied == 0) {
        log(LogLevel::Warning, "config apply: blob not recognized (0 fields)");
        return 0;
    }
    // НЕ scheduleSave: ребут запланирован через 1.5 с, дебаунс 1 с — гонка.
    // Пишем немедленно и атомарно.
    saveToJson();
    _dirty = false;
    return (int)applied;
}

int ConfigService::restoreFromNvs() {
    StorageService& fs = StorageService::getInstance();
    if (!fs.nvsExists(CFG_NVS_NS, CFG_BAK_KEY)) return -1;

    uint8_t* buf = (uint8_t*)malloc(CFG_SNAPSHOT_CAP + 1);
    if (buf == nullptr) {
        log(LogLevel::Error, "config restore: no heap for snapshot");
        return -1;
    }
    size_t n = fs.nvsRestore(CFG_NVS_NS, CFG_BAK_KEY, buf, CFG_SNAPSHOT_CAP);
    if (n == 0) { free(buf); return -1; }
    buf[n] = '\0';

    // applySnapshotJson сам делает saveToJson (тот аллоцирует свой буфер),
    // поэтому пик транзиентной кучи — два снимка на ~10 мс. Принято:
    // куча держит 80+ КБ, а альтернатива (освободить buf до apply) —
    // невозможна: парсинг идёт по живому буферу, копий apply не делает.
    int applied = applySnapshotJson((const char*)buf);
    free(buf);
    if (applied > 0) {
        log(LogLevel::Info, "config restored from NVS: %u fields", applied);
    }
    return applied;
}

size_t ConfigService::backupInfoJson(char* buf, size_t bufSize) const {
    char inf[96];
    size_t n = StorageService::getInstance().nvsRestore(
        CFG_NVS_NS, CFG_BAK_INFO, inf, sizeof(inf) - 1);
    if (n == 0) {
        return (size_t)snprintf(buf, bufSize, "{\"exists\":0}");
    }
    inf[n] = '\0';
    // inf — свой компактный JSON {"unix":...}: подмешиваем exists:1,
    // не парясь пересборкой (inf[0] == '{' по построению backupToNvs).
    if (inf[0] != '{') return (size_t)snprintf(buf, bufSize, "{\"exists\":0}");
    return (size_t)snprintf(buf, bufSize, "{\"exists\":1,%s", inf + 1);
}

void ConfigService::loadFromJson() {
    // Буфер ~5,5 КБ — ТОЛЬКО в куче, не на стеке: loopTask имеет 8 КБ,
    // а цепочка VFS→LittleFS→flash на S3 заметно глубже, чем на классике
    // (паника «stack canary watchpoint (loopTask)» на стенде M0, 06.08.2026).
    constexpr size_t BUF_SZ = CFG_MAX_FIELDS * (CFG_KEY_LEN + CFG_VALUE_LEN + 6);
    uint8_t* buf = (uint8_t*)malloc(BUF_SZ);
    if (!buf) {
        log(LogLevel::Error, "config load: no heap, defaults in use");
        return;
    }
    size_t n = StorageService::getInstance().readFile(CFG_FILE_PATH, buf,
                                                      BUF_SZ - 1);
    if (n == 0) {
        log(LogLevel::Info, "no config file, defaults in use");
        free(buf);
        return;
    }
    buf[n] = '\0';

    // Ручной парсер: ищем "key":"value" последовательно. Битый JSON
    // просто не даст совпадений — поля останутся в умолчаниях.
    const char* p = (const char*)buf;
    while ((p = strchr(p, '"')) != nullptr) {
        p++;
        const char* keyEnd = strchr(p, '"');
        if (keyEnd == nullptr) break;
        char key[CFG_KEY_LEN];
        size_t klen = keyEnd - p;
        if (klen >= sizeof(key)) { p = keyEnd + 1; continue; }
        memcpy(key, p, klen);
        key[klen] = '\0';

        p = keyEnd + 1;
        if (p[0] != ':' || p[1] != '"') { continue; }
        p += 2;
        const char* valEnd = strchr(p, '"');
        if (valEnd == nullptr) break;

        int8_t i = findFieldIndex(key);
        if (i >= 0 && !(_fields[i].flags & CFG_SECRET)) {
            size_t vlen = valEnd - p;
            if (vlen < CFG_VALUE_LEN) {
                memcpy(_values[i], p, vlen);
                _values[i][vlen] = '\0';
            }
        }
        p = valEnd + 1;
    }
    log(LogLevel::Info, "config loaded from %s", CFG_FILE_PATH);
    free(buf);
}

// ============================================================================
// МИГРАЦИЯ NVS v2.5.0 -> 5.0 (один раз, флаг в NVS)
// ============================================================================
// Таблица — из SmartLockConfig.cpp (колонка NVS монолита). Системные ключи
// (hostname, net.*, mqtt.*) мигрируют в ядерные поля, которые регистрирует
// BaseProfile/NetworkManager (Phase 2b) — если поле ещё не зарегистрировано,
// миграция его пропускает (повторный запуск не нужен: флаг ставится после
// полного прохода, системные поля будут зарегистрированы ядром раньше
// профилей — порядок фаз это гарантирует).
// ============================================================================
void ConfigService::migrateFromLegacyNvs() {
    Preferences marker;
    if (!marker.begin(CFG_NVS_NS, false)) return;
    bool done = marker.getBool("mig_250", false);
    if (done) { marker.end(); return; }

    Preferences old;
    if (!old.begin("lock_cfg", true)) {   // namespace монолита, read-only
        marker.putBool("mig_250", true);  // монолита не было — метим и уходим
        marker.end();
        return;
    }

    struct MigEntry { const char* oldKey; const char* newKey; bool isBool; };
    static const MigEntry TABLE[] = {
        { "l_time",        "lock.open_ms",              false },
        { "l_type",        "lock.fail_secure",          true  },
        { "dr_alarm_m",    "lock.door_alarm_min",       false },
        { "a_vol",         "lock.audio_volume",         false },
        { "quiet",         "lock.quiet_mode",           true  },
        { "exit_restrict", "lock.exit_restrict",        true  },
        { "ex_restr_act",  "lock.exit_restrict_active", true  },
        { "cycle_count",   "lock.cycle_count",          false },
        { "hostname",      "sys.hostname",              false },
        { "net_dhcp",      "net.dhcp",                  true  },
        { "cfg_ip",        "net.ip",                    false },
        { "cfg_mask",      "net.mask",                  false },
        { "cfg_gateway",   "net.gateway",               false },
        { "cfg_dns",       "net.dns",                   false },
        { "mq_ip",         "mqtt.host",                 false },
        { "mq_u",          "mqtt.user",                 false },
        { "mq_p",          "mqtt.pass",                 false },
        { "use_mqtt",      "mqtt.enabled",              true  },
    };

    uint8_t migrated = 0;
    for (const auto& e : TABLE) {
        int8_t idx = findFieldIndex(e.newKey);
        if (idx < 0) continue;   // поле ещё не зарегистрировано — пропуск

        char val[CFG_VALUE_LEN];
        if (e.isBool) {
            snprintf(val, sizeof(val), "%d", old.getBool(e.oldKey, false) ? 1 : 0);
        } else {
            // В монолите числа — uInt, строки — String
            if (strcmp(e.oldKey, "l_time") == 0 ||
                strcmp(e.oldKey, "dr_alarm_m") == 0 ||
                strcmp(e.oldKey, "a_vol") == 0 ||
                strcmp(e.oldKey, "cycle_count") == 0) {
                snprintf(val, sizeof(val), "%lu",
                         (unsigned long)old.getUInt(e.oldKey, 0));
            } else {
                String s = old.getString(e.oldKey, "");
                safeStrCopy(val, sizeof(val), s.c_str());
            }
        }
        if (val[0] != '\0') {
            safeStrCopy(_values[idx], CFG_VALUE_LEN, val);
            migrated++;
        }
    }

    // Расписание кнопки: часы/минуты -> "HH:MM"
    int8_t idxSt = findFieldIndex("lock.exit_restrict_start");
    int8_t idxEn = findFieldIndex("lock.exit_restrict_end");
    if (idxSt >= 0 && idxEn >= 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02u:%02u",
                 old.getUChar("restr_st_h", 22), old.getUChar("restr_st_m", 0));
        safeStrCopy(_values[idxSt], CFG_VALUE_LEN, buf);
        snprintf(buf, sizeof(buf), "%02u:%02u",
                 old.getUChar("restr_en_h", 6), old.getUChar("restr_en_m", 0));
        safeStrCopy(_values[idxEn], CFG_VALUE_LEN, buf);
    }
    old.end();

    marker.putBool("mig_250", true);
    marker.end();

    if (migrated > 0) {
        scheduleSave();
        ShEventData d; d.clear();
        d.code = migrated;
        postEvent(CFG_EVENT_MIGRATED, &d);
        log(LogLevel::Info, "migrated %u values from NVS v2.5.0", migrated);
    }
}

// ============================================================================
// СЕРИАЛИЗАЦИЯ ДЛЯ API/UI (без SECRET)
// ============================================================================
size_t ConfigService::toJson(char* buf, size_t bufSize) const {
    size_t pos = 0;
    buf[pos++] = '{';
    bool first = true;
    for (uint8_t i = 0; i < _fieldCount; ++i) {
        // SECRET (mqtt.pass и др.): в схему включаем, но ЗНАЧЕНИЕ — никогда.
        // value всегда "", set — задан ли секрет (для placeholder'а в UI),
        // secret:1 — UI рисует поле ввода пароля (write-only). Запись идёт
        // штатным POST /api/config — set() секреты кладёт сразу в NVS.
        const bool sec = (_fields[i].flags & CFG_SECRET) != 0;
        // t: тип (0=INT,1=UINT,2=FLOAT,3=BOOL,4=STRING,5=IP) — авто-UI
        // строит виджет по типу; min/max — клиентская валидация чисел.
        // 5.8.0: hg:1 — группа помечена профилем служебной (аккордеон
        // «Служебные» в панели; значение/запись поля не меняются).
        char extra[40]; extra[0] = '\0';
        if (groupHidden(_fields[i].group)) strcat(extra, ",\"hg\":1");
        if (sec) strcat(extra, _values[i][0] ? ",\"secret\":1,\"set\":1"
                                             : ",\"secret\":1,\"set\":0");
        int n = snprintf(buf + pos, bufSize - pos,
                         "%s\"%s\":{\"value\":\"%s\",\"group\":\"%s\","
                         "\"label\":\"%s\",\"ro\":%d,\"t\":%d,"
                         "\"min\":%ld,\"max\":%ld%s}",
                         first ? "" : ",", _fields[i].key,
                         sec ? "" : _values[i],
                         _fields[i].group, _fields[i].label,
                         (_fields[i].flags & CFG_READONLY) ? 1 : 0,
                         (int)_fields[i].type,
                         (long)_fields[i].min, (long)_fields[i].max,
                         extra);
        // Дисциплина буфера: при усечении snprintf возвращает ЖЕЛАЕМУЮ
        // длину (> остатка) — pos ушёл бы за границу. Прерываем цикл,
        // откатив незавершённое поле: JSON останется валидным.
        // 5.1.1: усечение — ГРОМКОЕ. Молчаливый обрыв 5.1.0 съел хвост
        // схемы (61 поле > буфер 8 КБ), панель потеряла 6 полей профиля,
        // и никто ничего не заподозрил: JSON-то валидный. Error в лог —
        // обязателен, чтобы следующий рост схемы увидели сразу.
        if (n < 0 || (size_t)n >= bufSize - pos) {
            if (!first) buf[pos] = '\0';   // отрезать ",начало_поля"
            else pos = 1;                  // вообще ничего не влезло
            log(LogLevel::Error,
                "toJson: SCHEMA TRUNCATED at field %u/%u (%s): buf %u B small",
                (unsigned)i, (unsigned)_fieldCount, _fields[i].key,
                (unsigned)bufSize);
            break;
        }
        pos += (size_t)n;
        first = false;
    }
    if (pos < bufSize - 2) buf[pos++] = '}';
    buf[pos] = '\0';
    return pos;
}
