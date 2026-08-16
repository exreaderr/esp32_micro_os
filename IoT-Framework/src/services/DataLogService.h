// ============================================================================
// DataLogService.h — ДАТАЛОГГЕР (ядерный сервис временных рядов, п.5)
// ============================================================================
// Наследник идей DataLoggerManager мёртвой ветки (см. DataLogCore.h — там
// чистая логика и разбор отличий). Сервис — тонкая обёртка вокруг ядра:
//   · каналы регистрирует ПРОФИЛЬ (smart_lock: cpu_t, heap, wx_t; завтра
//     контроллер света — lux/power): сервис пассивен, магии нет;
//   · RAM-кольца сырых точек + append-only файлы ярусов hour/day в LittleFS;
//   · время — TimeService: точка с недостоверным временем ОТБРАСЫВАЕТСЯ
//     (урок 5.0.x: данные без времени — мусор, а не данные);
//   · потокобезопасность: один мьютекс, короткие критические секции;
//     писать могут и tick профиля, и MQTT-колбэк (погода), читать — HTTP.
//
// Flash-бережливость: в час на канал — одна 16-байтовая запись (append) +
// обновление заголовка; компакция яруса ~раз в месяц/год. Износ ничтожен.
// Потеря при обесточке: открытое ведро (недописанный час) — приемлемо,
// задокументировано; журналы — не критичные данные (тот же принцип, что
// у AuditService.appendFile).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "DataLogCore.h"

// Пути (LittleFS): /datalog/H_<id>.bin, /datalog/D_<id>.bin
constexpr const char* DLOG_DIR = "/datalog";

class DataLogService : public ModuleBase {
public:
    static DataLogService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "DataLogService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x000E; }   // ядро

    void registerExtensions() override;   // datalog.enabled
    void init() override;                 // mutex + mkdir /datalog
    void start() override {}
    void stop() override;                 // честное закрытие вёдер -> файлы
    void tick() override;                 // сброс «молчащих» вёдер прошлого
    uint32_t getTickIntervalMs() const override { return 60000; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- КАНАЛЫ (регистрирует профиль) --------------------------------------
    /// Объявить канал. id — латиница до 11 символов (имя файла!), name —
    /// русская подпись для панели, unit — "°C"/"КБ"/... Возврат: индекс
    /// 0..7 или -1 (нет места / битый id / дубликат).
    int8_t registerChannel(const char* id, const char* name, const char* unit);
    uint8_t channelCount() const { return _count; }
    /// Подпись канала для UI. false — нет такого индекса.
    bool channelInfo(uint8_t ch, char* idOut, size_t idSz,
                     char* nameOut, size_t nameSz,
                     char* unitOut, size_t unitSz) const;

    // --- ЗАПИСЬ -------------------------------------------------------------
    /// Точка с текущим временем RTC. false — канал/время недостоверно
    /// (счётчик dropped). Безопасна из любой задачи (мьютекс внутри).
    /// NB: имя logPoint, а НЕ log — иначе перекрывает ModuleBase::log.
    bool logPoint(int8_t ch, float v);

    // --- ЧТЕНИЕ (для UI) ------------------------------------------------------
    /// Сырые точки ts >= fromTs (0 — всё кольцо), хронологически.
    uint16_t getRaw(uint8_t ch, DlogPoint* out, uint16_t maxN, uint32_t fromTs);
    /// Агрегаты яруса (daily=false — часовой, true — суточный), ts >= fromTs.
    /// Читает файл яруса + доклеивает открытое ведро (свежие данные до
    /// закрытия часа видны на графике сразу).
    uint16_t getTier(uint8_t ch, bool daily, DlogAggr* out, uint16_t maxN,
                     uint32_t fromTs);

    // --- ДИАГНОСТИКА ------------------------------------------------------------
    uint32_t droppedPoints(uint8_t ch) const;

private:
    DataLogService() = default;

    struct Channel {
        char        id[12];
        char        name[28];
        char        unit[8];
        dlog::Ring  ring;             // 2.9 КБ — сырые точки (6 ч)
        dlog::Bucket hour;            // открытое часовое ведро
        dlog::Bucket day;             // открытое суточное ведро
        uint32_t    dropped = 0;      // отброшено (нет RTC / нет мьютекса)
    };

    bool validId(const char* id) const;   // [a-z0-9_], 1..11 — имя файла
    void tierPaths(uint8_t ch, char* hourOut, char* dayOut) const;
    /// Append записи в файл яруса (+ компакция при переполнении). Мьютекс
    /// УЖЕ взят вызывающим.
    bool appendTier(const char* path, const DlogAggr& rec, uint16_t cap);
    /// Закрыть вёдра, чей период истёк (tick) или все (stop). nowTs — unix.
    void flushBuckets(uint32_t nowTs, bool all);

    // Каналы — РАЗОВАЯ аллокация в init() (не BSS!). Постмортем 5.0.x:
    // 8 колец по 2.9 КБ в BSS = +24 КБ статики -> DRAM-переполнение при
    // линковке (region dram0_0_seg overflowed). Мьяния: boot-time calloc
    // без единого free — churn отсутствует, фрагментации не возникает,
    // а линкер статику не видит. Провалилась аллокация — сервис деградирует
    // в «нет каналов», система живёт дальше (даталоггер — не критичный).
    Channel* _ch = nullptr;
    uint8_t  _count = 0;
    SemaphoreHandle_t _mtx = nullptr;
};
