// ============================================================================
// HmArchiveModule.h — АРХИВАРИУС W7: «длинный» архив погоды на SD мастера
// ============================================================================
// Дизайн-нота W7 (ревизия 2, 20.09.2026) + ответ ядерной ветки (5 штифтиков
// контракта). Ядро не тронуто (ЗАКОН ФЛОТА) — модуль профиля home_master.
//
// ДВЕ ЗАДАЧИ:
//   1) НАКОПЛЕНИЕ: слушаем weather-JSON шлюза через хук локального брокера
//      (in-proc, 0 сокетов — работает и в автономии, и в норме), часовые
//      ведра dlog::Bucket (DataLogCore.h, чистая логика ядра), слив на SD
//      append-only: /archive/<gw_id>/<канал>-<ГГГГ>.w7a
//      (заголовок W7A1 + DlogAggr 16 Б — канон флота).
//   2) ВОССТАНОВЛЕНИЕ: MQTT запрос-ответ (контракт ниже). Шлюз после
//      перепрошивки ФС забирает свои 7 дней кадрами «канал-день».
//
// КОНТРАКТ (зафиксирован ответом ядерной ветки 20.09.2026):
//   req : <prefix>/master/archive/req        {"id":"<gw>","days":7}
//                                             {"id":"<gw>","seq":<n>} (переспрос)
//   resp: <prefix>/master/archive/resp/<gw>  {"seq":n,"total":N,"ch":"wx_ot",
//                                             "day":d,"crc":<u32>,"r":[[ts,mn,mx,avg],..]}
//   · day=0 — самый старый день, записи по возрастанию ts;
//   · crc — CRC32/zlib (poly 0xEDB88320, init/xorout 0xFFFFFFFF) по байтам
//     сериализованного поля "r" (строка вида [[..],[..]] как есть);
//   · кадры НЕ retained; total = каналы × days всегда (пустые дни — "r":[]);
//   · пустой архив целиком — немедленный {"seq":0,"total":0};
//   · антидребезг: повторный полный req от того же id игнорируется 120 с;
//   · темп: не более одного кадра за тик (150 мс) — мастер брокер, залпов
//     из обработчика быть не должно. seq детерминирован: seq = ch*days + day
//     → переспрос регенерирует кадр из архива, хранить отправленное не нужно.
//
// Условия ядра к архивариусу: SD-сеансы open→append→close короткие (урок
// питания 15.09), tick без сканов каталога, чтение архива — кусками по
// 4 КБ за тик (файл открыт только на время сессии выдачи, ≤ десятков
// секунд; сторож сессии 120 с закрывает при любом сбое).
//
// Конфиг (конец схемы, группа «Архив W7»):
//   arch.enabled   BOOL   true  — архивариус вкл/выкл
//   arch.src_topic STRING "microos/weather_gate/weather" — топик источника
//   arch.gw_id     STRING "weather_gate" — id шлюза (каталог и resp-топик)
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <services/DataLogCore.h>   // dlog::Bucket, DlogAggr (канон 16 Б)
#include "BrokerService.h"          // BrokerEventInfo, хук in-proc
#include <FS.h>                    // fs::File (файл сканирования сессии)

class HmArchiveModule : public ModuleBase {
public:
    static HmArchiveModule& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "HmArchive"; }
    const char* getVersion() const override { return "0.1.0-w7"; }
    ModuleId getModuleId() const override { return 0x1107; }   // hm: 0x1101..0x1106 заняты
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 150; }  // темп кадров
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- Точка входа хука брокера (static, контекст tick — ТОЛЬКО лёгкое) --
    static void onBrokerEvent(const BrokerEventInfo& info);

    // --- Состояние (ПАЗ/панель) -------------------------------------------
    bool     enabled()      const { return _enabled; }
    uint32_t recWritten()   const { return _recWritten; }   // записей в архив
    uint32_t recSkipped()   const { return _recSkipped; }   // идемпотентность/дедуп
    uint32_t reqServed()    const { return _reqServed; }    // сессий выдачи
    uint32_t framesSent()   const { return _framesSent; }
    bool     serving()      const { return _serving; }

private:
    HmArchiveModule() = default;

    // --- Накопление ---------------------------------------------------------
    static constexpr uint8_t CH_COUNT = 5;
    // Каналы архива = каналы шлюза (wx_ot/wx_oh/wx_p/wx_w/wx_r), ключи JSON
    // weather-контракта шлюза (temp/humidity/press/wind/rain).
    static const char* chName(uint8_t i);
    static const char* chJsonKey(uint8_t i);

    void onWeather(const char* payload);            // парсинг + ведра (хук)
    void flushChannel(uint8_t ch);                  // SD: append ведра (tick)
    bool appendRecord(uint8_t ch, const DlogAggr& r);
    uint32_t readLastTs(uint8_t ch);                // хвост файла (идемпотент.)
    void filePath(uint8_t ch, int year, char* out, size_t n) const;
    static bool writeHeaderIfNew(fs::File& f, const char* chName);

    // --- Выдача (восстановление) ---------------------------------------------
    void onRequest(const char* payload);            // парсинг req (хук)
    void serveTick();                               // сессия выдачи (tick)
    bool buildFrame();                              // кадр _seq в _frame (кусками)
    void sendFrame(const char* ch, int day, const char* rbuf, uint32_t seq,
                   uint32_t total);
    void sendEmpty();                               // {"seq":0,"total":0}
    void abortSession(const char* why);
    int  dayOfRequest() const;                      // day из _seq

    // CRC32/zlib (контрактный вариант; таблица 1 КБ в RAM — лениво, один раз)
    static uint32_t crc32zlib(const uint8_t* d, size_t n);

    // --- Данные: накопление ---------------------------------------------------
    bool     _enabled = false;
    char     _srcTopic[MQTT_TOPIC_LEN] = "";
    char     _gwId[33] = "";
    char     _lastPayload[BROKER_EVENT_PAYLOAD_LEN] = "";  // дедуп retained
    dlog::Bucket _bkt[CH_COUNT];              // открытые часовые ведра
    DlogAggr _rolled[CH_COUNT];               // закрытые ведра (к сливу)
    bool     _rollPend[CH_COUNT] = {};
    uint32_t _lastTs[CH_COUNT] = {};          // последний ts в файле (0=не читали)
    bool     _lastTsKnown[CH_COUNT] = {};
    uint32_t _recWritten = 0, _recSkipped = 0;
    bool     _firstWxLogged = false;          // инфо-лог о первом принятом weather-кадре

    // --- Данные: выдача ---------------------------------------------------------
    bool     _serving = false;
    char     _reqId[33] = "";
    uint32_t _reqStartedMs = 0;
    uint32_t _lastFullReqMs = 0;              // антидребезг 120 с
    char     _lastFullReqId[33] = "";
    uint8_t  _days = 7;
    uint32_t _seq = 0, _total = 0;
    uint8_t  _resendOnly = 0;                 // 1 = одиночный переспрос seq
    uint32_t _resendSeq = 0;
    // Сканирование файла кусками (512 Б/тик = 32 целых записи; чтение
    // выровнено по 16 Б, неполных записей в окне не бывает).
    fs::File _scanFile;
    uint8_t  _scanCh = 0xFF;
    uint8_t  _scanYearIdx = 0;                // 0 = прошлый год, 1 = текущий
    DlogAggr _win[512 / sizeof(DlogAggr)];    // окно разбора (32 записи)
    // Сборка текущего кадра
    char     _rbuf[176] = "";                 // "[[ts,mn,mx,avg],...]" (4 записи, худший 143 Б)
    uint16_t _rN = 0;                         // записей в кадре
    char     _frame[256] = "";                // кадр целиком (худший 216 Б < капа 240)
    uint32_t _reqServed = 0, _framesSent = 0;
};
