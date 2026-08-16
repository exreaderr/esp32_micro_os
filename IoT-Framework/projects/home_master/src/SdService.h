// ============================================================================
// SdService.h — ВЛАДЕЛЕЦ microSD-ХРАНИЛИЩА МАСТЕРА (M0)
// ============================================================================
// Третий ярус хранения платформы (NVS → LittleFS → SD): журнал событий,
// бэкапы конфигураций парка, OTA-репозиторий (этапы M3/M4 концепции).
// M0 — фундамент: монтирование, состояние, ПАЗ, ремоунт.
//
// Аппаратно: слот microSD на ОТДЕЛЬНОЙ от W5500 SPI-шине (SPI3_HOST,
// пины из BoardDesc) — журнал и сеть не делят пропускную способность.
//
// Политика деградации: отсутствие/сбой карты НЕ фатальны — мастер живёт
// как «тонкий мост» (концепция §4.1, уровни A3), журнал просто не ведётся.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include <SPI.h>
#include <SD.h>

/// Состояние хранилища (публикуется в hm_ev::sdStateChanged, в ПАЗ и UI).
enum class SdState : uint8_t {
    Disabled = 0,   // sd.enabled=false — осознанно выключена (вердикт Ok)
    NoCard   = 1,   // карта отсутствует (ПАЗ: Warning)
    Failed   = 2,   // карта есть, но не монтируется (ПАЗ: Critical)
    Mounted  = 3,   // работаем
};

class SdService : public ModuleBase {
public:
    static SdService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "SdService"; }
    const char* getVersion() const override { return "0.1.0-m0"; }
    ModuleId getModuleId() const override { return 0x1101; }   // блок профиля home_master
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- Состояние (для UI/ПАЗ/будущего журнала) -------------------------
    SdState state() const { return _state; }
    const char* stateStr() const;
    const char* cardTypeStr() const { return _cardType; }
    uint64_t sizeMb() const { return _sizeMb; }
    uint64_t usedMb() const { return _usedMb; }

    /// Файловая система карты. nullptr, если не Mounted — потребитель
    /// (журнал M3) обязан проверять и деградировать, а не падать.
    fs::FS* fs() { return _state == SdState::Mounted ? (fs::FS*)&SD : nullptr; }

    /// Попытка (пере)монтирования: из init, из tick (раз в минуту при
    /// сбое) и из UI-кнопки «Перемонтировать». true — карта смонтирована.
    bool tryMount();

private:
    SdService() : _spi(SD_SPI_HOST) {}

    void setState(SdState s);

    // SPI-шина карты в нумерации Arduino-SPIClass. Обязана совпадать с
    // BoardDesc.sdSpiHost (проверяется в init — расхождение = ошибка
    // конфигурации платформы, Error в лог).
    // ВНИМАНИЕ, две системы координат (ядро 3.3.x, esp32-hal-spi.h):
    //   · Arduino SPIClass на S3: FSPI=0 (шина SPI2), HSPI=1 (шина SPI3);
    //     SPIClass(2) — НЕСУЩЕСТВУЮЩАЯ шина (баг M0: карта молчала на CMD0).
    //   · IDF spi_host_device_t (его ест ETH.begin для W5500):
    //     SPI2_HOST=1, SPI3_HOST=2.
    // SD на периферии SPI3 → Arduino HSPI = 1. W5500 на SPI2 — шины РАЗНЫЕ.
    static constexpr uint8_t SD_SPI_HOST = 1;

    static constexpr uint32_t REMOUNT_INTERVAL_MS = 60000;  // пауза ретраев

    SPIClass _spi;
    SdState  _state = SdState::Disabled;
    char     _cardType[8] = "";   // "MMC"/"SDSC"/"SDHC"/"SDXC"/""
    uint64_t _sizeMb = 0;
    uint64_t _usedMb = 0;
    uint32_t _lastTryMs = 0;
};
