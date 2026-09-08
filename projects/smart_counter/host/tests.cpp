// ============================================================================
// tests.cpp — HOST-ТЕСТЫ ЧИСТОЙ ЛОГИКИ профиля smart_counter (C1)
// ============================================================================
// Запуск на хосте (без железа), из projects/smart_counter/host:
//   ./run_tests.sh
// (каноничная раскладка: ядро — ../../../MicroOS; шим — ../../../MicroOS/host/shim)
//
// Покрытие (только то, что НЕ требует FreeRTOS/GPIO/шины — честная граница):
//   · ScMercuryCore.h — CRC16, BCD, сборка/разбор кадров Меркурий-206;
//   · ScPulseCore.h   — антидребезг, шумовой вентиль, детектор протечки, EMA;
//   · ScBillingCore.h — биллинг, отчётный день (вкл. догон), фильтр Big Blue.
// Векторы — docs/Тест-векторы_Mercury-206.md (концепт-нота C0, Issue #3).
// Микро-фреймворк: CHECK + итог (образец — weather_gate/host/tests.cpp).
// Любой FAIL -> код возврата 1.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <cmath>

#include "../src/ScMercuryCore.h"
#include "../src/ScPulseCore.h"
#include "../src/ScBillingCore.h"

// ============================================================================
// МИКРО-ФРЕЙМВОРК
// ============================================================================
static int g_pass = 0, g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static bool feq(float a, float b) { return fabsf(a - b) < 0.001f; }

// ============================================================================
// ГРУППА 1–2. CRC16 И КАДРЫ ЗАПРОСОВ (векторы из журнала тестов C1)
// ============================================================================
static void testCrc16() {
    printf("== ScMercuryCore: CRC16 ==\n");

    // Контрольный вектор каталога CRC-16/MODBUS
    CHECK(scm::crc16((const uint8_t*)"123456789", 9) == 0x4B37);

    // Резидуальный трюк парсера: CRC(кадр + свой CRC) == 0
    uint8_t f[8]; size_t n = 0;
    const uint8_t addr[4] = {0x12, 0x34, 0x56, 0x78};
    n = scm::buildRequest(addr, scm::SCM_CMD_INSTANT, nullptr, 0, f, sizeof(f));
    CHECK(n == 7);
    CHECK(scm::crc16(f, n) == 0x0000);
}

static void testRequests() {
    printf("== ScMercuryCore: кадры запросов (SN=12345678) ==\n");
    const uint8_t addr[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t f[16];

    // Эталоны вычислены независимо (python, журнал тестов) — не копией кода
    static const struct { uint8_t cmd; const uint8_t* par; uint8_t parLen;
                          const char* hex; } V[] = {
        { scm::SCM_CMD_TEST,    nullptr, 0, "\x12\x34\x56\x78\x00\x50\x23" },
        { scm::SCM_CMD_INSTANT, nullptr, 0, "\x12\x34\x56\x78\x63\x10\x0A" },
        { scm::SCM_CMD_TIME,    nullptr, 0, "\x12\x34\x56\x78\x21\x90\x3B" },
        { scm::SCM_CMD_SERIAL,  nullptr, 0, "\x12\x34\x56\x78\x2F\x11\xFF" },
    };
    for (auto& v : V) {
        size_t n = scm::buildRequest(addr, v.cmd, v.par, v.parLen, f, sizeof(f));
        CHECK(n == 7);
        CHECK(memcmp(f, v.hex, 7) == 0);
    }

    // 0x27 энергия: текущие и на начало суток
    size_t n = scm::buildEnergyRequest(addr, 0x00, f, sizeof(f));
    CHECK(n == 8 && memcmp(f, "\x12\x34\x56\x78\x27\x00\x38\xCC", 8) == 0);
    n = scm::buildEnergyRequest(addr, 0x0F, f, sizeof(f));
    CHECK(n == 8 && memcmp(f, "\x12\x34\x56\x78\x27\x0F\x78\xC8", 8) == 0);

    // 0x37 профиль за 05.03.26
    n = scm::buildProfileRequest(addr, 5, 3, 26, f, sizeof(f));
    CHECK(n == 11 && memcmp(f, "\x12\x34\x56\x78\x37\x00\x05\x03\x26\x84\x25", 11) == 0);

    // Переполнение выходного буфера -> 0
    CHECK(scm::buildRequest(addr, scm::SCM_CMD_INSTANT, nullptr, 0, f, 5) == 0);
}

static void testAddrFromSerial() {
    printf("== ScMercuryCore: адрес из серийника ==\n");
    uint8_t a[4];
    scm::addrFromSerial(12345678u, a);
    CHECK(a[0]==0x12 && a[1]==0x34 && a[2]==0x56 && a[3]==0x78); // урок журнала: НЕ 0C 23 45 67
    scm::addrFromSerial(1234u, a);   // короткий номер — нули СЛЕВА
    CHECK(a[0]==0x00 && a[1]==0x00 && a[2]==0x12 && a[3]==0x34);
}

// ============================================================================
// ГРУППА 3–4. BCD И РАЗБОР ОТВЕТОВ
// ============================================================================
// Сборка эталонного ответа: addr + cmd + payload + CRC (как счётчик).
static size_t buildReply(const uint8_t* addr, uint8_t cmd,
                         const uint8_t* payload, uint8_t payloadLen,
                         uint8_t* out) {
    size_t len = 4 + 1 + payloadLen + 2;
    memcpy(out, addr, 4); out[4] = cmd;
    if (payloadLen) memcpy(out + 5, payload, payloadLen);
    uint16_t crc = scm::crc16(out, len - 2);
    out[len-2] = (uint8_t)(crc & 0xFF); out[len-1] = (uint8_t)(crc >> 8);
    return len;
}

static void testBcd() {
    printf("== ScMercuryCore: BCD ==\n");
    const uint8_t u[] = {0x23, 0x10};       CHECK(feq(scm::bcdToF(u, 2, 10.0f), 231.0f));
    const uint8_t i[] = {0x15, 0x20};       CHECK(feq(scm::bcdToF(i, 2, 100.0f), 15.20f));
    const uint8_t p[] = {0x00, 0x35, 0x00}; CHECK(feq(scm::bcdToF(p, 3, 100.0f), 35.0f));
    const uint8_t p2[] = {0x35, 0x00, 0x00}; CHECK(feq(scm::bcdToF(p2, 3, 100.0f), 3500.0f));
    const uint8_t e[] = {0x00, 0x12, 0x34, 0x56};
    CHECK(feq(scm::bcdToF(e, 4, 100.0f), 1234.56f));
    const uint8_t mx[] = {0x99, 0x99, 0x99, 0x99};
    CHECK(scm::bcdToU32(mx, 4) == 99999999u);
    const uint8_t one[] = {0x00, 0x00, 0x00, 0x01};
    CHECK(scm::bcdToU32(one, 4) == 1u);
}

static void testParsers() {
    printf("== ScMercuryCore: разбор ответов ==\n");
    const uint8_t addr[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t buf[64];

    // 0x63: 231.0 В / 15.20 А / 3500 Вт (P=3500 -> BCD 35 00 00, /100)
    const uint8_t pl63[] = {0x23,0x10, 0x15,0x20, 0x35,0x00,0x00};
    size_t n = buildReply(addr, scm::SCM_CMD_INSTANT, pl63, sizeof(pl63), buf);
    scm::Instant inst;
    CHECK(scm::parseInstant(buf, n, inst));
    CHECK(feq(inst.voltage_v, 231.0f) && feq(inst.current_a, 15.20f) && feq(inst.power_w, 3500.0f));
    buf[n-1] ^= 0x01;                                     // битый CRC
    CHECK(!scm::parseInstant(buf, n, inst));
    CHECK(!scm::parseInstant(buf, n - 2, inst));          // усечённый кадр
    n = buildReply(addr, scm::SCM_CMD_INSTANT, pl63, sizeof(pl63), buf);
    buf[4] = scm::SCM_CMD_ENERGY;                         // чужой код команды
    CHECK(!scm::parseInstant(buf, n, inst));

    // 0x27: T1 = 1234.56 кВт·ч, T2..T4 = 0
    const uint8_t pl27[] = {0x00,0x12,0x34,0x56, 0,0,0,0, 0,0,0,0, 0,0,0,0};
    n = buildReply(addr, scm::SCM_CMD_ENERGY, pl27, sizeof(pl27), buf);
    uint32_t kwh100 = 0;
    CHECK(scm::parseEnergyTotal(buf, n, kwh100));
    CHECK(kwh100 == 123456u);                             // сотые кВт·ч, без float

    // 0x2F, раскладка С эхом кода: [addr][2F][SN4][дата3][CRC]
    const uint8_t pl2f[] = {0x12,0x34,0x56,0x78, 0x01,0x01,0x20};
    n = buildReply(addr, scm::SCM_CMD_SERIAL, pl2f, sizeof(pl2f), buf);
    uint8_t ra[4]; char sn[16];
    CHECK(scm::parseSerial(buf, n, ra, sn, sizeof(sn)));
    CHECK(memcmp(ra, addr, 4) == 0);
    CHECK(strcmp(sn, "12345678") == 0);
    // ведущие нули срезаются: SN 00001234 -> "1234"
    const uint8_t pl2z[] = {0x00,0x00,0x12,0x34, 0x01,0x01,0x20};
    n = buildReply(addr, scm::SCM_CMD_SERIAL, pl2z, sizeof(pl2z), buf);
    CHECK(scm::parseSerial(buf, n, ra, sn, sizeof(sn)));
    CHECK(strcmp(sn, "1234") == 0);
    // 0x2F, раскладка БЕЗ эха кода (подозрение по коду монолита): [addr][SN4][дата3][CRC]
    {
        uint8_t raw[13];
        memcpy(raw, addr, 4);
        memcpy(raw + 4, pl2f, 7);
        uint16_t crc = scm::crc16(raw, 11);
        raw[11] = (uint8_t)(crc & 0xFF); raw[12] = (uint8_t)(crc >> 8);
        CHECK(scm::parseSerial(raw, 13, ra, sn, sizeof(sn)));
        CHECK(strcmp(sn, "12345678") == 0);
        CHECK(memcmp(ra, addr, 4) == 0);
    }

    // 0x37: 3 точки профиля, терминатор FF FF
    const uint8_t pl37[] = {0x00,0x64, 0x01,0x2C, 0x00,0x0A, 0xFF,0xFF};
    n = buildReply(addr, scm::SCM_CMD_PROFILE, pl37, sizeof(pl37), buf);
    float prof[48];
    int cnt = scm::parseProfile(buf, n, prof, 48);
    CHECK(cnt == 3);
    CHECK(feq(prof[0], 10.0f) && feq(prof[1], 30.0f) && feq(prof[2], 1.0f));
    buf[n-1] ^= 0x80;
    CHECK(scm::parseProfile(buf, n, prof, 48) == -1);     // битый кадр

    // FSM-политика опроса: мгновенные приоритетнее энергии
    CHECK(scm::nextPoll(6000, 0, 0) == scm::Fsm::WaitInstant);
    // мгновенные не дозрели (2 с < 5 с), энергия дозрела (31 с >= 30 с)
    CHECK(scm::nextPoll(31000, 29000, 0) == scm::Fsm::WaitEnergy);
    // обе дозрели -> приоритет мгновенных (оперативный канал ПАЗ)
    CHECK(scm::nextPoll(35000, 29000, 0) == scm::Fsm::WaitInstant);
    CHECK(scm::nextPoll(6000, 5000, 5500) == scm::Fsm::Idle);
}

// ============================================================================
// ГРУППА 5. SC_PULSE_CORE
// ============================================================================
static void testPulseCore() {
    printf("== ScPulseCore ==\n");

    // Антидребезг 200 мс (мкс)
    CHECK(scp::debounceAccept(200001, 0, scp::SCP_DEFAULT_DEBOUNCE_US));
    CHECK(!scp::debounceAccept(199999, 0, scp::SCP_DEFAULT_DEBOUNCE_US));
    // wrap-safe: разность unsigned (last за 65536 мкс до переполнения,
    // now после него -> прошло 215536 мкс > 200000)
    CHECK(scp::debounceAccept(150000, 0xFFFFFFFFFFFF0000ULL, 200000));

    // Шумовой вентиль (sc.pulse.max_hz = 50)
    CHECK(scp::isNoiseRate(60, 1, 50));       // 60 Гц за окно 1 с — наводка
    CHECK(!scp::isNoiseRate(40, 1, 50));      // 40 Гц — в норме
    CHECK(!scp::isNoiseRate(5, 0, 50));       // окно 0 — вентиль молчит

    // Пересчёт импульсов (только для показа; учёт — в импульсах)
    CHECK(feq(scp::pulsesToM3(12345, 1.0f), 12.345f));
    CHECK(feq(scp::pulsesToM3(100, 0.0f), 0.0f));          // защита от деления
    // Газ: «импульсов на м3» (делитель, симметрия с водой)
    CHECK(feq(scp::gasPulsesToM3(12345, 100.0f), 123.45f));  // 100 имп/м3 (1 имп = 0.01 м3)
    CHECK(feq(scp::gasPulsesToM3(1000, 1.0f), 1000.0f));     // грубый счётчик 1 имп/м3
    CHECK(feq(scp::gasPulsesToM3(500, 0.0f), 0.0f));         // защита от деления
    CHECK(feq(scp::gasPulsesToM3(500, -5.0f), 0.0f));        // отрицательный делитель
    CHECK(feq(scp::flowLitresPerHour(5, 3600, 1.0f), 5.0f));

    // Детектор протечки: 6 мокрых часов со средним >= порога -> LEAK,
    // сухой час -> CLEARED; микропоток ниже порога — тревоги нет.
    {
        scp::LeakDetector d; d.init();
        int ev = 0;
        for (int h = 0; h < scp::SCP_LEAK_WINDOW_H - 1; h++) {
            d.addLitres(250.0f); ev = d.closeHour(200.0f);
            CHECK(ev == 0);                               // серия ещё короткая
        }
        d.addLitres(250.0f); ev = d.closeHour(200.0f);    // 6-й мокрый час
        CHECK(ev == 1);                                   // SC_EVENT_LEAK_DETECTED
        d.addLitres(250.0f); ev = d.closeHour(200.0f);    // тревога не дублируется
        CHECK(ev == 0);
        ev = d.closeHour(200.0f);                         // сухой час
        CHECK(ev == 2);                                   // SC_EVENT_LEAK_CLEARED
    }
    {
        scp::LeakDetector d; d.init();
        int ev = 0;
        for (int h = 0; h < 24; h++) { d.addLitres(50.0f); ev |= d.closeHour(200.0f); }
        CHECK(ev == 0);   // капающий кран 50 л/ч ниже порога — ПАЗ молчит
    }

    // EMA АКБ: первый отсчёт не сглаживается (ПАЗ батареи не слепнет
    // на старте), alpha из конфига (sc.bat.ema_alpha)
    CHECK(feq(scp::emaUpdate(0.0f, 4.1f, 0.15f), 4.1f));
    CHECK(feq(scp::emaUpdate(4.0f, 4.0f, 0.15f), 4.0f));
    CHECK(feq(scp::emaUpdate(4.0f, 5.0f, 0.15f), 4.15f));
    CHECK(feq(scp::emaUpdate(4.0f, 5.0f, 1.0f), 5.0f));    // alpha=1 — без фильтра
    CHECK(feq(scp::emaUpdate(4.0f, 5.0f, 0.0f), 4.0f));    // alpha=0 — заморожено

    // Статус АКБ (sc.bat.low_v = 3.4)
    CHECK(scp::batteryState(0.2f, true, 3.4f) == 0);       // АКБ отсутствует
    CHECK(scp::batteryState(3.2f, false, 3.4f) == 1);      // разряд на батарее
    CHECK(scp::batteryState(3.2f, true, 3.4f) == 2);       // та же батарея на сети — норма
    CHECK(scp::batteryState(4.1f, false, 3.4f) == 2);
}

// ============================================================================
// ГРУППА 6. SC_BILLING_CORE
// ============================================================================
static void testBillingCore() {
    printf("== ScBillingCore ==\n");

    // monthly/bill; база выше показаний -> 0 (отрицательного счёта нет)
    scb::Bill b = scb::computeBill(100.0f, 80.0f, 6.5f);
    CHECK(feq(b.monthly, 20.0f) && feq(b.amount, 130.0f));
    b = scb::computeBill(50.0f, 80.0f, 6.5f);
    CHECK(feq(b.monthly, 0.0f) && feq(b.amount, 0.0f));

    // Отчётный день 24-е. YYYYMMDD сравнивается числом.
    CHECK(scb::ymd(2026, 9, 24) == 20260924u);
    // штатный сброс: сегодня 24-е, последний сброс вчера
    CHECK(scb::isReportDue(2026, 9, 24, 24, scb::ymd(2026, 9, 23)));
    // повторный вызов в тот же день -> нет (урок монолита: RAM-static
    // lastResetDay после ребута давал повторный сброс и обнулял месяц)
    CHECK(!scb::isReportDue(2026, 9, 24, 24, scb::ymd(2026, 9, 24)));
    // обычный день — нет
    CHECK(!scb::isReportDue(2026, 9, 20, 24, scb::ymd(2026, 8, 24)));
    // первый запуск (0) — фиксируем базу сразу
    CHECK(scb::isReportDue(2026, 9, 20, 24, 0));
    // catch-up: отчётный день прошёл, пока устройство молчало
    CHECK(scb::isReportDue(2026, 9, 26, 24, scb::ymd(2026, 9, 20)));
    // но если сброс уже был позже отчётного дня — не догоняем
    CHECK(!scb::isReportDue(2026, 9, 26, 24, scb::ymd(2026, 9, 25)));

    // База при catch-up — показания AC-loss снимка (не текущие!)
    CHECK(feq(scb::catchupBase(101.2f, 99.5f, true), 99.5f));
    CHECK(feq(scb::catchupBase(101.2f, 0.0f, false), 101.2f));
    CHECK(feq(scb::catchupBase(101.2f, 120.0f, true), 101.2f)); // снимок «новее» — брак

    // Фильтр Big Blue 20"
    CHECK(feq(scb::filterUsageL(12.345f, 10.0f), 2345.0f));
    CHECK(feq(scb::filterUsageL(10.0f, 12.0f), 0.0f));     // база выше — износа в минус нет
    CHECK(feq(scb::filterRemainingL(60.0f, 10.0f, 50000.0f), 0.0f));
    CHECK(scb::filterWarningStep(false, 0.0f) == 1);       // ресурс исчерпан -> событие
    CHECK(scb::filterWarningStep(true, 0.0f) == 0);        // не дублируем
    CHECK(scb::filterWarningStep(true, 100.0f) == 2);      // картридж заменен -> снятие
    CHECK(scb::filterWarningStep(false, 100.0f) == 0);
}

// ============================================================================
int main() {
    testCrc16();
    testRequests();
    testAddrFromSerial();
    testBcd();
    testParsers();
    testPulseCore();
    testBillingCore();
    printf("== ИТОГ: %d PASS, %d FAIL ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
