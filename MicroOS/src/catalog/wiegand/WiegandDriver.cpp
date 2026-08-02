// ============================================================================
// WiegandDriver.cpp — универсальный драйвер Wiegand (26–56 бит)
// ============================================================================
#include "WiegandDriver.h"
#include "../../core/Events.h"

WiegandDriver& WiegandDriver::getInstance() {
    static WiegandDriver instance;
    return instance;
}

// Указатель на экземпляр для ISR. ВАЖНО: нельзя звать getInstance() из
// IRAM-кода — функциональный static использует guard-переменную (thread-safe
// init C++11), и линкер роняет сборку с "dangerous relocation: literal
// placed after use". Обычный файловый указатель такой проблемы не имеет.
static WiegandDriver* s_isrInstance = nullptr;

// ============================================================================
// КОНФИГУРАЦИЯ
// ============================================================================
void WiegandDriver::configure(uint8_t pinD0, uint8_t pinD1, const Config& cfg) {
    _pinD0 = pinD0;
    _pinD1 = pinD1;
    _cfg = cfg;
    _configured = true;
}

// ============================================================================
// INIT
// ============================================================================
bool WiegandDriver::init() {
    if (!_configured) return false;   // ошибка профиля, не железа

    _isrBits = 0;
    _isrCount = 0;
    _isrLastBitMs = 0;

    pinMode(_pinD0, INPUT_PULLUP);
    pinMode(_pinD1, INPUT_PULLUP);
    // Указатель для ISR — до установки обработчиков
    s_isrInstance = this;

    attachInterrupt(digitalPinToInterrupt(_pinD0), isrD0, FALLING);
    attachInterrupt(digitalPinToInterrupt(_pinD1), isrD1, FALLING);

    _healthy = true;
    return true;
}

// ============================================================================
// ISR: минимальная работа — только накопить бит (IRAM, без malloc/логов)
// ============================================================================
void IRAM_ATTR WiegandDriver::isrD0() { if (s_isrInstance) s_isrInstance->onBit(0); }
void IRAM_ATTR WiegandDriver::isrD1() { if (s_isrInstance) s_isrInstance->onBit(1); }

inline void IRAM_ATTR WiegandDriver::onBit(uint8_t bit) {
    // ВАЖНО: millis() из IRAM вызывать нельзя — функция во flash, её адрес
    // грузится через literal pool -> "dangerous relocation" на линковке.
    // xTaskGetTickCountFromISR() резидентен в IRAM (FreeRTOS).
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;

    // Пауза больше таймаута кадра => начался новый кадр
    if (now - _isrLastBitMs > _cfg.frameTimeoutMs) {
        _isrBits = 0;
        _isrCount = 0;
    }
    _isrLastBitMs = now;

    if (_isrCount < WIEGAND_MAX_BITS) {
        _isrBits = (_isrBits << 1) | bit;
        _isrCount++;
    }
    // Переполнение (>64 бит) — мусор на линии: кадр будет отброшен
    // декодером (длина неизвестна ни одному формату).
}

// ============================================================================
// POLL: финализация кадра по таймауту (контекст задачи)
// ============================================================================
void WiegandDriver::poll() {
    if (_isrCount == 0) return;
    if (millis() - _isrLastBitMs < _cfg.frameTimeoutMs) return;

    noInterrupts();
    uint64_t bits = _isrBits;
    uint8_t count = _isrCount;
    _isrBits = 0;
    _isrCount = 0;
    interrupts();

    processFrame(bits, count);
}

// ============================================================================
// ДЕКОДЕР: делегаты к чистой логике WiegandFormats.h (D2: тесты на хосте
// покрывают wiegand::decodeFrame — тот же код, что выполняет железо)
// ============================================================================
const WiegandFormat* WiegandDriver::findFormat(uint8_t totalBits) {
    return wiegand::findFormatByBits(totalBits);
}

WiegandCard WiegandDriver::decode(uint64_t bits, uint8_t count) {
    return wiegand::decodeFrame(bits, count);
}

// ============================================================================
// ОБРАБОТКА КАДРА
// ============================================================================
void WiegandDriver::processFrame(uint64_t bits, uint8_t count) {
    // --- Антишум ---------------------------------------------------------------
    if (count < _cfg.minNoiseBits) {
        ShEventData d; d.clear();
        d.code = count;
        EventBus::getInstance().post(DRV_EVENT_WIEGAND_NOISE, &d);
        return;
    }

    WiegandCard card = decode(bits, count);

    // --- Неизвестная длина кадра ----------------------------------------------
    if (card.format == nullptr) {
        if (_cfg.rawFallback && count <= WIEGAND_MAX_BITS) {
            card.data = bits;       // публикуем как есть (профиль разберётся)
            publishCard(card);
        } else {
            ShEventData d; d.clear();
            d.code = count;
            EventBus::getInstance().post(DRV_EVENT_WIEGAND_NOISE, &d);
        }
        return;
    }

    // --- Паритет ------------------------------------------------------------------
    if (!card.parityOk && _cfg.strictParity) {
        ShEventData d; d.clear();
        d.code = count;   // длина кадра — диагностика считывателя
        EventBus::getInstance().post(DRV_EVENT_WIEGAND_NOISE, &d);
        return;
    }

    // --- Антидребезг: та же карта в окне repeatWindowMs --------------------------
    uint32_t now = millis();
    if (card.data == _lastCardData && now - _lastCardMs < _cfg.repeatWindowMs) {
        return;
    }
    _lastCardData = card.data;
    _lastCardMs = now;

    publishCard(card);
}

// ============================================================================
// ПУБЛИКАЦИЯ КАРТЫ
// ============================================================================
// payload: HEX верхнего регистра, до 16 цифр (64 бита). Формат монолита
// (6 цифр для W26) сохраняется как частный случай — ведущие нули HEX-строки
// для карт до 24 бит дают ту же строку "00AB12" при сравнении хвоста.
// code = длина кадра; если паритет был проигнорирован (strictParity=false),
// code отрицательный — подписчик видит сомнительную карту.
// ============================================================================
void WiegandDriver::publishCard(const WiegandCard& card) {
    ShEventData d; d.clear();
    d.code = card.parityOk ? (int32_t)card.bitCount
                           : -(int32_t)card.bitCount;

    if (card.data <= 0xFFFFFFULL) {
        snprintf(d.payload, sizeof(d.payload), "%06lX",
                 (unsigned long)card.data);
    } else if (card.data <= 0xFFFFFFFFULL) {
        snprintf(d.payload, sizeof(d.payload), "%08lX",
                 (unsigned long)card.data);
    } else {
        // >32 бит: 64-битный HEX (16 цифр)
        snprintf(d.payload, sizeof(d.payload), "%08lX%08lX",
                 (unsigned long)(card.data >> 32),
                 (unsigned long)(card.data & 0xFFFFFFFFULL));
    }
    EventBus::getInstance().post(DRV_EVENT_WIEGAND_CARD, &d);
}
