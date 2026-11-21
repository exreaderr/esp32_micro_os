// ============================================================================
// FleetRegistryCore.h — чистая RAM-модель пассивного реестра M3.4a
// Без Arduino/ESP/FS/сети: вся идентичность и state machine проверяются на host.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace fleet {

constexpr uint8_t MAX_DEVICES = 12;
constexpr uint32_t STALE_AFTER_MS = 600000UL;
constexpr uint8_t ID_LEN = 13;
constexpr uint8_t ALIAS_LEN = 33;
constexpr uint8_t PROFILE_LEN = 24;
constexpr uint8_t VERSION_LEN = 20;
constexpr uint8_t HOST_LEN = 25;
constexpr uint8_t IP_LEN = 16;
constexpr uint8_t CLIENT_LEN = 48;

bool validId(const char* id);
bool validIpv4(const char* ip);

struct Telemetry {
    bool present = false;
    uint32_t uptime = 0;
    uint32_t heap = 0;
    uint32_t heapMin = 0;
    uint32_t busDropped = 0;
    uint16_t busHwm = 0;
    int32_t cpuTenths = 0;
    uint8_t degradation = 0;
    uint32_t gwRtt = 0;
    uint16_t bootloop = 0;
    uint32_t auditLost = 0;
    bool bootloopPresent = false;
    bool auditLostPresent = false;
};

enum class Availability : uint8_t {
    Unknown = 0,
    Online,
    Offline,
    Stale
};

struct Record {
    bool used = false;
    bool self = false;
    bool identified = false;
    bool descriptorPresent = false;
    char id[ID_LEN] = "";
    char alias[ALIAS_LEN] = "";
    char profile[PROFILE_LEN] = "";
    char kernel[VERSION_LEN] = "";
    char profileVer[VERSION_LEN] = "";
    char host[HOST_LEN] = "";
    char ip[IP_LEN] = "";
    char clientId[CLIENT_LEN] = "";

    uint32_t descriptorMs = 0;
    uint32_t descriptorUnix = 0;
    uint32_t availabilityMs = 0;
    uint32_t availabilityUnix = 0;
    uint32_t telemetryMs = 0;
    uint32_t telemetryUnix = 0;
    uint32_t lastOnlineMs = 0;

    Availability availability = Availability::Unknown;
    Telemetry telemetry;
    uint16_t identityConflicts = 0;
};

struct Counters {
    uint32_t accepted = 0;
    uint32_t rejected = 0;
    uint32_t truncated = 0;
    uint32_t identityConflicts = 0;
    uint32_t recordsRejected = 0;
    uint32_t removesIgnored = 0;
};

class Core {
public:
    void reset();
    bool setSelfId(const char* id);

    // Топик строго <prefix>/<12HEX>/(descriptor|state|telemetry).
    // self=true используется только локальным источником мастера.
    bool onPublish(const char* topic, const char* payload,
                   const char* clientId, bool truncated,
                   uint32_t nowMs, uint32_t nowUnix,
                   const char* prefix, bool self = false);

    // hasClient — фактическая проверка BrokerService на момент RemoveClient.
    void onRemoveClient(const char* clientId, bool hasClient,
                        uint32_t nowMs, uint32_t nowUnix);
    void tick(uint32_t nowMs);

    Record* find(const char* id);
    const Record* find(const char* id) const;
    const Record* self() const;
    const Record* records() const { return _records; }
    const Counters& counters() const { return _counters; }
    uint8_t count() const;

    bool setAlias(const char* id, const char* alias);
    static bool validAlias(const char* alias);
    const char* resolveIp(const char* id) const;
    static const char* availabilityName(Availability value);

private:
    enum class Kind : uint8_t { Invalid, Descriptor, State, Telemetry };

    Kind parseTopic(const char* topic, const char* prefix,
                    char id[ID_LEN]) const;
    Record* ensure(const char* id, bool self);
    void clearDescriptor(Record& r);
    bool parseDescriptor(Record& r, const char* payload);
    bool parseTelemetry(Record& r, const char* payload);
    static void stamp(uint32_t& ms, uint32_t& unix,
                      uint32_t nowMs, uint32_t nowUnix);
    static bool fresh(uint32_t stampMs, uint32_t nowMs) {
        return stampMs != 0 && (uint32_t)(nowMs - stampMs) <= STALE_AFTER_MS;
    }

    Record _records[MAX_DEVICES];
    Counters _counters;
    char _selfId[ID_LEN] = "";
};

} // namespace fleet
