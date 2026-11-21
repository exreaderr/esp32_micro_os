// ============================================================================
// FleetRegistryService.h — M3.4a, пассивный реестр флота home_master
// ----------------------------------------------------------------------------
// Только наблюдение: broker hook -> RAM, HTTP/UI <- RAM. Никаких команд,
// активного OTA, автоматического restore/backup и периодической записи на FS.
// Персистентны лишь локальные алиасы мастера; identity остаётся deviceId.
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include "BrokerService.h"
#include "FleetRegistryCore.h"

class FleetRegistryService : public ModuleBase {
public:
    static FleetRegistryService& getInstance();

    const char* getName() const override { return "FleetRegistryService"; }
    const char* getVersion() const override { return "0.1.0-m34a"; }
    ModuleId getModuleId() const override { return 0x110D; }

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 1000; }
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    const fleet::Core& core() const { return _core; }

    /// Локальный алиас мастера. Ничего не отправляет устройству.
    /// false — неизвестный id, недопустимый алиас или ошибка атомарной записи.
    bool setAlias(const char* id, const char* alias);
    const char* aliasFor(const char* id) const;

    /// Полный JSON или 0 (буфер мал/ошибка построения); молчаливая обрезка
    /// запрещена. Интеграции backup/OTA добавляются read-only снимками.
    size_t apiStatus(char* buf, size_t bufSize) const;

    static void onBrokerEvent(const BrokerEventInfo& info);

private:
    struct AliasEntry {
        bool used = false;
        char id[fleet::ID_LEN] = "";
        char alias[fleet::ALIAS_LEN] = "";
    };

    FleetRegistryService() = default;

    void refreshSelf();
    void applyStoredAliases();
    void loadAliases();
    bool saveAliases();
    AliasEntry* aliasEntry(const char* id, bool create);
    const AliasEntry* aliasEntry(const char* id) const;

    static constexpr const char* ALIASES_PATH = "/fleet_aliases.txt";
    static constexpr uint16_t ALIASES_FILE_CAP = 768;

    fleet::Core _core;
    AliasEntry _aliases[fleet::MAX_DEVICES];
    bool _selfDirty = true;
    bool _hookRegistered = false;
    bool _aliasPersist = true;
    uint32_t _lastSelfRefreshMs = 0;
};
