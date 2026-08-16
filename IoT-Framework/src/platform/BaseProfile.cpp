// ============================================================================
// BaseProfile.cpp — ТАБЛИЦА ПЛАТ ПЛАТФОРМЫ (5.3.0, A4: вторая плата)
// ============================================================================
// Единственное место, где описано железо каждой платы. Пины — по схемам
// вендоров: WT32-ETH01 (LAN8720, каноника монолита v2.5.0) и Waveshare
// ESP32-S3-POE-ETH (W5500 + microSD, вики Waveshare — вилка пинов
// зафиксирована в шапке BaseProfile.h).
// ============================================================================
#include "BaseProfile.h"

namespace platform {

// SPI-хосты числом (spi_host_device_t): SPI2_HOST=1 (FSPI), SPI3_HOST=2
// (HSPI). Числом, чтобы BaseProfile.h не тянул esp32-hal-spi.h в ядро.

static const BoardDesc BOARD_WT32 = {
    BoardId::Wt32Eth01, "WT32-ETH01",
    I2C_SDA_PIN, I2C_SCL_PIN,                    // 32, 33
    EthKind::RmiiLan8720,
    (int8_t)ETH_PHY_POWER_PIN,                   // 16 — питание PHY
    -1, -1, -1,                                  // SPI не используется
    -1, -1, -1, 0,
    false, -1, -1, -1, -1, 0                     // SD нет
};

static const BoardDesc BOARD_S3_POE = {
    BoardId::Esp32S3PoeEth, "ESP32-S3-POE-ETH",
    16, 17,                                      // I2C DS3231 (выбор M0)
    EthKind::SpiW5500,
    -1,                                          // power-пина нет
    14, 10, 9,                                   // CS, INT, RST
    13, 12, 11,                                  // SCK, MISO, MOSI
    1,                                           // ETH: IDF SPI2_HOST=1 (ETH.begin ест IDF-нумерацию)
    true,
    4, 7, 5, 6,                                  // SD: CS, SCK, MISO, MOSI
    1                                            // SD: Arduino HSPI=1 = периферия SPI3 (ядро 3.3.x: FSPI=0, HSPI=1 на S3!)
};

// ESP32-C3 SuperMini: узел шины RS485 (Помощник smart_light и др.).
// Сети нет фактом железа → EthKind::None → NetworkService сам уходит
// в локальный режим (честный AUTONOMOUS), HTTP/MQTT не стартуют.
// I2C 6/7 — выбор платформы (см. документацию пинов в BaseProfile.h).
static const BoardDesc BOARD_C3_MINI = {
    BoardId::Esp32C3SuperMini, "ESP32-C3-SuperMini",
    6, 7,                                      // I2C: SDA, SCL
    EthKind::None,
    -1,                                        // power-пина нет
    -1, -1, -1,                                // SPI ETH отсутствует
    -1, -1, -1, 0,
    false, -1, -1, -1, -1, 0                   // SD нет
};

static const BoardDesc* g_current = &BOARD_WT32;

const BoardDesc& boardDesc(BoardId id) {
    switch (id) {
        case BoardId::Esp32S3PoeEth:    return BOARD_S3_POE;
        case BoardId::Esp32C3SuperMini: return BOARD_C3_MINI;
        default:                        return BOARD_WT32;
    }
}

const BoardDesc& board() {
    return *g_current;
}

void selectBoard(BoardId id) {
    g_current = &boardDesc(id);
}

} // namespace platform
