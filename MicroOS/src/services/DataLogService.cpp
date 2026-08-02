// ============================================================================
// DataLogService.cpp — реализация даталоггера (см. шапку .h)
// ============================================================================
#include "DataLogService.h"
#include "ConfigService.h"
#include "TimeService.h"
#include <LittleFS.h>

DataLogService& DataLogService::getInstance() {
    static DataLogService instance;
    return instance;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void DataLogService::registerExtensions() {
    ConfigService::getInstance().addFields("Даталоггер", {
        { "datalog.enabled", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Даталоггер", "Писать историю метрик (графики, /datalog)" },
    });
}

void DataLogService::init() {
    _mtx = xSemaphoreCreateMutex();
    if (_mtx == nullptr) {
        log(LogLevel::Error, "mutex create FAILED — даталоггер выключен");
        return;
    }
    // Разовая boot-аллокация колец (см. .h — почему не BSS). calloc даёт
    // нули = валидное пустое состояние всех полей Channel.
    _ch = (Channel*)calloc(DLOG_MAX_CHANNELS, sizeof(Channel));
    if (_ch == nullptr) {
        log(LogLevel::Error, "channels alloc FAILED (%u КБ) — выключен",
            (unsigned)(DLOG_MAX_CHANNELS * sizeof(Channel) / 1024));
        return;
    }
    if (!LittleFS.exists(DLOG_DIR)) {
        LittleFS.mkdir(DLOG_DIR);
    }
    _initialized = true;
    log(LogLevel::Info, "init: %u канал(ов) max, ярусы 6ч/31сут/год",
        DLOG_MAX_CHANNELS);
}

void DataLogService::stop() {
    // Честное закрытие: открытые вёдра — в файлы (ребут/OTA не потеряет
    // накопленный час). Время может быть уже мёртво — тогда пропускаем.
    uint32_t now = (uint32_t)TimeService::getInstance().getUnixTime();
    if (_mtx != nullptr && now > 0 &&
        xSemaphoreTake(_mtx, pdMS_TO_TICKS(500)) == pdTRUE) {
        flushBuckets(now, /*all*/ true);
        xSemaphoreGive(_mtx);
    }
}

void DataLogService::tick() {
    // Ведро молчащего канала (точек нет) закрывается не следующей точкой,
    // а тиком: история не «застревает» в RAM на неопределённый срок.
    uint32_t now = (uint32_t)TimeService::getInstance().getUnixTime();
    if (now == 0 || _mtx == nullptr) return;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        flushBuckets(now, /*all*/ false);
        xSemaphoreGive(_mtx);
    }
}

// ============================================================================
// КАНАЛЫ
// ============================================================================
bool DataLogService::validId(const char* id) const {
    if (id == nullptr || id[0] == '\0' || strlen(id) >= sizeof(((Channel*)0)->id)) {
        return false;
    }
    for (const char* p = id; *p; ++p) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;    // id — часть пути файла, без сюрпризов
    }
    return true;
}

int8_t DataLogService::registerChannel(const char* id, const char* name,
                                       const char* unit) {
    if (!_initialized || _count >= DLOG_MAX_CHANNELS || !validId(id)) {
        return -1;
    }
    if (_mtx == nullptr ||
        xSemaphoreTake(_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    for (uint8_t i = 0; i < _count; ++i) {          // дубликат id
        if (strcmp(_ch[i].id, id) == 0) {
            xSemaphoreGive(_mtx);
            return (int8_t)i;                        // идемпотентно: тот же
        }
    }
    Channel& c = _ch[_count];
    safeStrCopy(c.id, sizeof(c.id), id);
    safeStrCopy(c.name, sizeof(c.name), name ? name : id);
    safeStrCopy(c.unit, sizeof(c.unit), unit ? unit : "");
    c.ring.reset();
    int8_t idx = (int8_t)_count;
    ++_count;
    xSemaphoreGive(_mtx);
    log(LogLevel::Info, "channel #%d '%s' (%s)", idx, id, c.name);
    return idx;
}

bool DataLogService::channelInfo(uint8_t ch, char* idOut, size_t idSz,
                                 char* nameOut, size_t nameSz,
                                 char* unitOut, size_t unitSz) const {
    if (ch >= _count) return false;
    safeStrCopy(idOut, idSz, _ch[ch].id);
    safeStrCopy(nameOut, nameSz, _ch[ch].name);
    safeStrCopy(unitOut, unitSz, _ch[ch].unit);
    return true;
}

// ============================================================================
// ЗАПИСЬ
// ============================================================================
bool DataLogService::logPoint(int8_t ch, float v) {
    if (!_initialized || ch < 0 || ch >= (int8_t)_count) return false;
    if (cfgGetBool("datalog.enabled", true) == false) return false;

    uint32_t now = (uint32_t)TimeService::getInstance().getUnixTime();
    if (now == 0) {                       // время недостоверно — точка мусор
        if (ch < DLOG_MAX_CHANNELS) ++_ch[ch].dropped;   // без мьютекса:
        return false;                     // dropped — диагностический счётчик
    }
    if (_mtx == nullptr ||
        xSemaphoreTake(_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        ++_ch[ch].dropped;
        return false;
    }
    Channel& c = _ch[ch];
    c.ring.push(now, v);

    // Вёдра ярусов: перекрытие периода -> готовый агрегат в файл
    DlogAggr rolled;
    char hp[32], dp[32];
    tierPaths((uint8_t)ch, hp, dp);
    if (c.hour.add(now, v, 3600, rolled)) {
        appendTier(hp, rolled, DLOG_HOUR_CAP);
    }
    if (c.day.add(now, v, 86400, rolled)) {
        appendTier(dp, rolled, DLOG_DAY_CAP);
    }
    xSemaphoreGive(_mtx);
    return true;
}

uint32_t DataLogService::droppedPoints(uint8_t ch) const {
    return ch < _count ? _ch[ch].dropped : 0;
}

// ============================================================================
// ЧТЕНИЕ
// ============================================================================
uint16_t DataLogService::getRaw(uint8_t ch, DlogPoint* out, uint16_t maxN,
                                uint32_t fromTs) {
    if (ch >= _count || _mtx == nullptr) return 0;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(300)) != pdTRUE) return 0;
    uint16_t n = _ch[ch].ring.snapshot(out, maxN, fromTs);
    xSemaphoreGive(_mtx);
    return n;
}

uint16_t DataLogService::getTier(uint8_t ch, bool daily, DlogAggr* out,
                                 uint16_t maxN, uint32_t fromTs) {
    if (ch >= _count || _mtx == nullptr) return 0;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return 0;

    char hp[32], dp[32];
    tierPaths(ch, hp, dp);
    const char* path = daily ? dp : hp;

    uint16_t n = 0;
    File f = LittleFS.open(path, "r");
    if (f) {
        DlogFileHeader hdr;
        if (f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr) &&
            hdr.magic == DLOG_FILE_MAGIC && hdr.count > 0) {
            // Потоковая децимация: записей может быть больше maxN — сливаем
            // группы по stride на лету (min/max/avg — та же математика, что
            // dlog::decimateAggr), охватывая ВЕСЬ диапазон, а не его хвост.
            // Файл в RAM не поднимаем — читаем по одной записи.
            uint16_t total = hdr.count;
            uint16_t stride = total > maxN
                ? (uint16_t)((total + maxN - 1) / maxN) : 1;
            DlogAggr acc;
            uint16_t inGroup = 0;
            for (uint16_t i = 0; i < total && n < maxN; ++i) {
                DlogAggr a;
                if (f.read((uint8_t*)&a, sizeof(a)) != sizeof(a)) break;
                if (fromTs != 0 && a.ts < fromTs) continue;
                if (inGroup == 0) {
                    acc = a;
                    inGroup = 1;
                } else {
                    if (a.mn < acc.mn) acc.mn = a.mn;
                    if (a.mx > acc.mx) acc.mx = a.mx;
                    acc.avg = (acc.avg + a.avg) * 0.5f;
                    ++inGroup;
                }
                if (inGroup >= stride) { out[n++] = acc; inGroup = 0; }
            }
            if (inGroup > 0 && n < maxN) out[n++] = acc;   // хвост-группа
        }
        f.close();
    }
    // Доклеиваем открытое ведро — свежий час/сутки виден сразу
    const dlog::Bucket& b = daily ? _ch[ch].day : _ch[ch].hour;
    if (n < maxN && b.n > 0) {
        DlogAggr cur;
        dlog::Bucket tmp = b;              // flush() мутирует — копия
        if (tmp.flush(cur) && (fromTs == 0 || cur.ts >= fromTs)) {
            out[n++] = cur;
        }
    }
    xSemaphoreGive(_mtx);
    return n;
}

// ============================================================================
// ФАЙЛЫ ЯРУСОВ (append-only + компакция)
// ============================================================================
void DataLogService::tierPaths(uint8_t ch, char* hourOut, char* dayOut) const {
    snprintf(hourOut, 32, "%s/H_%s.bin", DLOG_DIR, _ch[ch].id);
    snprintf(dayOut, 32, "%s/D_%s.bin", DLOG_DIR, _ch[ch].id);
}

bool DataLogService::appendTier(const char* path, const DlogAggr& rec,
                                uint16_t cap) {
    DlogFileHeader hdr;
    bool exists = false;

    File f = LittleFS.open(path, "r");
    if (f) {
        exists = f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr) &&
                 hdr.magic == DLOG_FILE_MAGIC;
        f.close();
    }
    if (!exists) {                        // новый ярус (или порча -> с нуля)
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = DLOG_FILE_MAGIC;
        hdr.cap   = cap;
    }

    if (exists && hdr.count >= cap) {
        // КОМПАКЦИЯ: сбрасываем старейшие 25%. Потоково, кусками по 8 записей
        // со СТЕКА (128 Б): целиком ярус в RAM не поднимаем (постмортем —
        // буфер 11.9 КБ в BSS съедал DRAM-регион при линковке).
        // Раз в месяц (часовой) / раз в год (суточный) — flash не страдает.
        File in = LittleFS.open(path, "r");
        if (!in) return false;
        char tmp[36];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        File out = LittleFS.open(tmp, "w");
        if (!out) { in.close(); return false; }

        uint16_t keepFrom = (uint16_t)(hdr.count / 4);
        uint16_t kept = 0;
        DlogFileHeader nh;
        memset(&nh, 0, sizeof(nh));
        nh.magic = DLOG_FILE_MAGIC;
        nh.cap   = cap;
        bool ok = out.write((uint8_t*)&nh, sizeof(nh)) == sizeof(nh);
        in.seek(sizeof(DlogFileHeader) + (size_t)keepFrom * sizeof(DlogAggr));
        DlogAggr chunk[8];
        while (ok && kept < (uint16_t)(hdr.count - keepFrom)) {
            size_t want = sizeof(DlogAggr) *
                ((hdr.count - keepFrom - kept) < 8 ? (hdr.count - keepFrom - kept) : 8);
            size_t got = in.read((uint8_t*)chunk, want);
            if (got == 0) break;
            ok = out.write((uint8_t*)chunk, got) == got;
            kept = (uint16_t)(kept + got / sizeof(DlogAggr));
        }
        in.close();
        if (ok) ok = out.write((uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
        out.close();
        if (!ok) { LittleFS.remove(tmp); return false; }
        ++kept;
        // Счётчик в заголовке tmp-файла — точечная правка 2 байт
        File h = LittleFS.open(tmp, "r+");
        if (h) {
            h.seek(offsetof(DlogFileHeader, count));
            h.write((uint8_t*)&kept, sizeof(kept));
            h.close();
        }
        LittleFS.remove(path);
        LittleFS.rename(tmp, path);
        log(LogLevel::Info, "tier %s compacted: %u -> %u", path,
            hdr.count, kept);
        return true;
    }

    // Обычный append: запись в конец + счётчик в заголовке
    File out = LittleFS.open(path, exists ? "a" : "w");
    if (!out) return false;
    bool ok = true;
    if (!exists) {
        ok = out.write((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr);
    }
    if (ok) ok = out.write((uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
    out.close();
    if (!ok) return false;

    // Счётчик в заголовке — точечная перезапись 2 байт
    File h = LittleFS.open(path, "r+");
    if (h) {
        uint16_t cnt = hdr.count + 1;
        h.seek(offsetof(DlogFileHeader, count));
        h.write((uint8_t*)&cnt, sizeof(cnt));
        h.close();
    }
    return true;
}

// ============================================================================
// ЗАКРЫТИЕ ВЁДЕР
// ============================================================================
void DataLogService::flushBuckets(uint32_t nowTs, bool all) {
    // Мьютекс УЖЕ взят вызывающим (tick/stop).
    for (uint8_t i = 0; i < _count; ++i) {
        char hp[32], dp[32];
        tierPaths(i, hp, dp);
        DlogAggr a;
        dlog::Bucket& hb = _ch[i].hour;
        if (hb.n > 0 && (all || hb.periodStart + 3600 <= nowTs)) {
            dlog::Bucket tmp = hb;
            if (tmp.flush(a) && appendTier(hp, a, DLOG_HOUR_CAP)) {
                hb.n = 0;                 // закрыто и записано
                hb.periodStart = 0;
            }
        }
        dlog::Bucket& db = _ch[i].day;
        if (db.n > 0 && (all || db.periodStart + 86400 <= nowTs)) {
            dlog::Bucket tmp = db;
            if (tmp.flush(a) && appendTier(dp, a, DLOG_DAY_CAP)) {
                db.n = 0;
                db.periodStart = 0;
            }
        }
    }
}
