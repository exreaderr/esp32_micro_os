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
void ConfigService::saveToJson() {
    char buf[CFG_MAX_FIELDS * (CFG_KEY_LEN + CFG_VALUE_LEN + 6)];
    size_t pos = 0;
    buf[pos++] = '{';
    bool first = true;
    for (uint8_t i = 0; i < _fieldCount; ++i) {
        if (_fields[i].flags & CFG_SECRET) continue;   // секреты — не в JSON
        // READONLY — ХРАНИМ (урок 5.0.x: счётчик циклов замка писался только
        // в RAM и «считал и сбрасывался» на каждом ребуте; монолит держал
        // cycle_count в NVS). Износ флеша не аргумент: запись debounced
        // scheduleSave, счётчики меняются редко (цикл замка, не милисекунды).
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s\"%s\":\"%s\"",
                         first ? "" : ",", _fields[i].key, _values[i]);
        if (n < 0) break;
        pos += (size_t)n;
        first = false;
    }
    if (pos < sizeof(buf) - 2) buf[pos++] = '}';
    buf[pos] = '\0';

    if (!StorageService::getInstance().atomicWrite(CFG_FILE_PATH, buf)) {
        ShEventData d; d.clear();
        safeStrCopy(d.payload, sizeof(d.payload), CFG_FILE_PATH);
        postEvent(CFG_EVENT_SAVE_FAILED, &d);
        log(LogLevel::Error, "config save failed");
    }
}

void ConfigService::loadFromJson() {
    uint8_t buf[CFG_MAX_FIELDS * (CFG_KEY_LEN + CFG_VALUE_LEN + 6)];
    size_t n = StorageService::getInstance().readFile(CFG_FILE_PATH, buf,
                                                      sizeof(buf) - 1);
    if (n == 0) {
        log(LogLevel::Info, "no config file, defaults in use");
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
                         sec ? (_values[i][0] ? ",\"secret\":1,\"set\":1"
                                              : ",\"secret\":1,\"set\":0")
                             : "");
        // Дисциплина буфера: при усечении snprintf возвращает ЖЕЛАЕМУЮ
        // длину (> остатка) — pos ушёл бы за границу. Прерываем цикл,
        // откатив незавершённое поле: JSON останется валидным.
        if (n < 0 || (size_t)n >= bufSize - pos) {
            if (!first) buf[pos] = '\0';   // отрезать ",начало_поля"
            else pos = 1;                  // вообще ничего не влезло
            break;
        }
        pos += (size_t)n;
        first = false;
    }
    if (pos < bufSize - 2) buf[pos++] = '}';
    buf[pos] = '\0';
    return pos;
}
