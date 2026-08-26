// ============================================================================
// Cc1101Driver.cpp — реализация приёмника CC1101 (receive-only)
// ============================================================================
#include "Cc1101Driver.h"
#include <services/ConfigService.h>
#include <soc/gpio_struct.h>
#include <cstring>

// --- СТАТИКА ISR (файловая область — правило ISR платформы) -------------------
volatile uint16_t Cc1101Driver::_wHead = 0;
volatile uint16_t Cc1101Driver::_rTail = 0;
volatile bool     Cc1101Driver::_csActive = false;
Cc1101Driver::Edge Cc1101Driver::_ring[CC1101_EDGE_RING];

// Метка межпакетного зазора в кольце (шторка закрылась) — декодер сбросится
static constexpr uint16_t EDGE_GAP = 0xFFFF;

// Отсечка ISR GDO0 (файловая статика — перевзводится при открытии шторки)
static uint32_t s_lastUs  = 0;
static uint8_t  s_lastLvl = 0;
// Счётчик потерянных фронтов (переполнение кольца при ОТКРЫТОЙ шторке).
// Пишется из ISR, переносится в метрику драйвера из poll() — getInstance()
// в ISR запрещён. Шум закрытой шторки сюда не попадает никогда.
static volatile uint32_t s_edgesDropped = 0;

// Пины GDO для ISR — файловые статики (init копирует из _pins; обращение
// к членам через getInstance() в ISR запрещено правилами платформы).
static uint8_t s_pinGdo0 = 0xFF;
static uint8_t s_pinGdo2 = 0xFF;

// Маскирование прерывания GDO0 (5.8.4, стенд weather_gate): сырой
// демодулятор в async-режиме выдаёт 115-230 тыс. шумовых фронтов/с между
// пакетами. ISR с ранним выходом — это всё равно сотни тысяч пробуждений
// CPU в секунду впустую. Гасим само прерывание, пока шторка Carrier Sense
// закрыта; взводим при её открытии. Только прямая запись в регистр
// GPIO.pin[].int_ena — gpio_intr_enable/disable из IDF НЕ в IRAM, а шторм
// легко поймает окно записи flash (урок: отладка-по-регистрам).
// attachInterrupt/detachInterrupt в ISR запрещены (локи, heap).
static volatile uint32_t s_gdo0IntEna = 0;   // снимок после attachInterrupt

static inline void IRAM_ATTR gdo0IntMask() {
    GPIO.pin[s_pinGdo0].int_ena = 0;
}
static inline void IRAM_ATTR gdo0IntUnmask() {
    GPIO.pin[s_pinGdo0].int_ena = s_gdo0IntEna;
}

Cc1101Driver& Cc1101Driver::getInstance() {
    static Cc1101Driver instance;
    return instance;
}

// ============================================================================
// ISR: GDO2 (Carrier Sense) — шторка; GDO0 — фронты данных
// ============================================================================
void IRAM_ATTR Cc1101Driver::isrGdo2() {
    _csActive = (digitalRead(s_pinGdo2) == HIGH);
    if (_csActive) {
        // Шторка открылась: взводим фронты данных (маскированы между
        // пакетами) и перевзводим отсечку — первый фронт пакета не
        // должен унаследовать длительность тишины до него.
        gdo0IntUnmask();
        s_lastUs  = micros();
        s_lastLvl = (digitalRead(s_pinGdo0) == HIGH) ? 1 : 0;
    } else {
        // Шторка закрылась: глушим прерывание GDO0 (сырой демодулятор
        // шумит 115-230 тыс. фронтов/с — нечего будить CPU), затем
        // метка зазора — декодер сбросится на feed().
        gdo0IntMask();
        uint16_t next = (uint16_t)(_wHead + 1) % CC1101_EDGE_RING;
        if (next != _rTail) {
            _ring[_wHead] = { EDGE_GAP, 0 };
            _wHead = next;
        }
    }
}

void IRAM_ATTR Cc1101Driver::isrGdo0() {
    if (!_csActive) return;                 // шторка закрыта — эфирный шум
    // Длительность завершившегося уровня. micros() в ISR допустим
    // (чтение таймера, без heap/объектов); millis() в ISR — запрещён.
    uint32_t now = micros();
    uint8_t lvl = (digitalRead(s_pinGdo0) == HIGH) ? 1 : 0;
    uint32_t dur = now - s_lastUs;
    if (dur >= EDGE_GAP) dur = EDGE_GAP - 1;   // кламп: 0xFFFF = метка зазора
    uint16_t next = (uint16_t)(_wHead + 1) % CC1101_EDGE_RING;
    if (next != _rTail) {
        _ring[_wHead] = { (uint16_t)dur, s_lastLvl };
        _wHead = next;
    } else {
        ++s_edgesDropped;   // кольцо полно — фронт лучше потерять,
                            // но метрику сохранить (урок тихих потерь)
    }
    s_lastUs  = now;
    s_lastLvl = lvl;
}

// ============================================================================
// INIT
// ============================================================================
bool Cc1101Driver::init() {
    // Пины — только из configurePins() (профиль вызывает до регистрации).
    // Без них драйвер не инициализируется: молчаливый дефолт на чужих
    // ногах хуже честного отказа.
    if (!_pinsSet) { _healthy = false; return false; }
    _freqMHz = cfgGetFloat("wx.rf_freq_mhz", 915.0f);

    pinMode(_pins.cs,   OUTPUT);
    digitalWrite(_pins.cs, HIGH);          // CS idle HIGH — отпущен
    pinMode(_pins.gdo0, INPUT);            // input-only; подтяжки нет — CC1101 push-pull
    pinMode(_pins.gdo2, INPUT);

    // SPI: хост HSPI задаём ЯВНО (правило нумерации classic ESP32).
    // new(std::nothrow) — heap-блок один раз из init (урок outbox/BSS).
    _spi = new (std::nothrow) SPIClass(HSPI);
    if (_spi == nullptr) { _healthy = false; return false; }
    _spi->begin((int8_t)_pins.sck, (int8_t)_pins.miso,
                (int8_t)_pins.mosi, (int8_t)_pins.cs);

    _healthy = detectChip();
    if (!_healthy) return false;

    writeRxTable();

    // Рабочий режим — только приём. STX не существует в этом драйвере.
    xferReg(cc1101::STROBE_SIDLE, 0);
    xferReg(cc1101::STROBE_SFRX, 0);
    xferReg(cc1101::STROBE_SRX, 0);

    // Пины для ISR — в файловые статики (до attachInterrupt).
    s_pinGdo0 = _pins.gdo0;
    s_pinGdo2 = _pins.gdo2;
    attachInterrupt(digitalPinToInterrupt(_pins.gdo2), isrGdo2, CHANGE);
    attachInterrupt(digitalPinToInterrupt(_pins.gdo0), isrGdo0, CHANGE);
    // Маскирование GDO0 (5.8.4): снимок int_ena ПОСЛЕ attachInterrupt,
    // стартовое состояние — по фактическому уровню шторки.
    s_gdo0IntEna = GPIO.pin[s_pinGdo0].int_ena;
    if (digitalRead(_pins.gdo2) == HIGH) {
        _csActive = true;              // несущая/шум уже есть — шторка открыта
    } else {
        gdo0IntMask();                 // тишина — спим до первого фронта GDO2
    }
    return true;
}

// ============================================================================
// POLL: дрейн кольца -> декодер -> дедупликация пары (31 мс)
// ============================================================================
void Cc1101Driver::poll() {
    if (!_healthy) return;
    _edgesDropped = s_edgesDropped;

    fo::WeatherPacket pkt;
    while (_rTail != _wHead) {
        const Edge e = _ring[_rTail];
        _rTail = (uint16_t)(_rTail + 1) % CC1101_EDGE_RING;
        uint16_t dur = (e.durUs == EDGE_GAP) ? fo::GAP_US : e.durUs;
        if (_dec.feed(dur, e.level, pkt)) {
            // Пакет собран. Дедупликация: станция шлёт пару за 31 мс —
            // идентичные 10 байт в пределах 2 с = второй экземпляр пары.
            // Считаем отдельно, данные НЕ перетираем повтором.
            uint32_t now = millis();
            bool dup = (_pktSeq != 0) && (now - _lastPktMs < 2000) &&
                       (memcmp(_lastRaw, _dec.lastRaw(), sizeof(_lastRaw)) == 0);
            if (dup) {
                ++_dupSeq;
            } else {
                memcpy(_lastRaw, _dec.lastRaw(), sizeof(_lastRaw));
                _lastPkt = pkt;
                _lastPktMs = now;
                ++_pktSeq;
                _rssiDbm = readRssiDbm();
            }
        }
    }
}

// ============================================================================
// НИЗКИЙ УРОВЕНЬ SPI
// ============================================================================
uint8_t Cc1101Driver::xferReg(uint8_t addr, uint8_t val) {
    _spi->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_pins.cs, LOW);
    uint8_t status = _spi->transfer(addr);
    _spi->transfer(val);
    digitalWrite(_pins.cs, HIGH);
    _spi->endTransaction();
    return status;
}

uint8_t Cc1101Driver::readStatus(uint8_t addr) {
    _spi->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_pins.cs, LOW);
    _spi->transfer((uint8_t)(addr | 0xC0));
    uint8_t v = _spi->transfer(0x00);
    digitalWrite(_pins.cs, HIGH);
    _spi->endTransaction();
    return v;
}

bool Cc1101Driver::detectChip() {
    // Сброс, затем идентификация. 0x00/0xFF в VERSION = «чип молчит»
    // (обрыв SPI, нет питания модуля, чужой чип).
    xferReg(cc1101::STROBE_SRES, 0);
    delay(5);
    uint8_t pn  = readStatus(cc1101::REG_PARTNUM);
    uint8_t ver = readStatus(cc1101::REG_VERSION);
    return pn == cc1101::PARTNUM_EXPECT &&
           (ver == cc1101::VERSION_EXPECT || ver == cc1101::VERSION_LEGACY);
}

void Cc1101Driver::writeRxTable() {
    xferReg(cc1101::STROBE_SIDLE, 0);
    size_t n = 0;
    const cc1101::RegVal* t = cc1101::rxTableBase(n);
    for (size_t i = 0; i < n; ++i) xferReg(t[i].reg, t[i].val);
    // FREQ2/1/0 из конфигурируемой частоты (по умолчанию 915.00 -> 0x23313B)
    writeFreqRegs(_freqMHz);
    // AGCCTRL1 (пороги Carrier Sense) — полевой настроечный байт (5.8.4).
    // Шторм на стенде показал: с reset-дефолтом (относительный порог 0 дБ)
    // шторка открывается на собственный шум. Сырой байт, а не перечень
    // полей: кодировку порогов подбирают на стенде по даташиту/SmartRF.
    // 0 (дефолт) = регистр не трогаем. Бит 6 (AGC_LNA_PRIORITY) держать
    // выставленным, как в reset-значении 0x40.
    int32_t agc = cfgGetInt("wx.rf_agcctrl1", 0);
    if (agc > 0 && agc <= 0xFF) xferReg(0x1C, (uint8_t)agc);
}

void Cc1101Driver::writeFreqRegs(float mhz) {
    uint32_t f = cc1101::freqWord(mhz);
    xferReg(0x0D, (uint8_t)(f >> 16));
    xferReg(0x0E, (uint8_t)(f >> 8));
    xferReg(0x0F, (uint8_t)f);
}

// ============================================================================
// W3.3: ДЕЛЬТА ДЛЯ СКАНЕРА АЧХ (task-контекст; SPI, нельзя из ISR)
// ============================================================================
bool Cc1101Driver::setFreqMHz(float mhz) {
    if (!_healthy) return false;
    if (mhz < 300.0f || mhz > 928.0f) return false;   // пределы CC1101 (900-диап.)
    _freqMHz = mhz;
    xferReg(cc1101::STROBE_SIDLE, 0);
    writeFreqRegs(mhz);
    xferReg(cc1101::STROBE_SFRX, 0);
    xferReg(cc1101::STROBE_SRX, 0);
    return true;
}

int16_t Cc1101Driver::readRssiNow() {
    if (!_healthy) return 0;
    return readRssiDbm();
}

int16_t Cc1101Driver::readRssiDbm() {
    int8_t raw = (int8_t)readStatus(cc1101::REG_RSSI);
    return (int16_t)raw / 2 - 74;        // даташит TI: RSSI_dBm = raw/2 - 74
}
