// ============================================================================
// JournalService.h — ДОЛГИЙ ЖУРНАЛ ПАРКА НА SD (M3.1 «стержень»)
// ============================================================================
// Концепция §4.2 (EventJournal): JSONL на SD, ротация по размеру/возрасту,
// честная деградация без карты. Чистая логика — в JournalCore.h (host-
// тесты); здесь — обвязка: хук брокера, кольцо, файлы, расписание сброса.
//
// Поток: PUBLISH клиента -> BrokerService::fireHooks (контекст tick,
// СИНХРОННО) -> formatLine -> RAM-кольцо. tick() раз в journal.flush_s
// сбрасывает кольцо пачкой в текущий сегмент + flush(). SD в хуке НЕ
// трогаем никогда.
//
// FAT-кондиции (концепция §5, «SD — расходник»):
//   · батч-сброс + flush на каждой пачке;
//   · boot-check: хвост сегмента порван (обрыв питания посреди записи) —
//     НЕ стираем: переименовываем в *-tornHHMMSS.jsonl (улика живёт,
//     возрастная очистка её покрывает) и начинаем свежий сегмент;
//   · финальный flush по SH_EVENT_SHUTDOWN (концепция ИБЖ, стр. 25);
//   · SD пропала/вернулась — закрываем/переоткрываем, мастер не падает.
//
// Источники: все PUBLISH локального брокера, прошедшие фильтр
// journal.topics (маски MQTT, дефолт: события парка + живость, БЕЗ
// телеметрии), плюс синтетика broker/client_left (sMQTT LWT не исполняет —
// смерть клиента иначе нигде не фиксируется).
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <services/JournalCore.h>

struct BrokerEventInfo;   // BrokerService.h — только в .cpp (шапка лёгкая)

class JournalService : public ModuleBase {
public:
    static JournalService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "JournalService"; }
    const char* getVersion() const override { return "0.3.0"; }   // M3.1 стержень, M3.2 вьюер, 5.8.0 антифлуд+скачивание
    ModuleId getModuleId() const override { return 0x1106; }   // hm: 0x1101=Sd, 0x1102=App, 0x1103=Broker, 0x1104=Bridge, 0x1105=WxMirror; 04.09.2026: 0x1104→0x1106 (был дубль с BridgeService)
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    // --- Хук брокера (статический; контекст tick — только кольцо) --------
    static void onBrokerEvent(const BrokerEventInfo& info);

    // --- Состояние (для UI/ПАЗ) ------------------------------------------
    bool     enabled()   const { return _enabled; }
    bool     writing()   const { return _fileOpen; }      // сегмент открыт
    uint8_t  queued()    const { return _ring.count; }
    uint32_t dropped()   const { return _ring.dropped; }
    uint32_t written()   const { return _written; }       // строк за uptime
    uint32_t segmentKb() const { return _segBytes / 1024; }
    uint32_t lastFlushUnix() const { return _lastFlushUnix; }
    const char* segment() const { return _segName; }
    bool     degraded()  const { return _degraded; }      // SD нет / время нет

    // --- M3.2: чтение для вьюера (контекст HTTP-задачи, НЕ tick; read-only) -
    // Сегменты читаем потоково блоками JRN_BLK; append из tick не блокируем:
    // прочитанные байты append не сдвигает, а хвост без '\n' просто не
    // отдаём — он допишется и придёт следующим курсором. BSS-буферы чтения —
    // НЕ реентерабельны (одна админка, один запрос за раз).
    bool apiFiles(char* out, size_t cap);                 // список сегментов
    bool apiTail(char* out, size_t cap, uint16_t maxLines);   // хвост текущего
    bool apiRead(char* out, size_t cap, const char* name, // страница + курсор
                 uint32_t offset, const jrn::JrnQuery& q);
    // 5.8.0, «Скачать журнал»: валидированный путь сегмента для потоковой
    // отдачи (стримит HomeMasterUi через HttpService::streamFileDownload —
    // файл целиком в heap НЕ поднимается). false — имя чужое/файла нет.
    bool segmentPath(const char* name, char* out, size_t cap) const;

    // Бюджеты чтения (M3.2) — публичные: UI клампит n по JRN_PAGE_LINES.
    // Страница 100 строк ИЛИ сколько влезло в буфер ответа (byte-cap в
    // сборщике) — что наступит раньше; окно хвоста 7 блоков.
    static constexpr uint8_t  JRN_LIST_MAX   = 32;     // сегментов в списке
    static constexpr uint16_t JRN_PAGE_LINES = 100;    // строк на страницу
    static constexpr uint16_t JRN_BLK        = 2048;   // блок чтения сегмента
    static constexpr uint16_t JRN_TAIL_WIN   = 14336;  // окно хвоста, байт

private:
    JournalService() = default;

    bool validSegmentName(const char* name) const;  // events-*[0-9a-z.-].jsonl

    void flushRing();                 // кольцо -> сегмент (tick/SHUTDOWN)
    // Антифлуд 5.8.0 (урок ночи 14→15.08: защёлкнутый HEAP_CRITICAL замка
    // писал одну и ту же строку каждые 30 с всю ночь — 1488 строк, сегмент
    // раздут на 177 КБ чистого шума). Одинаковые (топик+тело) события
    // подряд склеиваются: первая строка пишется сразу, повторы считаются,
    // при смене события/закрытии серии выходит строка «<тело> ×N».
    // Потерь нет: количество сохранено в N, время первого — в её строке,
    // время последнего — в строке сводки.
    void materializeRepeat(bool continues);
    bool openSegment();               // открыть/создать текущий сегмент
    void closeSegment();
    void bootCheck(const char* name); // проверка хвоста, torn-переименование
    void rotateIfNeeded();            // размер/смена суток
    void cleanupExpired();            // возрастная очистка (раз в час)
    void fullPath(char* out, size_t cap, const char* name) const;

    static constexpr const char* DIR = "/journal";
    static constexpr size_t   TAIL_SCAN = 512;   // boot-check: последние байты
    static constexpr uint32_t CLEANUP_INTERVAL_MS = 3600000;  // раз в час

    JrnRing  _ring;
    bool     _enabled   = false;
    bool     _fileOpen  = false;
    bool     _degraded  = false;
    char     _filter[JRN_FILTER_LEN * 2] = "";   // список масок через запятую
    char     _segName[JRN_NAME_LEN] = "";
    uint32_t _segBytes  = 0;          // размер открытого сегмента (без lseek)
    uint32_t _written   = 0;
    uint32_t _lastFlushMs  = 0;
    uint32_t _lastFlushUnix = 0;
    uint32_t _lastCleanupMs = 0;
    uint32_t _flushS    = JRN_FLUSH_DEF_S;
    uint32_t _maxMb     = JRN_MAX_MB_DEF;
    uint32_t _maxDays   = JRN_MAX_DAYS_DEF;
    // Антифлуд 5.8.0 — состояние открытой серии повторов (см. выше).
    // Сравнение — по бюджетам строки журнала: за их пределами записанные
    // строки и так неотличимы, значит и клеить их законно.
    static constexpr uint8_t JRN_REPEAT_CAP = 20;   // сводка на каждые 20
    bool     _streakActive = false;
    uint32_t _repeatCount  = 0;
    char     _lastTopic[96] = "";
    char     _lastPayload[JRN_PAYLOAD_KEEP + 8] = "";
    void*    _file      = nullptr;    // fs::File* (без include FS.h в шапке)
    // BSS чтения (M3.2): список сегментов и блок сканирования. Не на стек —
    // HTTP-задача 8 КБ (урок loopTask); не в кучу — бюджеты BSS (урок v4.2.2).
    char     _lstNames[JRN_LIST_MAX][JRN_NAME_LEN];
    uint32_t _lstSizes[JRN_LIST_MAX];
    uint8_t  _rdbuf[JRN_BLK];
};
