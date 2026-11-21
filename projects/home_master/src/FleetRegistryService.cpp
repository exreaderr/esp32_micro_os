// ============================================================================
// FleetRegistryService.cpp — M3.4a, профильная интеграция пассивного реестра
// ============================================================================
#include "FleetRegistryService.h"
#include "FleetRegistryJson.h"
#include "BackupService.h"
#include "OtaMirrorService.h"
#include "HomeMasterApp.h"       // MICROOS_PROFILE_VER до core/Version.h
#include <core/EventBus.h>
#include <core/Events.h>
#include <core/Kernel.h>
#include <core/Version.h>
#include <services/ConfigService.h>
#include <services/MqttDescriptor.h>
#include <services/MqttTransport.h>
#include <services/NetworkManager.h>
#include <services/StorageService.h>
#include <services/TelemetryService.h>
#include <services/TimeService.h>

namespace {

uint32_t fleetNowUnix() {
    const uint32_t now = (uint32_t)TimeService::getInstance().getUnixTime();
    return now >= 1700000000UL ? now : 0;
}

void fleetCopy(char* dst, size_t cap, const char* src) {
    if (dst == nullptr || cap == 0) return;
    if (src == nullptr) src = "";
    const size_t n = strlen(src);
    const size_t copy = n < cap ? n : cap - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
}

void fleetIntegration(const fleet::Record& r, fleet::DeviceIntegration& out,
                      void* context) {
    (void)context;
    const bool currentDescriptor = r.identified && r.descriptorPresent;
    BackupFleetStatus backup;
    BackupService::getInstance().fleetStatus(
        r.id, r.self, currentDescriptor ? r.ip : nullptr, backup);
    out.backupSelected = backup.selected;
    fleetCopy(out.backupTarget, sizeof(out.backupTarget), backup.target);
    fleetCopy(out.backupResolvedIp, sizeof(out.backupResolvedIp),
              backup.resolvedIp);
    out.backupLastOk = backup.lastOk;
    fleetCopy(out.backupErr, sizeof(out.backupErr), backup.err);
    out.backupBlocked = backup.blocked;
    fleetCopy(out.backupStorage, sizeof(out.backupStorage), backup.storage);
    fleetCopy(out.backupResolution, sizeof(out.backupResolution),
              backup.resolution);

    OtaFleetStatus ota;
    OtaMirrorService::getInstance().fleetStatus(
        currentDescriptor ? r.host : "", currentDescriptor ? r.kernel : "",
        currentDescriptor ? r.profileVer : "", r.self, ota);
    out.otaConfigured = ota.configured;
    out.otaLinked = ota.linked;
    out.otaPairReady = ota.pairReady;
    fleetCopy(out.otaHost, sizeof(out.otaHost), ota.host);
    out.otaFwSize = ota.fwSize;
    out.otaFsSize = ota.fsSize;
    fleetCopy(out.otaMirrorKernel, sizeof(out.otaMirrorKernel),
              ota.mirrorKernel);
    fleetCopy(out.otaMirrorProfileVer, sizeof(out.otaMirrorProfileVer),
              ota.mirrorProfileVer);
    fleetCopy(out.otaVersionRelation, sizeof(out.otaVersionRelation),
              ota.versionRelation);
    out.otaLastOk = ota.lastOk;
    fleetCopy(out.otaErr, sizeof(out.otaErr), ota.err);
}

} // namespace

FleetRegistryService& FleetRegistryService::getInstance() {
    static FleetRegistryService inst;
    return inst;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void FleetRegistryService::init() {
    _core.reset();
    _core.setSelfId(NetworkService::getInstance().deviceId());
    _aliasPersist = true;
    loadAliases();
    _initialized = true;
    _selfDirty = true;
    log(LogLevel::Info, "init: passive RAM registry, aliases=%s", ALIASES_PATH);
}

void FleetRegistryService::start() {
    _hookRegistered = BrokerService::getInstance().addEventHook(
        &FleetRegistryService::onBrokerEvent);
    if (!_hookRegistered) {
        log(LogLevel::Error, "start: broker hook table full");
        publishError("FLEET_HOOK");
    }

    EventBus& bus = EventBus::getInstance();
    static const int32_t EVENTS[] = {
        NET_EVENT_CONNECTED, NET_EVENT_DISCONNECTED, NET_EVENT_IP_CHANGED,
        SH_EVENT_MQTT_CONNECTED, TEL_EVENT_SNAPSHOT,
    };
    bool subscribed = true;
    for (int32_t id : EVENTS) {
        if (!bus.subscribe(id, this)) subscribed = false;
    }
    if (!subscribed) {
        log(LogLevel::Error, "start: event subscription failed");
        publishError("FLEET_SUB");
    }

    _selfDirty = true;
    refreshSelf();
    _started = _hookRegistered && subscribed;
}

void FleetRegistryService::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    if (_hookRegistered) {
        BrokerService::getInstance().removeEventHook(
            &FleetRegistryService::onBrokerEvent);
        _hookRegistered = false;
    }
    _started = false;
}

void FleetRegistryService::tick() {
    const uint32_t now = millis();
    _core.tick(now);
    if ((uint32_t)(now - _lastSelfRefreshMs) >= 15000UL) _selfDirty = true;
    if (_selfDirty) {
        _selfDirty = false;
        refreshSelf();
    }
}

bool FleetRegistryService::canHandleEvent(int32_t eventId) const {
    return eventId == NET_EVENT_CONNECTED ||
           eventId == NET_EVENT_DISCONNECTED ||
           eventId == NET_EVENT_IP_CHANGED ||
           eventId == SH_EVENT_MQTT_CONNECTED ||
           eventId == TEL_EVENT_SNAPSHOT;
}

void FleetRegistryService::onEvent(int32_t eventId, const ShEventData*) {
    // EventBus работает в своей задаче. Core изменяется только из loopTask
    // (broker hook / tick / HTTP), поэтому здесь лишь отложенный флаг.
    if (canHandleEvent(eventId)) _selfDirty = true;
}

// ============================================================================
// SELF: локальный фактический источник, не зависящий от верхнего MQTT
// ============================================================================
void FleetRegistryService::refreshSelf() {
    NetworkService& net = NetworkService::getInstance();
    const char* id = net.deviceId();
    if (!_core.setSelfId(id)) return;

    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    const uint32_t nowMs = millis();
    _lastSelfRefreshMs = nowMs;
    const uint32_t nowUnix = fleetNowUnix();
    const char* clientId = net.hostname();
    char topic[MQTT_TOPIC_LEN];

    const int stateLen = snprintf(topic, sizeof(topic), "%s/%s/state", prefix, id);
    if (stateLen > 0 && (size_t)stateLen < sizeof(topic)) {
        _core.onPublish(topic, net.isConnected() ? "online" : "offline",
                        clientId, false, nowMs, nowUnix, prefix, true);
    }

    // При отсутствии валидного IP descriptor не выдумываем и не стираем
    // прежний: state уже честно показывает локальную доступность.
    char ip[16];
    net.ipString(ip, sizeof(ip));
    if (strcmp(ip, "0.0.0.0") != 0) {
        const mqtt_descriptor::Fields fields = {
            id, Kernel::getInstance().profileId(), MICROOS_VERSION,
            MICROOS_STR(MICROOS_PROFILE_VER), net.hostname(), ip
        };
        char body[384];
        if (mqtt_descriptor::buildTopic(topic, sizeof(topic), prefix, id) > 0 &&
            mqtt_descriptor::buildJson(body, sizeof(body), fields) > 0) {
            _core.onPublish(topic, body, clientId, false,
                            nowMs, nowUnix, prefix, true);
        }
    }

    const int telemetryLen = snprintf(topic, sizeof(topic),
                                      "%s/%s/telemetry", prefix, id);
    char telemetry[384];
    const size_t n = TelemetryService::getInstance().toJson(
        telemetry, sizeof(telemetry));
    if (telemetryLen > 0 && (size_t)telemetryLen < sizeof(topic) &&
        n > 0 && n < sizeof(telemetry)) {
        _core.onPublish(topic, telemetry, clientId, false,
                        nowMs, nowUnix, prefix, true);
    }
    applyStoredAliases();
}

// ============================================================================
// BROKER: синхронный hook, только RAM и чтение RAM-сервисов
// ============================================================================
void FleetRegistryService::onBrokerEvent(const BrokerEventInfo& info) {
    FleetRegistryService& self = getInstance();
    if (!self._hookRegistered) return;

    char prefix[CFG_VALUE_LEN];
    cfgGetStr("mqtt.prefix", prefix, sizeof(prefix), "microos");
    const uint32_t nowMs = millis();
    const uint32_t nowUnix = fleetNowUnix();

    if (info.type == BrokerEventInfo::Publish) {
        self._core.onPublish(info.topic, info.payload, info.clientId,
                             info.truncated, nowMs, nowUnix, prefix, false);
        self.applyStoredAliases();
        return;
    }
    if (info.type == BrokerEventInfo::RemoveClient) {
        const bool alive = BrokerService::getInstance().hasClient(info.clientId);
        self._core.onRemoveClient(info.clientId, alive, nowMs, nowUnix);
    }
}

// ============================================================================
// HTTP READ MODEL: реестр + существующие read-only статусы backup/OTA
// ============================================================================
size_t FleetRegistryService::apiStatus(char* buf, size_t bufSize) const {
    return fleet::writeRegistryJson(_core, millis(), fleetNowUnix(), buf, bufSize,
                                    _aliasPersist, &fleetIntegration, nullptr);
}

// ============================================================================
// АЛИАСЫ: единственная персистентная часть реестра
// ============================================================================
FleetRegistryService::AliasEntry*
FleetRegistryService::aliasEntry(const char* id, bool create) {
    AliasEntry* freeSlot = nullptr;
    for (AliasEntry& entry : _aliases) {
        if (entry.used && strcmp(entry.id, id) == 0) return &entry;
        if (!entry.used && freeSlot == nullptr) freeSlot = &entry;
    }
    if (!create || freeSlot == nullptr || !mqtt_descriptor::validId(id)) {
        return nullptr;
    }
    freeSlot->used = true;
    safeStrCopy(freeSlot->id, sizeof(freeSlot->id), id);
    freeSlot->alias[0] = '\0';
    return freeSlot;
}

const FleetRegistryService::AliasEntry*
FleetRegistryService::aliasEntry(const char* id) const {
    if (id == nullptr) return nullptr;
    for (const AliasEntry& entry : _aliases) {
        if (entry.used && strcmp(entry.id, id) == 0) return &entry;
    }
    return nullptr;
}

const char* FleetRegistryService::aliasFor(const char* id) const {
    const AliasEntry* entry = aliasEntry(id);
    return entry != nullptr ? entry->alias : "";
}

void FleetRegistryService::applyStoredAliases() {
    for (const AliasEntry& entry : _aliases) {
        if (!entry.used) continue;
        const fleet::Record* record = _core.find(entry.id);
        if (record != nullptr && strcmp(record->alias, entry.alias) != 0) {
            _core.setAlias(entry.id, entry.alias);
        }
    }
}

void FleetRegistryService::loadAliases() {
    for (AliasEntry& entry : _aliases) entry = AliasEntry{};

    char text[ALIASES_FILE_CAP + 1];
    const size_t n = StorageService::getInstance().readFile(
        ALIASES_PATH, (uint8_t*)text, ALIASES_FILE_CAP);
    if (n == 0) return;                 // первый запуск — пустой набор
    if (n == ALIASES_FILE_CAP) {
        _aliasPersist = false;
        log(LogLevel::Error, "aliases: file too large, ignored");
        return;
    }
    text[n] = '\0';

    char* save = nullptr;
    for (char* line = strtok_r(text, "\n", &save);
         line != nullptr; line = strtok_r(nullptr, "\n", &save)) {
        char* eq = strchr(line, '=');
        if (eq == nullptr) {
            _aliasPersist = false;
            log(LogLevel::Warning, "aliases: malformed line ignored");
            continue;
        }
        *eq = '\0';
        const char* alias = eq + 1;
        if (!mqtt_descriptor::validId(line) || !_core.validAlias(alias)) {
            _aliasPersist = false;
            log(LogLevel::Warning, "aliases: invalid entry ignored");
            continue;
        }
        AliasEntry* entry = aliasEntry(line, true);
        if (entry == nullptr) {
            _aliasPersist = false;
            log(LogLevel::Warning, "aliases: capacity reached");
            break;
        }
        safeStrCopy(entry->alias, sizeof(entry->alias), alias);
    }
    applyStoredAliases();
}

bool FleetRegistryService::saveAliases() {
    char text[ALIASES_FILE_CAP];
    size_t pos = 0;
    text[0] = '\0';
    for (const AliasEntry& entry : _aliases) {
        if (!entry.used) continue;
        const int n = snprintf(text + pos, sizeof(text) - pos, "%s=%s\n",
                               entry.id, entry.alias);
        if (n < 0 || (size_t)n >= sizeof(text) - pos) {
            _aliasPersist = false;
            log(LogLevel::Error, "aliases: save buffer too small");
            return false;
        }
        pos += (size_t)n;
    }
    const bool ok = StorageService::getInstance().atomicWrite(
        ALIASES_PATH, (const uint8_t*)text, pos);
    _aliasPersist = ok;
    return ok;
}

bool FleetRegistryService::setAlias(const char* id, const char* alias) {
    if (_core.find(id) == nullptr || !_core.validAlias(alias)) return false;
    AliasEntry* entry = aliasEntry(id, false);
    const bool fresh = entry == nullptr;
    if (fresh) entry = aliasEntry(id, true);
    if (entry == nullptr) return false;

    AliasEntry previous = *entry;
    if (fresh) previous = AliasEntry{};
    safeStrCopy(entry->alias, sizeof(entry->alias), alias);
    if (!saveAliases()) {
        *entry = previous;
        log(LogLevel::Error, "aliases: atomic save failed for %s", id);
        return false;
    }
    return _core.setAlias(id, alias);
}
