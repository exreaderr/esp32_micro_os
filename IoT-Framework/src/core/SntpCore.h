// ============================================================================
// SntpCore.h — ЧИСТАЯ ЛОГИКА SNTP-ПАКЕТОВ (RFC 4330, клиент)
// ============================================================================
// Почему СВОЙ клиент, а не lwIP configTime (урок диагностики 5.5.6):
// lwIP SNTP на связке ESP32-S3 + W5500 (SPI, MACRAW) + core 3.3.11 молчал
// БЕССЛЕДНО: Torch на MikroTik показал, что запросы даже не покидают
// устройство, а lwIP об этом не сказал ни слова. Свой клиент — 50 строк,
// каждая под логом: отправлено/ответ/таймаут видны в журнале явно.
//
// Протокол тривиален: 48 байт запрос (0x1B = LI 0, VN 3, Mode 3=client),
// 48 байт ответ; серверное время — transmit timestamp (байты 40..43),
// эпоха NTP 1900 → unix = ts − 2208988800.
//
// Чистая логика без Arduino-зависимостей — покрыта host-тестами (D2).
// ============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

namespace sh_sntp {

constexpr size_t   PACKET_LEN   = 48;
constexpr uint8_t  MODE_CLIENT  = 3;
constexpr uint8_t  MODE_SERVER  = 4;
constexpr uint8_t  VERSION_3    = 3;
constexpr uint32_t EPOCH_OFFSET = 2208988800UL;  // 1900 → 1970

/// Собрать SNTP-запрос. out — буфер минимум PACKET_LEN.
inline void buildRequest(uint8_t* out) {
    for (size_t i = 0; i < PACKET_LEN; ++i) out[i] = 0;
    out[0] = (uint8_t)((0 << 6) | (VERSION_3 << 3) | MODE_CLIENT);  // 0x1B
}

/// Разобрать ответ сервера. true + outUnixSec — валидный ответ.
/// Отбраковка: длина, режим server(4), stratum ≥ 1 (0 = kiss-o'-death),
/// timestamp не нулевой и не раньше 2025-01-01 (защита от мусора).
inline bool parseReply(const uint8_t* buf, size_t len, uint32_t& outUnixSec) {
    if (buf == nullptr || len < PACKET_LEN) return false;
    const uint8_t mode = buf[0] & 0x07;
    if (mode != MODE_SERVER) return false;
    if (buf[1] == 0) return false;                    // stratum 0 — отказ
    const uint32_t ts = ((uint32_t)buf[40] << 24) | ((uint32_t)buf[41] << 16) |
                        ((uint32_t)buf[42] << 8)  |  (uint32_t)buf[43];
    if (ts < EPOCH_OFFSET) return false;              // мусор/нулевой
    const uint32_t unix = ts - EPOCH_OFFSET;
    if (unix < 1735689600UL) return false;            // раньше 2025-01-01
    outUnixSec = unix;
    return true;
}

} // namespace sh_sntp
