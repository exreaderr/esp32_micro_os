// ============================================================================
// ConfigService.h — КОНФИГУРАЦИЯ С ИНЖЕКЦИЕЙ СХЕМ (точка расширения)
// ============================================================================
// Фаза 2. Наследник ConfigManager v4.2.2 МИНУС сетевая логика
// (applyNetworkSettings ушёл в NetworkManager, Phase 2b) ПЛЮС главная
// новая возможность 5.0 — декларативные схемы полей от модулей и профилей.
//
// Модель (базовая архитектура, 8.3):
//   · модуль объявляет свои поля через addFields() в registerExtensions —
//     ядро о конкретных полях не знает;
//   · ключи неймспейсированы: "lock.open_ms", "net.dhcp" — коллизии
//     исключены по построению;
//   · хранение: JSON в LittleFS через StorageService (атомарно), секреты —
//     в NVS (auth.* не попадают в JSON и не отдаются наружу);
//   · изменение поля -> валидация типа/диапазона -> CFG_EVENT_CHANGED(key);
//   · веб-UI строится из схем автоматически (Фаза 3, IUiProvider);
//   · миграция NVS монолита v2.5.0 — один раз при первом старте.
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"

// ============================================================================
// ТИПЫ И ФЛАГИ ПОЛЕЙ
// ============================================================================
enum class ConfigType : uint8_t { INT, UINT, FLOAT, BOOL, STRING, IP, SECRET };

enum ConfigFlags : uint8_t {
    CFG_NONE     = 0,
    CFG_CRITICAL = 1 << 0,   // изменение требует перезагрузки/переприменения
    CFG_READONLY = 1 << 1,   // не изменяется через API (счётчики и т.п.)
    CFG_SECRET   = 1 << 2    // не отдаётся в API/UI в открытом виде (NVS)
};

struct ConfigField {
    const char* key;         // "lock.open_ms" — неймспейс = модуль
    ConfigType  type;
    const char* defValue;    // умолчание строкой (парсится по типу)
    int32_t     min, max;    // для числовых типов
    uint8_t     flags;
    const char* group;       // "Замок" — группа в веб-UI
    const char* label;       // "Длительность импульса, мс"
};

// Бюджеты
constexpr uint8_t  CFG_MAX_FIELDS     = 64;   // полей в системе
constexpr uint8_t  CFG_KEY_LEN        = 32;   // длина ключа
constexpr uint8_t  CFG_VALUE_LEN      = 48;   // длина значения (строкой)
constexpr uint32_t CFG_SAVE_DEBOUNCE_MS = 1000; // отложенная запись JSON
constexpr const char* CFG_FILE_PATH   = "/config.json";
constexpr const char* CFG_NVS_NS      = "config";   // секреты и флаги

class ConfigService : public ModuleBase {
public:
    static ConfigService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "ConfigService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0002; }

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 250; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- РЕГИСТРАЦИЯ СХЕМ (вызывается из registerExtensions модулей) -----------
    /// Добавить пакет полей одной группы. group — имя UI-группы.
    bool addFields(const char* group, const ConfigField* fields, uint8_t count);
    /// Удобная обёртка для инициализатора-списка.
    bool addFields(const char* group, std::initializer_list<ConfigField> fields) {
        return addFields(group, fields.begin(), (uint8_t)fields.size());
    }

    // --- ДОСТУП К ЗНАЧЕНИЯМ (типизированный) --------------------------------------
    bool        getBool (const char* key, bool def = false) const;
    int32_t     getInt  (const char* key, int32_t def = 0) const;
    uint32_t    getUInt (const char* key, uint32_t def = 0) const;
    float       getFloat(const char* key, float def = 0.0f) const;
    /// Строковое значение в буфер вызывающего (без String!).
    void        getStr  (const char* key, char* buf, size_t bufSize,
                         const char* def = "") const;

    /// Установить значение (валидация типа/диапазона по схеме).
    /// false — ключ неизвестен, readonly, значение вне диапазона.
    bool set(const char* key, const char* value);

    /// Программная запись в READONLY-поле (счётчики ресурса, служебные
    /// метрики — напр. lock.cycle_count). Валидация типа СОХРАНЯЕТСЯ,
    /// запись персистентна (scheduleSave), но событие CFG_EVENT_CHANGED
    /// НЕ издаётся: счётчик циклов — не изменение конфигурации оператором,
    /// иначе каждый проход засорял бы аудит (CFG_CHANGED аудируется, B3).
    bool setInternal(const char* key, const char* value);

    /// Есть ли поле в схеме.
    bool hasField(const char* key) const { return findField(key) != nullptr; }

    // --- ДЛЯ ВЕБ-UI/API (Фаза 3) ---------------------------------------------------
    uint8_t fieldCount() const { return _fieldCount; }
    const ConfigField* fieldAt(uint8_t i) const {
        return i < _fieldCount ? &_fields[i] : nullptr;
    }
    /// Сериализация всех значений в JSON (без SECRET-полей) для API/UI.
    size_t toJson(char* buf, size_t bufSize) const;

private:
    ConfigService() = default;

    // --- ХРАНЕНИЕ ------------------------------------------------------------
    void loadFromJson();          // чтение /config.json при init
    void scheduleSave();          // запрос отложенной записи
    void saveToJson();            // атомарная запись через StorageService
    void migrateFromLegacyNvs();  // миграция NVS v2.5.0 -> поля 5.0

    // --- ПОИСК/ВАЛИДАЦИЯ ---------------------------------------------------------
    const ConfigField* findField(const char* key) const;
    int8_t findFieldIndex(const char* key) const;
    bool validate(const ConfigField& f, const char* value) const;

    // --- ДАННЫЕ -------------------------------------------------------------------
    // Схема: указатели на строки регистранта (живут вечно — PROGMEM/статика)
    ConfigField _fields[CFG_MAX_FIELDS];
    uint8_t     _fieldCount = 0;

    // Текущие значения (все — строками; тип применяется при get/set)
    char _values[CFG_MAX_FIELDS][CFG_VALUE_LEN];

    bool     _dirty = false;          // есть несохранённые изменения
    uint32_t _dirtySinceMs = 0;       // дебаунс записи
};

// ============================================================================
// УДОБНЫЕ ГЛОБАЛЬНЫЕ ОБЁРТКИ (используются в SmartLockApp и профилях)
// ============================================================================
inline bool     cfgGetBool (const char* k, bool d = false) {
    return ConfigService::getInstance().getBool(k, d);
}
inline uint32_t cfgGetUInt (const char* k, uint32_t d = 0) {
    return ConfigService::getInstance().getUInt(k, d);
}
inline int32_t  cfgGetInt  (const char* k, int32_t d = 0) {
    return ConfigService::getInstance().getInt(k, d);
}
inline float    cfgGetFloat(const char* k, float d = 0.0f) {
    return ConfigService::getInstance().getFloat(k, d);
}
inline void     cfgGetStr  (const char* k, char* buf, size_t n,
                            const char* d = "") {
    ConfigService::getInstance().getStr(k, buf, n, d);
}
