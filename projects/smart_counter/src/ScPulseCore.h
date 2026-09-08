// ============================================================================
// ScPulseCore.h — ИМПУЛЬСНЫЕ ВХОДЫ (ВОДА/ГАЗ): ЧИСТАЯ ЛОГИКА (C1)
// ============================================================================
// СТАТУС: C1. Тег v5.8.4 выпущен, блокировка снята (Issue #3).
// Host-тесты: ../host/tests.cpp.
//
// Происхождение: антидребезг геркона 200 мс и EMA АКБ 0.15/0.85 — из
// продакшн-монолита; PCNT-периферия и батч-NVS — НЕ здесь (это ядерный
// CounterService: attachPcnt + increment). Здесь только политика,
// которую можно гонять в host-тестах без железа.
//
// Уроки, зашитые в этот заголовок:
//  1. УЧЁТ ВЕДЁМ В ИМПУЛЬСАХ, ВЫВОДИМ В МЕТРАХ КУБИЧЕСКИХ. Монолит хранил
//     water_total как float м³ и пересчитывал каждый цикл — на десятках м³
//     float начинает «ржаветь» (7 значащих цифр: 99999.99 м³ — предел).
//     Импульс — атомарен и целочисленен; м³ = pulses / impPerL / 1000
//     только для показа и биллинга. Согласовано с CounterService: он
//     хранит uint32-инкременты.
//  2. GPIO34/35/36/39 — input-only БЕЗ внутренних подтяжек (платформенный
//     справочник): внешний pull-up обязателен, это требование заносится
//     в паспорт узла. К логике не относится, но отсюда правило: «нет
//     импульсов» и «обрыв линии» в железе неразличимы — значит, диагностику
//     молчания счётчика возлагаем на health-check, не на этот код.
//  3. Антидребезг 200 мс монолита — это потолок 5 Гц на геркон. Бытовой
//     водосчётчик даёт 1 имп/литр, душ ~12 л/мин = 0.2 Гц — запас 25×.
//     Всё, что БЫСТРЕЕ sc.pulse.max_hz после антидребезга, — это не вода,
//     а наводка/дребезг катастрофы: не считаем расходом, считаем шумом
//     (health-check sc.pulse). Поле sc.pulse.max_hz — поправка рецензии §3.1.
//  4. Протечка — это не «много литров», а «нет сухого часа». Капающий кран
//     даст 3 л/сутки — ни один порог литров/час её не поймает; зато у неё
//     нет ни одного часа с нулевым расходом. Поэтому детектор: N подряд
//     часов с ненулевым расходом И средний >= threshold_l_h (порог отсекает
//     АКБ-холодильник-сценарии у газа... у воды — фильтр-промывку). Снятие
//     тревоги — первый сухой час. Поле sc.leak.threshold_l_h есть в схеме;
//     окно N — константа здесь (в схему не просится, меняется редко).
// ============================================================================
#pragma once

#include <cstdint>

namespace scp {

// --- БЮДЖЕТЫ ---------------------------------------------------------------------
constexpr uint32_t SCP_DEFAULT_DEBOUNCE_US = 200000;  // 200 мс (монолит, продакшн)
constexpr uint32_t SCP_DEFAULT_MAX_HZ      = 50;      // sc.pulse.max_hz (рецензия §3.1)
constexpr uint8_t  SCP_LEAK_WINDOW_H       = 6;       // часов без «сухого часа» -> протечка
constexpr float    SCP_DEFAULT_EMA_ALPHA   = 0.15f;   // sc.bat.ema_alpha (рецензия §3.2)

// --- АНТИДРЕБЕЗГ -------------------------------------------------------------------
/// Принять импульс? Микросекундные метки, unsigned-разность wrap-safe
/// (тот же приём, что cnt::shouldFlush). Первый импульс после инициализации
/// принимаем всегда (lastUs==0 — а реальная метка esp_timer стартует с ~0;
/// поэтому инициализировать lastUs = nowUs при старте канала).
inline bool debounceAccept(uint64_t nowUs, uint64_t lastUs, uint32_t debounceUs) {
    return (nowUs - lastUs) > debounceUs;
}

/// Шумовой вентиль ПАЗ: частота принятых (после антидребезга) импульсов
/// выше maxHz -> это наводка, не расход. Подавать «частоту» окном:
/// pulsesAccepted / windowS. Возвращает true = кадрировать как шум.
inline bool isNoiseRate(uint32_t pulsesAccepted, uint32_t windowS, uint32_t maxHz) {
    if (windowS == 0) return false;
    return (uint64_t)pulsesAccepted * 1000ull / windowS > (uint64_t)maxHz * 1000ull;
}

// --- ПЕРЕСЧЁТ (только для показа/биллинга, учёт — в импульсах, урок 1) ------------
inline float pulsesToM3(uint32_t pulses, float impPerLitre) {
    if (impPerLitre <= 0.0f) return 0.0f;
    return (float)pulses / impPerLitre / 1000.0f;
}

/// Мгновенный расход окном, л/ч: дельта импульсов за windowS секунд.
inline float flowLitresPerHour(uint32_t pulsesDelta, uint32_t windowS, float impPerLitre) {
    if (windowS == 0 || impPerLitre <= 0.0f) return 0.0f;
    return (float)pulsesDelta / impPerLitre * 3600.0f / (float)windowS;
}

// --- ДЕТЕКТОР ПРОТЕЧКИ (урок 4) ------------------------------------------------------
/// Кольцо почасового расхода (24 слота — сутки истории заодно для UI).
/// Чистая логика: тикает раз в час из SmartCounterApp::tick, без RTC —
/// часы считаются по миллисекундным меткам вызывающего.
struct LeakDetector {
    float    hourLitres[24];  // кольцо расхода по часам, литры
    uint8_t  head;            // слот текущего (незавершённого) часа
    uint8_t  dryStreakBroken; // служебный флаг: в текущем часе уже был расход
    bool     leakActive;

    void init() {
        for (int i = 0; i < 24; i++) hourLitres[i] = 0.0f;
        head = 0; dryStreakBroken = 0; leakActive = false;
    }

    /// Добавить импульсы в текущий час (пересчёт в литры — снаружи).
    void addLitres(float litres) {
        hourLitres[head] += litres;
        if (litres > 0.0f) dryStreakBroken = 1;
    }

    /// Закрыть час и сдвинуть кольцо. Возвращает событие:
    /// 0 — ничего, 1 — протечка обнаружена, 2 — протечка снята.
    int closeHour(float thresholdLph) {
        // подряд идущих «мокрых» часов, включая только что закрытый
        int wetStreak = 0;
        for (int k = 0; k < 24; k++) {
            int idx = (head - k + 24) % 24;
            if (hourLitres[idx] > 0.0f) wetStreak++; else break;
        }
        // средний расход по мокрой серии — порог отсечения микропотоков
        float sum = 0.0f;
        for (int k = 0; k < wetStreak; k++) sum += hourLitres[(head - k + 24) % 24];
        float avg = wetStreak ? sum / wetStreak : 0.0f;

        int event = 0;
        bool nowLeak = (wetStreak >= SCP_LEAK_WINDOW_H) && (avg >= thresholdLph);
        if (nowLeak && !leakActive)  { leakActive = true;  event = 1; }
        if (!nowLeak && leakActive)  { leakActive = false; event = 2; }

        head = (uint8_t)((head + 1) % 24);
        hourLitres[head] = 0.0f;
        dryStreakBroken = 0;
        return event;
    }
};

// --- EMA-ФИЛЬТР АКБ (продакшн-монолит, alpha теперь из конфига — §3.2) ---------------
/// Первый отсчёт после включения — не сглаживаем, иначе фильтр «ползёт»
/// к реальному напряжению минуты и ПАЗ АКБ слепнет на старте (монолит:
/// инициализация при battery_voltage < 0.1).
inline float emaUpdate(float prev, float sample, float alpha) {
    if (prev < 0.1f) return sample;
    if (alpha <= 0.0f) return prev;
    if (alpha >= 1.0f) return sample;
    return sample * alpha + prev * (1.0f - alpha);
}

/// Статус АКБ: 0 — отсутствует, 1 — разряжен (на батарее, сеть пропала),
/// 2 — норма. Порог low_v из конфига sc.bat.low_v (умолчание 3.4 В).
inline int batteryState(float battV, bool acPresent, float lowV) {
    if (battV < 0.5f) return 0;
    if (battV < lowV && !acPresent) return 1;
    return 2;
}

} // namespace scp
