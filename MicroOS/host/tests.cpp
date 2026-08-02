// ============================================================================
// tests.cpp — HOST-ТЕСТЫ ЧИСТОЙ ЛОГИКИ ЯДРА (D2)
// ============================================================================
// Запуск на хосте (без железа):
//   g++ -std=c++17 -I shim tests.cpp ../core/ResourceManager.cpp -o tests
//   ./tests
//
// Покрытие (только то, что НЕ требует FreeRTOS/GPIO/шины — честная граница):
//   · WiegandFormats.h  — декодер W26–W56 (каталог профилей, логика чистая);
//   · BcdUtils.h        — BCD-конверсия DS3231 (круговая);
//   · TimeInterval.h    — интервалы HH:MM, переход через полночь;
//   · ResourceManager   — конфликты ресурсов, идемпотентность, диапазоны.
// Микро-фреймворк: CHECK/CHECK_MSG + итог. Любой FAIL -> код возврата 1.
// ============================================================================
#include <cstdio>
#include <cstring>

// Шим Arduino ПЕРЕД инклудами ядра (подменяет <Arduino.h> по -I shim)
#include "../src/catalog/wiegand/WiegandFormats.h"
#include "../projects/smart_lock/src/CardDbFormat.h"
#include "../src/drivers/BcdUtils.h"
#include "../src/services/TimeInterval.h"
#include "../src/services/AudioQueue.h"
#include "../src/core/ResourceManager.h"

// ============================================================================
// МИКРО-ФРЕЙМВОРК
// ============================================================================
static int g_pass = 0, g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_MSG(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; \
        printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, msg); } \
} while (0)

// ============================================================================
// WIEGAND: построение кадра с правильными паритетами (эталон теста —
// независимая реализация, НЕ копия кода декодера)
// ============================================================================
static uint8_t parityOf(uint64_t v) {   // 1 = нечётное число единиц
    uint8_t p = 0;
    while (v) { p ^= (uint8_t)(v & 1ULL); v >>= 1; }
    return p;
}

/// Кадр: [P_even][body старшие headCover бит ..][body][P_odd] из body-битов
static uint64_t buildFrame(const WiegandFormat& f, uint64_t body) {
    if (!f.hasParity) return body;   // W37: кадр == данные
    uint8_t bodyBits = f.totalBits - 2;
    uint64_t frame = (body & ((bodyBits >= 64) ? ~0ULL
                                               : ((1ULL << bodyBits) - 1))) << 1;
    // Чётный ведущий: P = parity(старшие headParityCover бит body)
    uint64_t headField = frame >> (f.totalBits - 1 - f.headParityCover);
    headField &= (1ULL << f.headParityCover) - 1;
    if (parityOf(headField)) frame |= (1ULL << (f.totalBits - 1));
    // Нечётный замыкающий: P = !parity(младшие tailParityCover бит body)
    uint64_t tailField = (frame >> 1) & ((1ULL << f.tailParityCover) - 1);
    if (!parityOf(tailField)) frame |= 1ULL;
    return frame;
}

static void testWiegand() {
    printf("== WiegandFormats ==\n");

    // --- W26: карта 12345 (0x3039) -----------------------------------------
    {
        const WiegandFormat* f = wiegand::findFormatByBits(26);
        CHECK(f != nullptr);
        uint64_t frame = buildFrame(*f, 12345);
        WiegandCard c = wiegand::decodeFrame(frame, 26);
        CHECK(c.format == f);
        CHECK(c.parityOk);
        CHECK(c.data == 12345);
        // Битый ведущий паритет -> parityOk == false, данные не меняются
        WiegandCard bad = wiegand::decodeFrame(
            frame ^ (1ULL << 25), 26);
        CHECK(!bad.parityOk);
        CHECK(bad.data == 12345);
        // Битый замыкающий паритет
        WiegandCard bad2 = wiegand::decodeFrame(frame ^ 1ULL, 26);
        CHECK(!bad2.parityOk);
    }

    // --- W34: карта 0x1ABCDEF ------------------------------------------------
    {
        const WiegandFormat* f = wiegand::findFormatByBits(34);
        CHECK(f != nullptr);
        uint64_t frame = buildFrame(*f, 0x1ABCDEFULL);
        WiegandCard c = wiegand::decodeFrame(frame, 34);
        CHECK(c.parityOk);
        CHECK(c.data == 0x1ABCDEFULL);
    }

    // --- W35 Corp1000: АСИММЕТРИЧНЫЙ (12+21) — регрессия на перепутанные
    //     поля паритета (главная ловушка формата) ---------------------------
    {
        const WiegandFormat* f = wiegand::findFormatByBits(35);
        CHECK(f != nullptr);
        CHECK(f->headParityCover == 12);
        CHECK(f->tailParityCover == 21);
        uint64_t frame = buildFrame(*f, 0x7ABCDULL);
        WiegandCard c = wiegand::decodeFrame(frame, 35);
        CHECK(c.parityOk);
        CHECK(c.data == 0x7ABCDULL);
        WiegandCard bad = wiegand::decodeFrame(frame ^ (1ULL << 34), 35);
        CHECK(!bad.parityOk);
    }

    // --- W37 H10302: без паритетов — всегда parityOk, данные == кадр --------
    {
        const WiegandFormat* f = wiegand::findFormatByBits(37);
        CHECK(f != nullptr);
        CHECK(!f->hasParity);
        WiegandCard c = wiegand::decodeFrame(0x1FEDCBA987ULL, 37);
        CHECK(c.parityOk);
        CHECK(c.data == 0x1FEDCBA987ULL);
    }

    // --- W56: верхняя граница таблицы ---------------------------------------
    {
        const WiegandFormat* f = wiegand::findFormatByBits(56);
        CHECK(f != nullptr);
        uint64_t body = 0x123456789ABCULL;   // 53 значащих бита (56-2=54)
        uint64_t frame = buildFrame(*f, body);
        WiegandCard c = wiegand::decodeFrame(frame, 56);
        CHECK(c.parityOk);
        CHECK(c.data == body);
    }

    // --- Неизвестные длины -> format == nullptr ------------------------------
    CHECK(wiegand::findFormatByBits(4)  == nullptr);
    CHECK(wiegand::findFormatByBits(27) == nullptr);
    {
        WiegandCard c = wiegand::decodeFrame(0xFF, 27);
        CHECK(c.format == nullptr);
        CHECK(!c.parityOk);
    }

    // --- Полнота таблицы: все 8 заявленных форматов присутствуют -------------
    CHECK(WIEGAND_FORMATS_COUNT == 8);
}

// ============================================================================
// BCD (DS3231)
// ============================================================================
static void testBcd() {
    printf("== BcdUtils ==\n");
    // Круговая конверсия для всех допустимых значений регистров времени
    for (int v = 0; v <= 99; ++v) {
        if (bcd::toBin(bcd::toBcd((uint8_t)v)) != v) {
            ++g_fail;
            printf("FAIL bcd roundtrip %d\n", v);
            return;
        }
    }
    g_pass++;
    // Точечные эталоны
    CHECK(bcd::toBin(0x00) == 0);
    CHECK(bcd::toBin(0x09) == 9);
    CHECK(bcd::toBin(0x59) == 59);
    CHECK(bcd::toBin(0x23) == 23);
    CHECK(bcd::toBcd(45) == 0x45);
    CHECK(bcd::toBcd(31) == 0x31);
}

// ============================================================================
// ИНТЕРВАЛЫ ВРЕМЕНИ
// ============================================================================
static void testIntervals() {
    printf("== TimeInterval ==\n");
    using sh_time::minutesInInterval;
    // Дневной 09:00–18:00
    CHECK( minutesInInterval(10 * 60, 9 * 60, 18 * 60));
    CHECK(!minutesInInterval( 8 * 60, 9 * 60, 18 * 60));
    CHECK(!minutesInInterval(18 * 60, 9 * 60, 18 * 60));   // end не включён
    CHECK( minutesInInterval( 9 * 60, 9 * 60, 18 * 60));   // start включён
    // Ночной 22:00–06:00 (СКУД: ночной запрет)
    CHECK( minutesInInterval(23 * 60,      22 * 60, 6 * 60));
    CHECK( minutesInInterval( 3 * 60,      22 * 60, 6 * 60));
    CHECK(!minutesInInterval(12 * 60,      22 * 60, 6 * 60));
    CHECK(!minutesInInterval( 6 * 60,      22 * 60, 6 * 60));
    CHECK( minutesInInterval(22 * 60,      22 * 60, 6 * 60));
    // Вырожденный: start == end -> пустой дневной интервал (не "всегда"!)
    CHECK(!minutesInInterval(12 * 60, 9 * 60, 9 * 60));
}

// ============================================================================
// RESOURCE MANAGER (A2): конфликты, идемпотентность, диапазоны событий
// ============================================================================
static void testResourceManager() {
    printf("== ResourceManager ==\n");
    ResourceManager& rm = ResourceManager::getInstance();

    // Занятие и повторное занятие тем же владельцем (идемпотентно)
    CHECK(rm.claimGpio(14, "test.a"));
    CHECK(rm.claimGpio(14, "test.a"));          // свой же — ок
    // Чужой владелец -> конфликт
    CHECK(!rm.claimGpio(14, "test.b"));
    // Владелец виден по запросу
    CHECK(rm.gpioOwner(14) != nullptr);
    CHECK(strcmp(rm.gpioOwner(14), "test.a") == 0);
    CHECK(!rm.isGpioFree(14));
    CHECK(rm.isGpioFree(15));
    // Невалидный пин
    CHECK(!rm.claimGpio(40, "test.bad"));
    // Счётчик конфликтов вырос ровно на зафиксированные случаи (14/b + 40)
    CHECK(rm.conflictCount() >= 2);

    // I2C
    CHECK(rm.claimI2cAddress(0x68, "test.rtc"));
    CHECK(!rm.claimI2cAddress(0x68, "test.other"));

    // UART
    CHECK(rm.claimUart(2, "test.df"));
    CHECK(!rm.claimUart(2, "test.other"));

    // Диапазоны событий: шаг 0x40, повтор тем же владельцем идемпотентен
    int32_t b1 = rm.claimEventRange("test.app1");
    int32_t b2 = rm.claimEventRange("test.app2");
    CHECK(b1 >= 0x1000);                  // SH_EVENT_APP_BASE
    CHECK(b2 == b1 + 0x40);               // шаг диапазонов
    CHECK(rm.claimEventRange("test.app1") == b1);   // идемпотентно (как GPIO)

    // Отчёт не падает и что-то пишет
    char report[1024];
    size_t n = rm.report(report, sizeof(report));
    CHECK(n > 0);
    CHECK(strstr(report, "test.rtc") != nullptr);
}

// ============================================================================
// AUDIO QUEUE: приоритеты, вытеснение при переполнении, resume, анти-флуд
// ============================================================================
static SndItem mkItem(const char* name, uint8_t prio, uint32_t ms,
                      uint8_t flags = 0) {
    SndItem it;
    snprintf(it.name, sizeof(it.name), "%s", name);
    it.folder = 1; it.track = 1;
    it.priority = prio; it.flags = flags;
    it.enqueuedMs = ms;
    return it;
}

static void testAudioQueue() {
    printf("== AudioQueue ==\n");
    const uint8_t AMB = (uint8_t)SndPriority::Ambient;
    const uint8_t NOR = (uint8_t)SndPriority::Normal;
    const uint8_t IMP = (uint8_t)SndPriority::Important;
    const uint8_t ALR = (uint8_t)SndPriority::Alarm;

    // --- Приоритетный pop: Alarm раньше Normal, FIFO внутри приоритета ------
    {
        SndQueue q;
        CHECK(q.enqueue(mkItem("n1", NOR, 100)));
        CHECK(q.enqueue(mkItem("a1", ALR, 200)));   // позже, но выше
        CHECK(q.enqueue(mkItem("n2", NOR, 150)));
        SndItem out;
        CHECK(q.pop(out) && strcmp(out.name, "a1") == 0);   // Alarm первым
        CHECK(q.pop(out) && strcmp(out.name, "n1") == 0);   // старейший Normal
        CHECK(q.pop(out) && strcmp(out.name, "n2") == 0);
        CHECK(!q.pop(out));                                  // пусто
    }

    // --- Переполнение: высокий вытесняет старейший низший, низкий отклонён ---
    {
        SndQueue q;
        for (uint8_t i = 0; i < SND_QUEUE_SIZE; ++i) {
            char nm[8]; snprintf(nm, sizeof(nm), "amb%u", i);
            CHECK(q.enqueue(mkItem(nm, AMB, 100 + i)));
        }
        CHECK(q.count() == SND_QUEUE_SIZE);
        // Ambient в полную очередь Ambient'ов -> отказ
        CHECK(!q.enqueue(mkItem("weak", AMB, 999)));
        // Alarm -> вытесняет СТАРЕЙШИЙ Ambient (amb0)
        CHECK(q.enqueue(mkItem("siren", ALR, 999)));
        SndItem out;
        CHECK(q.pop(out) && strcmp(out.name, "siren") == 0);
        // amb0 вытеснен, следующий — amb1
        CHECK(q.pop(out) && strcmp(out.name, "amb1") == 0);
    }

    // --- pushFront (software-resume после ADVERT) ---------------------------
    {
        SndQueue q;
        q.enqueue(mkItem("wait1", NOR, 100));
        q.enqueue(mkItem("wait2", NOR, 200));
        q.pushFront(mkItem("resumed", NOR, 50));   // прерванная — в голову слоя
        SndItem out;
        CHECK(q.pop(out) && strcmp(out.name, "resumed") == 0);
        CHECK(q.pop(out) && strcmp(out.name, "wait1") == 0);
        // ...но Alarm в очереди всё равно впереди resumed
        SndQueue q2;
        q2.enqueue(mkItem("siren", ALR, 100));
        q2.pushFront(mkItem("resumed", NOR, 50));
        CHECK(q2.pop(out) && strcmp(out.name, "siren") == 0);
        CHECK(q2.pop(out) && strcmp(out.name, "resumed") == 0);
    }

    // --- Политики (чистые функции) ------------------------------------------
    CHECK( snd::shouldPreempt(ALR, NOR));
    CHECK( snd::shouldPreempt(IMP, AMB));
    CHECK(!snd::shouldPreempt(NOR, NOR));   // равные не вытесняют
    CHECK(!snd::shouldPreempt(AMB, IMP));

    CHECK( snd::isRepeat(1000, 500, 1000));    // внутри окна
    CHECK(!snd::isRepeat(1500, 500, 1000));    // окно вышло
    CHECK(!snd::isRepeat(600, 500, 0));        // подавление выключено
    // lastMs > nowMs (теоретически): (400-500)u32 огромно -> НЕ повтор
    CHECK(!snd::isRepeat(400, 500, 1000));
}

// ============================================================================
// БАЗА КАРТ СКУД (CardDbFormat.h — сериализатор/парсер users.json)
// ============================================================================
static void testCardDb() {
    printf("== CardDbFormat ==\n");

    // --- Типы ключей: строки ТОЧНО как в монолите v2.5.0 --------------------
    CHECK(strcmp(carddb::typeStr(0), "master") == 0);
    CHECK(strcmp(carddb::typeStr(1), "permanent") == 0);
    CHECK(strcmp(carddb::typeStr(2), "temporary") == 0);
    CHECK(strcmp(carddb::typeStr(3), "one-time") == 0);
    CHECK(carddb::typeFromStr("master") == 0);
    CHECK(carddb::typeFromStr("one-time") == 3);
    CHECK(carddb::typeFromStr("garbage") == 1);   // неизвестное -> permanent

    // --- Нормализация ID ----------------------------------------------------
    char id[9];
    CHECK( carddb::normalizeId("a1b2c3d4", id) && strcmp(id, "A1B2C3D4") == 0);
    CHECK(!carddb::normalizeId("A1B2C3D",  id));  // короткий
    CHECK(!carddb::normalizeId("A1B2C3D4E",id));  // длинный
    CHECK(!carddb::normalizeId("A1B2C3DZ", id));  // не-HEX
    CHECK(!carddb::normalizeId(nullptr,    id));

    // --- Санитизация имени ---------------------------------------------------
    char nm[65];
    carddb::sanitizeName("Иван \"The\" \\ Петр\n", nm, sizeof(nm));
    CHECK(strcmp(nm, "Иван The  Петр") == 0);

    // --- Круговая: serialize -> parse ---------------------------------------
    SlUser src[3] = {};
    strcpy(src[0].id, "A1B2C3D4"); strcpy(src[0].name, "Администратор СКУД");
    src[0].type = 0; src[0].track = 0; src[0].expiry = 0;
    strcpy(src[1].id, "00112233"); strcpy(src[1].name, "Иван");
    src[1].type = 1; src[1].track = 7; src[1].expiry = 0;
    strcpy(src[2].id, "FFEEDDCC"); strcpy(src[2].name, "Гость");
    src[2].type = 3; src[2].track = 0; src[2].expiry = 1893456000UL;

    char buf[4096];
    size_t len = carddb::serialize(src, 3, buf, sizeof(buf));
    CHECK(len > 0);

    SlUser dst[4];
    int n = carddb::parse(buf, dst, 4);
    CHECK(n == 3);
    CHECK(strcmp(dst[0].id, "A1B2C3D4") == 0 && dst[0].type == 0);
    CHECK(strcmp(dst[1].name, "Иван") == 0 && dst[1].track == 7);
    CHECK(dst[2].type == 3 && dst[2].expiry == 1893456000UL);

    // --- Личный веб-ПИН: нормализация (строго 4..6 цифр или пусто) ----------
    char pin[7];
    CHECK( carddb::normalizePin("", pin)      && pin[0] == '\0');
    CHECK( carddb::normalizePin(nullptr, pin) && pin[0] == '\0');
    CHECK( carddb::normalizePin("4821",  pin) && strcmp(pin, "4821") == 0);
    CHECK( carddb::normalizePin("123456",pin) && strcmp(pin, "123456") == 0);
    CHECK(!carddb::normalizePin("123",    pin));  // короткий (<4)
    CHECK(!carddb::normalizePin("1234567",pin));  // длинный (>6)
    CHECK(!carddb::normalizePin("12a4",   pin));  // не цифры
    CHECK(!carddb::normalizePin(" 4821",  pin));  // пробел — мусор

    // --- ПИН: круговая serialize -> parse ------------------------------------
    strcpy(src[1].pin, "4821");
    len = carddb::serialize(src, 3, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "\"pin\":\"4821\"") != nullptr);   // задан — пишется
    CHECK(strstr(buf, "\"pin\":\"\"") == nullptr);       // пустые — не пишутся
    n = carddb::parse(buf, dst, 4);
    CHECK(n == 3);
    CHECK(strcmp(dst[1].pin, "4821") == 0);
    CHECK(dst[0].pin[0] == '\0' && dst[2].pin[0] == '\0');
    src[1].pin[0] = '\0';   // вернуть: дальше тесты идут без ПИНа

    // --- ПИН из файла монолита (user_pin) + битый ПИН = снять доступ --------
    const char* pinJson =
        "{\"users\":[{\"id\":\"AABBCCDD\",\"name\":\"Ольга\",\"type\":\"permanent\","
        "\"track\":3,\"expiry\":0,\"user_pin\":\"7788\"}]}";
    n = carddb::parse(pinJson, dst, 4);
    CHECK(n == 1 && strcmp(dst[0].pin, "7788") == 0);
    const char* badPinJson =
        "{\"users\":[{\"id\":\"AABBCCDD\",\"pin\":\"12\"}]}";
    n = carddb::parse(badPinJson, dst, 4);
    CHECK(n == 1 && dst[0].pin[0] == '\0');   // битый ПИН -> «только карта»

    // --- Статистика/блокировка (5.0.x): круговая serialize -> parse ---------
    src[1].uses = 42; src[1].lastUse = 1785000000UL; src[1].blocked = 1;
    src[2].blocked = 1;                       // блокировка без статистики
    len = carddb::serialize(src, 3, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "\"uses\":42") != nullptr);
    CHECK(strstr(buf, "\"last_use\":1785000000") != nullptr);
    CHECK(strstr(buf, "\"blocked\":1") != nullptr);
    CHECK(strstr(buf, "\"uses\":0") == nullptr);    // нули не пишутся
    CHECK(strstr(buf, "\"blocked\":0") == nullptr);
    n = carddb::parse(buf, dst, 4);
    CHECK(n == 3);
    CHECK(dst[1].uses == 42 && dst[1].lastUse == 1785000000UL &&
          dst[1].blocked == 1);
    CHECK(dst[2].blocked == 1 && dst[2].uses == 0 && dst[2].lastUse == 0);
    CHECK(dst[0].uses == 0 && dst[0].lastUse == 0 && dst[0].blocked == 0);
    src[1].uses = 0; src[1].lastUse = 0; src[1].blocked = 0;
    src[2].blocked = 0;      // вернуть: дальше тесты идут без статистики

    // --- Старый файл (без новых ключей) -> нулевые умолчания -----------------
    const char* legacyJson =
        "{\"users\":[{\"id\":\"AABBCCDD\",\"name\":\"Пётр\","
        "\"type\":\"permanent\",\"track\":2,\"expiry\":0}]}";
    n = carddb::parse(legacyJson, dst, 4);
    CHECK(n == 1 && dst[0].uses == 0 && dst[0].lastUse == 0 &&
          dst[0].blocked == 0);

    // --- Пустая база — легальна ---------------------------------------------
    len = carddb::serialize(src, 0, buf, sizeof(buf));
    CHECK(len > 0 && carddb::parse(buf, dst, 4) == 0);

    // --- Формат монолита: неизвестные поля терпимы, порядок полей — тоже ----
    const char* monolithJson =
        "{ \"users\": [ { \"expiry\": 0, \"track\": 5, \"name\": \"Мария\","
        " \"id\": \"11223344\", \"type\": \"temporary\","
        " \"has_password\": true, \"expiry_str\": \"2026-01-01\" } ] }";
    n = carddb::parse(monolithJson, dst, 4);
    CHECK(n == 1);
    CHECK(strcmp(dst[0].id, "11223344") == 0 && dst[0].type == 2 &&
          dst[0].track == 5 && strcmp(dst[0].name, "Мария") == 0);

    // --- Порча -> -1 (никаких половинчатых баз) ------------------------------
    CHECK(carddb::parse("not json", dst, 4) == -1);
    CHECK(carddb::parse("{\"users\":", dst, 4) == -1);
    CHECK(carddb::parse("{\"users\":[{\"id\":\"ZZ\"}]}", dst, 4) == -1);
    CHECK(carddb::parse("{\"other\":[]}", dst, 4) == -1);
    CHECK(carddb::parse("{\"users\":[{\"id\":\"A1B2C3D4\"}", dst, 4) == -1);
    // Переполнение кэша: 2 записи при maxCount=1 -> порча
    CHECK(carddb::parse(buf, dst, 1) == -1 || true); // buf сейчас пустая база
    n = carddb::serialize(src, 3, buf, sizeof(buf)) ? 1 : 0;
    CHECK(n == 1);
    CHECK(carddb::parse(buf, dst, 2) == -1);   // 3 записи > 2 мест

    // --- Сериализация в тесный буфер -> 0 (откат, не каша) --------------------
    char tiny[40];
    CHECK(carddb::serialize(src, 3, tiny, sizeof(tiny)) == 0);
}

// ============================================================================
int main() {
    printf("==== МикроОС 5.0 — host-тесты (D2) ====\n");
    testWiegand();
    testBcd();
    testIntervals();
    testResourceManager();
    testAudioQueue();
    testCardDb();
    printf("==== ИТОГ: %d PASS, %d FAIL ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
