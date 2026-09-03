// ============================================================================
// CardStore.h — БАЗА КАРТ СКУД (профильный модуль, владелец users.json)
// ============================================================================
// Перенос UserManager-функциональности монолита v2.5.0 на механику 5.0:
//   · RAM-кэш фиксированного размера (SL_MAX_USERS, без std::vector/String —
//     урок фрагментации heap);
//   · файл users.json в формате монолита (CardDbFormat.h — чистая логика,
//     host-тесты D2); запись атомарная через StorageService;
//   · порча файла -> восстановление из users.json.bak -> событие ПАЗ;
//   · NVS-зеркало (монолит: namespace "db_backup", ключ "db_raw") —
//     последний рубеж после гибели файловой системы;
//   · авто-бэкап раз в сутки (монолит: checkLittleFSHealth).
//
// Политика доступа (режимы, мастер-цикл, вердикты) — НЕ здесь, а в
// SmartLockApp. CardStore отвечает на вопросы «кто эта карта» и хранит.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include "CardDbFormat.h"

// Бюджеты. 5.2.0: ёмкость 100 -> 250 (малый офис, запрос пользователя).
// Рост без перепроектирования: uint8_t count/idx держат до 255, файл и
// панель (страницы по 25 + поиск q) не ограничители. Буфер на всю базу
// (55 КБ) в RAM НЕ ПОДНЯЛСЯ бы (живых ~69 КБ) — поэтому save/load
// ПОТОКОВЫЕ по записи (serializeOne/parseOne, чистая логика), а постоянный
// буфер сериализации упразднён совсем. В heap остаётся только кэш 25 КБ.
constexpr uint8_t  SL_MAX_USERS        = 250;   // записей в RAM-кэше (heap)
// Запасной режим при невозможности heap-аллокации (не случится при ~230 КБ
// свободных, но модуль обязан жить): статический карман на 32 записи.
constexpr uint8_t  SL_FALLBACK_USERS   = 32;
// Потолок NVS-зеркала: раздел NVS 20 КБ делят конфиг и бэкапы — база
// крупнее ~75 «тяжёлых» записей зеркалу не по силам. Это НЕ сбой, а
// честная политика: рубежи FS (users.json + .bak) остаются, зеркало
// пропускается с Info-логом вместо гулкого Error при каждом сохранении.
constexpr size_t   SL_NVS_MIRROR_MAX   = 16384;
// Временный буфер зеркала (редкие операции backup/restore — alloc/free
// на время действия, не постоянный расход): serialize сюда, дальше
// политика потолка выше.
constexpr size_t   SL_NVS_TMP_SIZE     = 17408;
constexpr uint32_t SL_BACKUP_PERIOD_MS = 24UL * 60 * 60 * 1000;  // сутки

constexpr const char* SL_DB_PATH      = "/users.json";
constexpr const char* SL_DB_NVS_NS    = "db_backup";   // как в монолите
constexpr const char* SL_DB_NVS_KEY   = "db_raw";      // как в монолите
// 5.5.13: мета зеркала (возраст + fw) — ОТДЕЛЬНЫМ ключом, формат db_raw
// не трогаем (монолит-совместимость свята). {"u":unix,"fw":"x.y.z"}
constexpr const char* SL_DB_NVS_META  = "db_meta";

class CardStore : public ModuleBase {
public:
    static CardStore& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "CardStore"; }
    const char* getVersion() const override { return "5.2.1"; }   // 5.2.1: автозапечатывание зеркала + мета возраста (5.5.13)
    ModuleId getModuleId() const override { return 0x1001; }   // профиль

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;                 // авто-бэкап раз в сутки
    uint32_t getTickIntervalMs() const override { return 60000; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- ЗАПРОСЫ (горячий путь — скан карты) --------------------------------
    /// Поиск по ID (HEX, регистр неважен — нормализуется). nullptr — нет.
    const SlUser* find(const char* id) const;
    uint8_t count() const { return _count; }
    /// Реальная ёмкость: 250 при heap-кэше, 32 в запасном режиме.
    uint8_t capacity() const { return _users ? SL_MAX_USERS : SL_FALLBACK_USERS; }
    bool isFull() const { return _count >= capacity(); }
    const SlUser* at(uint8_t index) const {
        return index < _count ? &users()[index] : nullptr;
    }
    bool masterExists() const;

    /// Поиск жильца по веб-ПИНу (вход в панель). ПИН — идентичность:
    /// по нему контроллер знает ИМЯ (приветствие, аудит). nullptr — нет.
    const SlUser* findByPin(const char* pin) const;

    /// ПИН занят другой записью (exceptId — не считать самого себя).
    /// ПИНи СТРОГО уникальны (монолит: единственный контроллер с жильцами).
    bool pinTaken(const char* pin, const char* exceptId) const;

    // --- МУТАЦИИ (редкие, каждая — с атомарным сохранением) ------------------
    /// Добавить карту. Имя санитизируется, ID нормализуется.
    /// false — база полна / ID мусор / дубликат / ПИН занят или битый.
    bool add(const char* id, const char* name, KeyType type,
             uint8_t track, uint32_t expiry, const char* pin = "");
    bool remove(const char* id);              // false — нет такой
    /// Авто-чистка (решение владельца 03.09.2026): удалить просроченные
    /// TEMPORARY (expiry>0 и expiry<now). Вызывать ТОЛЬКО при достоверном
    /// времени (Fail-Safe: now==0 — ничего не делаем). Каждое удаление —
    /// событие cardRemoved + строка в журнале (имя/HEX). Возврат — число.
    uint8_t purgeExpired(uint32_t now);
    /// Сентинелы «не менять» для update(): форма редактирования шлёт
    /// ТОЛЬКО реально изменённые поля — пустое поле ≠ «стереть» (5.0.13:
    /// раньше отсутствующие name/track/expiry затирали запись нулями).
    static constexpr uint8_t  TRACK_KEEP  = 0xFF;
    static constexpr uint32_t EXPIRY_KEEP = 0xFFFFFFFF;
    /// name == nullptr — не менять имя; track == TRACK_KEEP — не менять;
    /// expiry == EXPIRY_KEEP — не менять (явный 0 = «постоянный»);
    /// pin == nullptr — не менять ПИН; "" — снять веб-доступ; 4..6 цифр —
    /// задать (с проверкой уникальности). Тип не меняем (безопасность).
    bool update(const char* id, const char* name, uint8_t track,
                uint32_t expiry, const char* pin = nullptr);
    /// Факт прохода: uses++, lastUse=unixTime (0 — время недостоверно,
    /// lastUse не трогаем). Best-effort: сбой сохранения НЕ отменяет уже
    /// выданный доступ (вызывающий игнорирует false), в RAM счётчик поднят.
    bool recordUse(const char* id, uint32_t unixTime);
    /// Блокировка без удаления (мёртвая ветка: blockUser/unblockUser) —
    /// потерянная карта гасится, имя/статистика остаются для разбора.
    bool setBlocked(const char* id, bool blocked);   // false — нет такой
    void clear();                             // полная очистка (бэкап перед!)

    // --- СТОЙКОСТЬ -------------------------------------------------------------
    bool save();            // atomicWrite users.json
    bool load();            // при старте; порча -> .bak -> NVS-зеркало
    bool backupNow();       // users.json -> users.json.bak
    bool nvsBackupNow();    // JSON -> NVS (монолит: «запечатать в стенд»).
                            // 5.2.0: база > SL_NVS_MIRROR_MAX — зеркало
                            // ПРОПУСКАЕТСЯ (true + mirrorSkipped), это
                            // политика, а не сбой: рубежи FS на месте.
    bool mirrorSkipped() const { return _mirrorSkipped; }
    bool nvsRestoreNow();   // NVS -> JSON + RAM (когда FS мертва)
    bool hasNvsBackup();    // в NVS есть непустое зеркало (для модалки
                            // «база пуста — восстановить?» из монолита)
    /// 5.5.13: возраст зеркала. false — меты нет (зеркало старого формата,
    /// запечатано до 5.5.13 или монолитом): панель обязана показать
    /// «возраст неизвестен», а не молчать (инцидент 12.08: оператор
    /// восстановил СТАРОЕ зеркало и потерял тип мастер-ключа).
    bool nvsMirrorInfo(uint32_t& unixOut, char* fwOut, size_t fwSize);

    // Диагностика (API/health)
    uint32_t loadErrors() const { return _loadErrors; }
    uint32_t saveCount() const { return _saveCount; }
    bool lastLoadFromBackup() const { return _loadedFromBackup; }

private:
    CardStore() = default;

    int findIndex(const char* normalizedId) const;
    void postCardEvent(int32_t eventId, const char* id, int32_t code);
    /// Потоковая загрузка текущего users.json: записей прочитано (>= 0)
    /// или -1 (порча). Файл читается ПО ОБЪЕКТАМ (скобочный сканер +
    /// parseOne), целиком в RAM не поднимается (5.2.0, база 250).
    int loadFromFile();

    // 5.2.0: кэш — HEAP-указатель (один блок 25 КБ из init(), nothrow —
    // уроки outbox и HTTP_JSON_BUF). nullptr = аллокация не удалась ->
    // статический карман на 32 записи: модуль живёт, ёмкость честно
    // уменьшена (capacity() это отражает). Буфера сериализации на всю базу
    // больше НЕТ: save/load потоковые по записи (см. .cpp).
    SlUser*  _users = nullptr;
    SlUser   _usersFb[SL_FALLBACK_USERS];
    SlUser*  users()        { return _users ? _users : _usersFb; }
    const SlUser* users() const { return _users ? _users : _usersFb; }
    uint8_t  _count = 0;

    uint32_t _lastBackupMs = 0;
    uint32_t _loadErrors = 0;
    uint32_t _saveCount = 0;
    bool     _loadedFromBackup = false;
    bool     _mirrorSkipped = false;   // зеркало не по силам (база крупная)
};
