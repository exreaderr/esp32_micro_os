// ============================================================================
// ScMercuryCore.h — ПРОТОКОЛ МЕРКУРИЙ-206: ЧИСТАЯ ЛОГИКА (C1)
// ============================================================================
// СТАТУС: C1. Тег v5.8.4 выпущен 03.09.2026, блокировка снята,
// концепт-нота принята ядерной веткой 01.09.2026 (Issue #3).
// Host-тесты: ../host/tests.cpp (векторы — docs/Тест-векторы_Mercury-206.md).
//
// Происхождение кода: проверенный годами монолит smart_counter_v4.4.2
// (crc16, bcdToF, неблокирующий FSM) — переносится КАК ЕСТЬ по смыслу,
// переписывается только форма (namespace, без Arduino-типов).
//
// Уроки, зашитые в этот заголовок:
//  1. «Журнал Меркурий-206.pdf» — это пересказ ответов ИИ-ассистента, а не
//     протокол Инкотекс. В нём есть прямые ошибки (пример адреса 12345678
//     как «0C 23 45 67» вместо 12 34 56 78; пример U «02 31 -> 231.0 В»,
//     который с его же функцией даёт 23.1 В; «12 байт» ответа 0x63 при
//     15-байтовой байтовой сетке в его же таблице). Источник истины по
//     байтовой сетке — ПРОДАКШН-монолит; источник истины по всему
//     остальному — стенд C1 с реальным счётчиком. Журнал используем
//     только как список «что вообще умеет прибор».
//  2. CRC16/MODBUS: полином 0x8005 в реверсивной форме 0xA001, init 0xFFFF,
//     без финального XOR. Контрольный вектор каталога CRC:
//     CRC("123456789") == 0x4B37 (проверено вычислением, см. тест-векторы).
//  3. Резидуальный трюк монолита: CRC(кадр + его CRC16) == 0x0000 для этого
//     варианта CRC. Проверено вычислением. Это даёт парсер, НЕ зависящий от
//     ожидаемой длины: любая пришедшая пачка байт либо цела (CRC==0), либо
//     бита. Ожидаемая длина нужна только как вентиль «данных достаточно».
//  4. Монолит ждал «m_idx >= 11/19» и мог начать разбор до прихода хвоста
//     кадра (на 9600 бод хвост приходит ~1 мс/байт). Работало за счёт того,
//     что к моменту M_PARSE всё уже лежало в буфере, — но это гонка.
//     Здесь вентиль ставим РОВНО на минимальную длину кадра команды.
//  5. Для энергии денежный учёт ведём в double/копейках — float теряет
//     копейки на миллионах Вт·ч (наблюдение из «Журнала», здесь верное).
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace scm {

// --- КОНСТАНТЫ ПРОТОКОЛА ------------------------------------------------------
constexpr uint8_t  SCM_ADDR_LEN   = 4;     // адрес = серийник, 4 байта BCD
constexpr uint16_t SCM_CRC_INIT   = 0xFFFF;
constexpr uint16_t SCM_CRC_POLY   = 0xA001; // 0x8005 в реверсивной записи

// Коды команд (байт 4 кадра запроса).
constexpr uint8_t SCM_CMD_TEST    = 0x00;  // тест связи (канальный уровень)
constexpr uint8_t SCM_CMD_TIME    = 0x21;  // чтение времени/даты счётчика
constexpr uint8_t SCM_CMD_ENERGY  = 0x27;  // накопленная энергия по тарифам
constexpr uint8_t SCM_CMD_SERIAL  = 0x2F;  // серийный номер + дата выпуска
constexpr uint8_t SCM_CMD_PROFILE = 0x37;  // суточный профиль мощности (30 мин)
constexpr uint8_t SCM_CMD_INSTANT = 0x63;  // U, I, P мгновенные

// Минимальные длины ПОЛНЫХ кадров ответа (адрес+код+данные+CRC) — вентиль
// «данных достаточно» (урок 4). Точную целостность даёт CRC-резидуал (урок 3).
constexpr uint8_t SCM_MINLEN_INSTANT = 14; // 4+1 + U2+I2+P3 + 2  (монолит, продакшн)
constexpr uint8_t SCM_MINLEN_ENERGY  = 23; // 4+1 + 4 тарифа × 4 байта + 2
constexpr uint8_t SCM_MINLEN_SERIAL  = 11; // 4 + SN4 + дата 3 (без эха кода — см. parseSerial)

// --- CRC16 (MODBUS-вариант, продакшн-монолит) ---------------------------------
/// Контрольный вектор каталога CRC: crc16("123456789", 9) == 0x4B37.
inline uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = SCM_CRC_INIT;
    for (size_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)data[pos];
        for (int i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) { crc >>= 1; crc ^= SCM_CRC_POLY; }
            else                     { crc >>= 1; }
        }
    }
    return crc;
}

// --- BCD -----------------------------------------------------------------------
/// Побайтовый BCD -> число: байт 0x23 == цифры 2 и 3.
/// bcdToF({0x23,0x10}, 10) == 231.0 (напряжение, U×10).
/// Монолит, продакшн. Переполнения не боимся: максимум 4 байта = 8 цифр,
/// float держит 7 значащих — для энергии вызывающий обязан брать
/// bcdToU32 и копейки (урок 5), см. parseEnergyTotal.
inline float bcdToF(const uint8_t* data, uint8_t len, float divider) {
    float result = 0.0f;
    for (uint8_t i = 0; i < len; i++) {
        result = result * 100.0f + (float)((data[i] >> 4) * 10 + (data[i] & 0x0F));
    }
    return result / divider;
}

/// То же в целых «сотых» (Вт·ч, сотые кВт·ч) — деньги без float-дрейфа.
inline uint32_t bcdToU32(const uint8_t* data, uint8_t len) {
    uint32_t result = 0;
    for (uint8_t i = 0; i < len; i++) {
        result = result * 100u + (uint32_t)((data[i] >> 4) * 10 + (data[i] & 0x0F));
    }
    return result;
}

/// Серийный номер (десятичная строка до 8 цифр) -> 4 байта BCD-адреса.
/// Дополнение нулями СЛЕВА (урок 1: 12345678 -> 12 34 56 78).
inline void addrFromSerial(uint32_t serial, uint8_t out[SCM_ADDR_LEN]) {
    char digits[9];
    // 8 цифр с ведущими нулями, без snprintf-локалей и String
    for (int i = 7; i >= 0; i--) { digits[i] = (char)('0' + (serial % 10u)); serial /= 10u; }
    digits[8] = '\0';
    for (int i = 0; i < SCM_ADDR_LEN; i++) {
        out[i] = (uint8_t)(((digits[i * 2] - '0') << 4) | (digits[i * 2 + 1] - '0'));
    }
}

// --- СБОРКА ЗАПРОСОВ -------------------------------------------------------------
/// Кадр запроса: [адрес 4][код][параметры...][CRC lo][CRC hi].
/// Возвращает длину кадра или 0 при переполнении out.
inline size_t buildRequest(const uint8_t addr[SCM_ADDR_LEN], uint8_t cmd,
                           const uint8_t* params, uint8_t paramsLen,
                           uint8_t* out, size_t outCap) {
    size_t len = SCM_ADDR_LEN + 1 + paramsLen + 2;
    if (outCap < len) return 0;
    memcpy(out, addr, SCM_ADDR_LEN);
    out[SCM_ADDR_LEN] = cmd;
    if (paramsLen) memcpy(out + SCM_ADDR_LEN + 1, params, paramsLen);
    uint16_t crc = crc16(out, len - 2);
    out[len - 2] = (uint8_t)(crc & 0xFF);   // младший байт первым (Modbus)
    out[len - 1] = (uint8_t)(crc >> 8);
    return len;
}

/// Запрос энергии 0x27: month 0x00 — текущие, 0x01..0x0C — на начало месяца,
/// 0x0F — на начало суток (монолит использовал текущие).
inline size_t buildEnergyRequest(const uint8_t addr[SCM_ADDR_LEN], uint8_t month,
                                 uint8_t* out, size_t outCap) {
    return buildRequest(addr, SCM_CMD_ENERGY, &month, 1, out, outCap);
}

/// Запрос суточного профиля 0x37: [0x37][0x00][DD][MM][YY] всё BCD
/// (байтовая сетка из продакшн-монолита readPowerProfileForDate).
inline size_t buildProfileRequest(const uint8_t addr[SCM_ADDR_LEN],
                                  uint8_t day, uint8_t month, uint8_t year2,
                                  uint8_t* out, size_t outCap) {
    uint8_t p[4] = {
        0x00,
        (uint8_t)(((day / 10) << 4) | (day % 10)),
        (uint8_t)(((month / 10) << 4) | (month % 10)),
        (uint8_t)(((year2 / 10) << 4) | (year2 % 10))
    };
    return buildRequest(addr, SCM_CMD_PROFILE, p, 4, out, outCap);
}

// --- РАЗБОР ОТВЕТОВ ---------------------------------------------------------------
/// Целостность кадра ответа: CRC-резидуал == 0 (урок 3) и эхо кода команды.
/// Адрес намеренно НЕ сверяем здесь: на шине один прибор, а сверка адреса —
/// политика вызывающего (при автопоиске адрес ещё неизвестен, см. 0x2F).
inline bool isFrameValid(const uint8_t* buf, size_t len, uint8_t expectCmd) {
    if (len < SCM_ADDR_LEN + 1 + 2) return false;
    if (buf[SCM_ADDR_LEN] != expectCmd) return false;
    return crc16(buf, len) == 0;
}

/// Ответ 0x63: U 2 байта BCD (×10), I 2 байта (×100), P 3 байта (×100) —
/// сетка продакшн-монолита. «Журнал» рисует I в 3 байта — на стенде C1
/// сверить с реальным прибором (урок 1); парсер благодаря CRC-резидуалу
/// от длины не зависит, смещения — зависят.
struct Instant { float voltage_v; float current_a; float power_w; };
inline bool parseInstant(const uint8_t* buf, size_t len, Instant& out) {
    if (len < SCM_MINLEN_INSTANT || !isFrameValid(buf, len, SCM_CMD_INSTANT)) return false;
    out.voltage_v = bcdToF(buf + 5, 2, 10.0f);
    out.current_a = bcdToF(buf + 7, 2, 100.0f);
    out.power_w   = bcdToF(buf + 9, 3, 100.0f);
    return true;
}

/// Ответ 0x27: 4 тарифа по 4 байта BCD (цена деления 0.01 кВт·ч).
/// Берём только суммарный тариф T1, как монолит (многотарифность — решение
/// владельца: не нужна, см. концепт-ноту §8). Возврат в сотых кВт·ч —
/// без float-дрейфа на больших показаниях (урок 5).
inline bool parseEnergyTotal(const uint8_t* buf, size_t len, uint32_t& outKwhX100) {
    if (len < SCM_MINLEN_ENERGY || !isFrameValid(buf, len, SCM_CMD_ENERGY)) return false;
    outKwhX100 = bcdToU32(buf + 5, 4);
    return true;
}

/// Ответ 0x2F: серийный номер + дата выпуска. Урок C1 (host-тесты):
/// продакшн-монолит читает цифры SN из байт [2..7] ответа — при кадре
/// «адрес+код+SN+дата» это задевало бы сам код 0x2F, т.е. реальный кадр
/// ответа, по-видимому, БЕЗ эха кода команды (адрес+SN+дата+CRC = 13 байт).
/// «Журнал» здесь молчит (урок 1). Поэтому парсер допускает ОБЕ раскладки:
/// эхо кода есть (SN с байта 5) или нет (SN с байта 4). Окончательная
/// сверка — на стенде C1 с живым прибором.
/// outSn — буфер >= 9 символов (8 цифр + NUL); ведущие нули срезаются.
inline bool parseSerial(const uint8_t* buf, size_t len,
                        uint8_t outAddr[SCM_ADDR_LEN], char* outSn, size_t outSnCap) {
    if (len < SCM_MINLEN_SERIAL || outSnCap < 9) return false;
    if (crc16(buf, len) != 0) return false;                 // целостность
    bool hasEcho = (buf[SCM_ADDR_LEN] == SCM_CMD_SERIAL);
    size_t minLen = SCM_ADDR_LEN + (hasEcho ? 1 : 0) + 4 + 3 + 2;
    if (len < minLen) return false;
    const uint8_t* sn = buf + SCM_ADDR_LEN + (hasEcho ? 1 : 0);
    memcpy(outAddr, buf, SCM_ADDR_LEN);                     // эхо адреса = прицеп
    char tmp[9]; int n = 0;
    for (int i = 0; i < 4; i++) {
        tmp[n++] = (char)('0' + ((sn[i] >> 4) & 0x0F));
        tmp[n++] = (char)('0' + (sn[i] & 0x0F));
    }
    tmp[n] = '\0';
    const char* p = tmp; while (p[0] == '0' && p[1] != '\0') p++; // срез ведущих нулей
    strcpy(outSn, p);
    return outSn[0] != '\0';
}

/// Ответ 0x37: пары байт big-endian «мощность × 0.1 Вт», до 48 получасовок,
/// терминатор FF FF (сетка продакшн-монолита). Энергия получасовки = P×0.5ч.
/// out — массив >= 48. Возвращает число точек или -1 при битом кадре.
inline int parseProfile(const uint8_t* buf, size_t len, float* outPowerW, int maxPoints) {
    if (len < 7 || !isFrameValid(buf, len, SCM_CMD_PROFILE)) return -1;
    int count = 0;
    for (size_t i = 5; i + 1 < len - 2 && count < maxPoints && count < 48; i += 2) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xFF) break;
        uint16_t raw = (uint16_t)((buf[i] << 8) | buf[i + 1]);
        outPowerW[count++] = (float)raw * 0.1f;
    }
    return count;
}

// --- АВТОМАТ ОПРОСА (СОСТОЯНИЯ) ---------------------------------------------------
/// Чистые состояния FSM из монолита; сам переход по таймерам и Serial2 —
/// в MercuryDriver (драйвер профиля), здесь только тип и бюджеты, чтобы
/// host-тест мог гонять логику «что спрашиваем следующим» без железа.
enum class Fsm : uint8_t { Idle, WaitInstant, WaitEnergy, WaitSerial, WaitProfile };

constexpr uint32_t SCM_PERIOD_INSTANT_MS = 5000;   // U/I/P каждые 5 с (монолит)
constexpr uint32_t SCM_PERIOD_ENERGY_MS  = 30000;  // энергия каждые 30 с
constexpr uint32_t SCM_TIMEOUT_MS        = 400;    // таймаут ответа (монолит)
constexpr uint32_t SCM_BUS_PAUSE_MS      = 100;    // пауза между кадрами на шине

/// Кого спрашивать следующим в Idle: мгновенные чаще, энергия реже.
/// Приоритет у мгновенных — как в монолите (иначе при пересечении периодов
/// энергия «задавила» бы оперативный канал мощности для ПАЗ).
inline Fsm nextPoll(uint32_t nowMs, uint32_t lastInstantMs, uint32_t lastEnergyMs) {
    if (nowMs - lastInstantMs >= SCM_PERIOD_INSTANT_MS) return Fsm::WaitInstant;
    if (nowMs - lastEnergyMs  >= SCM_PERIOD_ENERGY_MS)  return Fsm::WaitEnergy;
    return Fsm::Idle;
}

} // namespace scm
