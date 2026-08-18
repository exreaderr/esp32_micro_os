// ============================================================================
// SdService.cpp — реализация владельца microSD (M0)
// ============================================================================
#include "SdService.h"
#include "HomeMasterEvents.h"
#include <core/EventBus.h>
#include <platform/BaseProfile.h>
#include <services/ConfigService.h>

SdService& SdService::getInstance() {
    static SdService instance;
    return instance;
}

const char* SdService::stateStr() const {
    switch (_state) {
        case SdState::Disabled: return "disabled";
        case SdState::NoCard:   return "no_card";
        case SdState::Failed:   return "failed";
        case SdState::Mounted:  return "mounted";
    }
    return "unknown";
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void SdService::init() {
    ensureMutex();

    // Страж конфигурации платформы: шина карты в коде и в BoardDesc
    // обязаны совпадать (расхождение = не та плата/правка пинов).
    const platform::BoardDesc& bd = platform::board();
    if (bd.sdPresent && bd.sdSpiHost != SD_SPI_HOST) {
        log(LogLevel::Error, "init: SPI host mismatch: board=%u code=%u",
            bd.sdSpiHost, SD_SPI_HOST);
    }

    if (!bd.sdPresent) {
        log(LogLevel::Warning, "init: board has no SD slot, storage disabled");
        setState(SdState::Disabled);
    } else if (!cfgGetBool("sd.enabled", true)) {
        log(LogLevel::Info, "init: SD disabled by config (sd.enabled=false)");
        setState(SdState::Disabled);
    } else {
        tryMount();
    }
    _initialized = true;
}

void SdService::start() {
    _started = true;
}

void SdService::stop() {
    _started = false;
}

void SdService::tick() {
    // Ремоунт с паузой: карту могли вставить на живую, а при сбое
    // контроллер карты иногда отпускает. Раз в минуту — не шторм.
    if (!_started || _state == SdState::Mounted || _state == SdState::Disabled)
        return;
    if ((uint32_t)(millis() - _lastTryMs) < REMOUNT_INTERVAL_MS) return;
    tryMount();
}

// ============================================================================
// МОНТИРОВАНИЕ
// ============================================================================
bool SdService::tryMount() {
    if (!takeMutex()) return false;
    _lastTryMs = millis();

    const platform::BoardDesc& bd = platform::board();
    if (_state == SdState::Mounted) SD.end();   // перемонтирование — чисто

    // Своя шина, свои пины (BoardDesc): SCK/MISO/MOSI/CS. W5500 на другой
    // шине — remap пинов здесь ей не страшен.
    _spi.begin(bd.sdSck, bd.sdMiso, bd.sdMosi, bd.sdCs);

    uint32_t freqHz = ConfigService::getInstance().getUInt("sd.freq_mhz", 20)
                      * 1000000UL;
    bool ok = SD.begin(bd.sdCs, _spi, freqHz, "/sd", 8, false);

    if (!ok) {
        setState(SD.cardType() == CARD_NONE ? SdState::NoCard : SdState::Failed);
        giveMutex();
        return false;
    }

    switch (SD.cardType()) {
        case CARD_MMC:  safeStrCopy(_cardType, sizeof(_cardType), "MMC");  break;
        case CARD_SD:   safeStrCopy(_cardType, sizeof(_cardType), "SDSC"); break;
        case CARD_SDHC: safeStrCopy(_cardType, sizeof(_cardType), "SDHC"); break;
        default:        _cardType[0] = '\0'; break;
    }
    _sizeMb = SD.cardSize() / (1024ULL * 1024ULL);
    _usedMb = SD.usedBytes() / (1024ULL * 1024ULL);
    setState(SdState::Mounted);
    log(LogLevel::Info, "SD mounted: %s, %llu MB (used %llu MB), %lu MHz",
        _cardType, _sizeMb, _usedMb, (unsigned long)(freqHz / 1000000UL));
    giveMutex();
    return true;
}

void SdService::setState(SdState s) {
    if (s == _state) return;
    SdState prev = _state;
    _state = s;

    // Событие домена мастера — потребители: будущий журнал (M3, пауза
    // записи), панель, ПАЗ. payload — тип карты для диагностики.
    ShEventData d; d.clear();
    d.code = (int32_t)s;
    safeStrCopy(d.payload, sizeof(d.payload), _cardType);
    postEvent(hm_ev::sdStateChanged(), &d);

    if (s != SdState::Mounted) {
        log(s == SdState::Failed ? LogLevel::Error : LogLevel::Warning,
            "SD state: %d -> %d (%s)", (int)prev, (int)s, stateStr());
    }
}
