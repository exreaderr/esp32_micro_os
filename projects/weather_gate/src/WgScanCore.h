// ============================================================================
// WgScanCore.h — ЧИСТАЯ ЛОГИКА СКАНЕРА ЧАСТОТЫ (W3.3, 0.5.0)
// Без железа и FreeRTOS: сетка точек, агрегация метрик, рекомендация.
// Покрывается host-тестами (projects/weather_gate/host/tests.cpp).
// Частоты — в сотых МГц (91504 = 915.04 МГц), чтобы не таскать float.
// Окно скана ±0.20 МГц вокруг домашней частоты — проверено монолитом v5.2
// (АЧХ 914.80–915.20; оптимум датчика лежал в 915.00–915.04, уход кварца
// станции + приёмника суммарно ±50–70 кГц — окно покрывает с запасом).
// ============================================================================
#pragma once

#include <cstdint>

namespace wgs {

constexpr uint8_t  SCAN_MAX_POINTS   = 41;     // шаг 0.01 на всём окне — потолок
constexpr uint16_t SCAN_SPAN_X100    = 20;     // ±0.20 МГц вокруг домашней
constexpr uint32_t SCAN_FREQ_MIN_X100 = 91400; // границы схемы wx.rf_freq_mhz
constexpr uint32_t SCAN_FREQ_MAX_X100 = 91600;
constexpr uint16_t SCAN_STEP_MIN_X100 = 2;     // 0.02 МГц (21 точка)
constexpr uint16_t SCAN_STEP_MAX_X100 = 5;     // 0.05 МГц (9 точек)
constexpr uint16_t SCAN_DWELL_MIN_S   = 30;    // окно наблюдения на точку
constexpr uint16_t SCAN_DWELL_MAX_S   = 120;

// Метрики одной точки сетки. pkt — уникальные пакеты (дельта packetSeq,
// серия из 6 копий станции считается дедуплицированно драйвером).
struct ScanPoint {
    uint32_t freqX100 = 0;
    uint16_t pkt      = 0;
    uint16_t rssiN    = 0;      // событий приёма (для avg; pkt может быть >N)
    int16_t  rssiMax  = -127;
    int32_t  rssiSum  = 0;
    int32_t  noiseSum = 0;
    uint16_t noiseN   = 0;

    int16_t rssiAvg() const { return rssiN ? (int16_t)(rssiSum / rssiN) : 0; }
    int16_t noiseAvg() const { return noiseN ? (int16_t)(noiseSum / noiseN) : 0; }
};

// Учёт принятого пакета на текущей точке (rssi — RSSI этого пакета, дБм).
inline void scanPointOnPacket(ScanPoint& p, uint16_t uniqueDelta, int16_t rssi) {
    p.pkt = (uint16_t)(p.pkt + uniqueDelta);
    p.rssiN++;
    p.rssiSum += rssi;
    if (rssi > p.rssiMax) p.rssiMax = rssi;
}

// Выборка шумовой дорожки (readRssiNow между пакетами).
inline void scanPointOnNoise(ScanPoint& p, int16_t rssiNow) {
    p.noiseSum += rssiNow;
    p.noiseN++;
}

// Сетка: home ± SPAN с шагом stepX100, по возрастанию, кламп к границам
// схемы, без дублей (кламп краёв может схлопнуть точки). Возврат — число
// точек (<= maxOut). stepX100 клампится к [STEP_MIN, STEP_MAX].
inline uint8_t scanGrid(uint32_t homeX100, uint16_t stepX100,
                        uint32_t* outFreqs, uint8_t maxOut) {
    if (stepX100 < SCAN_STEP_MIN_X100) stepX100 = SCAN_STEP_MIN_X100;
    if (stepX100 > SCAN_STEP_MAX_X100) stepX100 = SCAN_STEP_MAX_X100;
    int32_t start = (int32_t)homeX100 - SCAN_SPAN_X100;
    int32_t stop  = (int32_t)homeX100 + SCAN_SPAN_X100;
    uint8_t n = 0;
    uint32_t prev = 0;
    for (int32_t f = start; f <= stop && n < maxOut; f += stepX100) {
        int32_t c = f;
        if (c < SCAN_FREQ_MIN_X100) c = SCAN_FREQ_MIN_X100;
        if (c > SCAN_FREQ_MAX_X100) c = SCAN_FREQ_MAX_X100;
        if (n > 0 && c == prev) continue;   // кламп схлопнул — дубль не пишем
        outFreqs[n++] = (uint32_t)c;
        prev = (uint32_t)c;
    }
    return n;
}

// Рекомендация: индекс точки с максимальным rssiMax среди имеющих пакеты
// (pkt >= 1); при равенстве — ближайшая к домашней частоте (меньший уход
// от проверенной рабочей точки). Пакетов нет нигде — (-1): рекомендации
// нет, решение за оператором.
inline int8_t scanRecommend(const ScanPoint* pts, uint8_t n, uint32_t homeX100) {
    int8_t best = -1;
    for (uint8_t i = 0; i < n; i++) {
        if (pts[i].pkt == 0) continue;
        if (best < 0) { best = (int8_t)i; continue; }
        const ScanPoint& b = pts[best];
        if (pts[i].rssiMax > b.rssiMax) { best = (int8_t)i; continue; }
        if (pts[i].rssiMax == b.rssiMax) {
            int32_t di = (int32_t)pts[i].freqX100 - homeX100; if (di < 0) di = -di;
            int32_t db = (int32_t)b.freqX100 - homeX100;      if (db < 0) db = -db;
            if (di < db) best = (int8_t)i;
        }
    }
    return best;
}

} // namespace wgs
