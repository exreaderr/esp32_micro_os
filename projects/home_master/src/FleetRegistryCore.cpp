// ============================================================================
// FleetRegistryCore.cpp — пассивная идентичность/availability/telemetry M3.4a
// ============================================================================
#include "FleetRegistryCore.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fleet {
namespace {

void copyLowerId(char out[ID_LEN], const char in[ID_LEN]) {
    for (uint8_t i = 0; i < 12; ++i) {
        const char c = in[i];
        out[i] = (c >= 'A' && c <= 'F') ? (char)(c + ('a' - 'A')) : c;
    }
    out[12] = '\0';
}

bool copyIfFits(char* out, size_t cap, const char* src) {
    const size_t n = strlen(src);
    if (n == 0 || n >= cap) return false;
    memcpy(out, src, n + 1);
    return true;
}

const char* valueForKey(const char* js, const char* key) {
    if (js == nullptr || key == nullptr) return nullptr;
    char needle[32];
    const int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return nullptr;
    const char* p = js;
    while ((p = strstr(p, needle)) != nullptr) {
        // Ключом считаем только позицию после '{' или ',' с пробелами.
        const char* before = p;
        while (before > js && (unsigned char)before[-1] <= ' ') --before;
        if (before > js && (before[-1] == '{' || before[-1] == ',')) {
            const char* q = p + n;
            while (*q != '\0' && (unsigned char)*q <= ' ') ++q;
            if (*q++ != ':') return nullptr;
            while (*q != '\0' && (unsigned char)*q <= ' ') ++q;
            return q;
        }
        p += n;
    }
    return nullptr;
}

bool jsonString(const char* js, const char* key, char* out, size_t cap) {
    const char* p = valueForKey(js, key);
    if (p == nullptr || *p++ != '"') return false;
    size_t n = 0;
    while (*p != '\0' && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            const char esc = *p++;
            switch (esc) {
                case '"': case '\\': case '/': c = (unsigned char)esc; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: return false;  // descriptor v1 не использует \u в полях
            }
        }
        if (c < 0x20 || n + 1 >= cap) return false;
        out[n++] = (char)c;
    }
    if (*p != '"' || n == 0) return false;
    out[n] = '\0';
    return true;
}

bool jsonU32(const char* js, const char* key, uint32_t& out) {
    const char* p = valueForKey(js, key);
    if (p == nullptr || *p < '0' || *p > '9') return false;
    char* end = nullptr;
    const unsigned long value = strtoul(p, &end, 10);
    if (end == p || (*end != ',' && *end != '}' &&
                     (unsigned char)*end > ' ')) return false;
    out = (uint32_t)value;
    return true;
}

bool jsonCpuTenths(const char* js, const char* key, int32_t& out) {
    const char* p = valueForKey(js, key);
    if (p == nullptr) return false;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    if (*p < '0' || *p > '9') return false;
    uint32_t whole = 0;
    while (*p >= '0' && *p <= '9') {
        if (whole > 1000000UL) return false;
        whole = whole * 10U + (uint32_t)(*p++ - '0');
    }
    uint32_t tenth = 0;
    if (*p == '.') {
        ++p;
        if (*p >= '0' && *p <= '9') tenth = (uint32_t)(*p++ - '0');
        while (*p >= '0' && *p <= '9') ++p;  // лишние знаки округляем вниз
    }
    if (*p != ',' && *p != '}' && (unsigned char)*p > ' ') return false;
    const int32_t value = (int32_t)(whole * 10U + tenth);
    out = neg ? -value : value;
    return true;
}

} // namespace

bool validId(const char* id) {
    if (id == nullptr) return false;
    for (uint8_t i = 0; i < 12; ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return id[12] == '\0';
}

bool validIpv4(const char* ip) {
    if (ip == nullptr || *ip == '\0') return false;
    uint8_t parts = 0;
    const char* p = ip;
    while (*p != '\0') {
        if (parts >= 4 || *p < '0' || *p > '9') return false;
        uint16_t value = 0;
        uint8_t digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = (uint16_t)(value * 10U + (uint16_t)(*p++ - '0'));
            if (++digits > 3 || value > 255U) return false;
        }
        ++parts;
        if (parts < 4) {
            if (*p++ != '.') return false;
        } else if (*p != '\0') {
            return false;
        }
    }
    return parts == 4;
}

void Core::reset() {
    for (auto& r : _records) r = Record{};
    _counters = Counters{};
    _selfId[0] = '\0';
}

bool Core::setSelfId(const char* id) {
    if (!validId(id)) return false;
    copyLowerId(_selfId, id);
    for (auto& r : _records) r.self = r.used && strcmp(r.id, _selfId) == 0;
    return true;
}

Core::Kind Core::parseTopic(const char* topic, const char* prefix,
                            char id[ID_LEN]) const {
    if (topic == nullptr || prefix == nullptr || prefix[0] == '\0') return Kind::Invalid;
    const size_t n = strlen(prefix);
    if (strncmp(topic, prefix, n) != 0 || topic[n] != '/') return Kind::Invalid;
    const char* rest = topic + n + 1;
    const char* slash = strchr(rest, '/');
    if (slash == nullptr || (size_t)(slash - rest) != 12) return Kind::Invalid;
    char raw[ID_LEN];
    memcpy(raw, rest, 12);
    raw[12] = '\0';
    if (!validId(raw)) return Kind::Invalid;
    copyLowerId(id, raw);
    const char* suffix = slash + 1;
    if (strcmp(suffix, "descriptor") == 0) return Kind::Descriptor;
    if (strcmp(suffix, "state") == 0) return Kind::State;
    if (strcmp(suffix, "telemetry") == 0) return Kind::Telemetry;
    return Kind::Invalid;
}

Record* Core::ensure(const char* id, bool self) {
    Record* freeSlot = nullptr;
    for (auto& r : _records) {
        if (r.used && strcmp(r.id, id) == 0) return &r;
        if (!r.used && freeSlot == nullptr) freeSlot = &r;
    }
    if (freeSlot == nullptr) {
        ++_counters.recordsRejected;
        return nullptr;
    }
    freeSlot->used = true;
    freeSlot->self = self;
    copyLowerId(freeSlot->id, id);
    return freeSlot;
}

Record* Core::find(const char* id) {
    if (!validId(id)) return nullptr;
    char norm[ID_LEN];
    copyLowerId(norm, id);
    for (auto& r : _records) if (r.used && strcmp(r.id, norm) == 0) return &r;
    return nullptr;
}

const Record* Core::find(const char* id) const {
    if (!validId(id)) return nullptr;
    char norm[ID_LEN];
    copyLowerId(norm, id);
    for (const auto& r : _records) if (r.used && strcmp(r.id, norm) == 0) return &r;
    return nullptr;
}

const Record* Core::self() const {
    return _selfId[0] != '\0' ? find(_selfId) : nullptr;
}

uint8_t Core::count() const {
    uint8_t n = 0;
    for (const auto& r : _records) if (r.used) ++n;
    return n;
}

void Core::stamp(uint32_t& ms, uint32_t& unix,
                 uint32_t nowMs, uint32_t nowUnix) {
    ms = nowMs;
    if (nowUnix >= 1700000000UL) unix = nowUnix;
}

void Core::clearDescriptor(Record& r) {
    r.identified = false;
    r.descriptorPresent = false;
    r.profile[0] = '\0';
    r.kernel[0] = '\0';
    r.profileVer[0] = '\0';
    r.host[0] = '\0';
    r.ip[0] = '\0';
}

bool Core::parseDescriptor(Record& r, const char* payload) {
    uint32_t version = 0;
    char id[ID_LEN], profile[PROFILE_LEN], kernel[VERSION_LEN];
    char profileVer[VERSION_LEN], host[HOST_LEN], ip[IP_LEN];
    if (!jsonU32(payload, "v", version) || version != 1 ||
        !jsonString(payload, "id", id, sizeof(id)) || !validId(id) ||
        !jsonString(payload, "profile", profile, sizeof(profile)) ||
        !jsonString(payload, "kernel", kernel, sizeof(kernel)) ||
        !jsonString(payload, "profile_ver", profileVer, sizeof(profileVer)) ||
        !jsonString(payload, "host", host, sizeof(host)) ||
        !jsonString(payload, "ip", ip, sizeof(ip)) || !validIpv4(ip)) {
        return false;
    }
    char norm[ID_LEN];
    copyLowerId(norm, id);
    if (strcmp(norm, r.id) != 0) return false;
    // IP/hostname/версии — изменяемые свойства. Другой profile под тем же
    // MAC-id несовместим: фиксируем коллизию, но применяем последний валидный
    // descriptor и не удаляем запись/алиас.
    if (r.identified && r.descriptorPresent && r.profile[0] != '\0' &&
        strcmp(r.profile, profile) != 0) {
        if (r.identityConflicts < 65535) ++r.identityConflicts;
        ++_counters.identityConflicts;
    }
    copyIfFits(r.profile, sizeof(r.profile), profile);
    copyIfFits(r.kernel, sizeof(r.kernel), kernel);
    copyIfFits(r.profileVer, sizeof(r.profileVer), profileVer);
    copyIfFits(r.host, sizeof(r.host), host);
    copyIfFits(r.ip, sizeof(r.ip), ip);
    r.identified = true;
    r.descriptorPresent = true;
    return true;
}

bool Core::parseTelemetry(Record& r, const char* payload) {
    Telemetry t = r.telemetry;
    uint32_t busHwm = 0, degradation = 0, bootloop = 0;
    if (!jsonU32(payload, "uptime", t.uptime) ||
        !jsonU32(payload, "heap", t.heap) ||
        !jsonU32(payload, "heap_min", t.heapMin) ||
        !jsonU32(payload, "bus_dropped", t.busDropped) ||
        !jsonU32(payload, "bus_hwm", busHwm) || busHwm > 65535UL ||
        !jsonCpuTenths(payload, "cpu_t", t.cpuTenths) ||
        !jsonU32(payload, "degradation", degradation) || degradation > 255UL ||
        !jsonU32(payload, "gw_rtt", t.gwRtt)) {
        return false;
    }
    t.busHwm = (uint16_t)busHwm;
    t.degradation = (uint8_t)degradation;
    // Поля появились раньше M3.4a, но старый внешний источник без них не
    // отбрасываем: отсутствие сохраняет прежнее значение, не выдумывает ноль.
    if (valueForKey(payload, "bootloop") != nullptr) {
        if (!jsonU32(payload, "bootloop", bootloop) || bootloop > 65535UL) return false;
        t.bootloop = (uint16_t)bootloop;
        t.bootloopPresent = true;
    }
    if (valueForKey(payload, "audit_lost") != nullptr) {
        if (!jsonU32(payload, "audit_lost", t.auditLost)) return false;
        t.auditLostPresent = true;
    }
    t.present = true;
    r.telemetry = t;
    return true;
}

bool Core::onPublish(const char* topic, const char* payload,
                     const char* clientId, bool truncated,
                     uint32_t nowMs, uint32_t nowUnix,
                     const char* prefix, bool self) {
    char id[ID_LEN];
    const Kind kind = parseTopic(topic, prefix, id);
    if (kind == Kind::Invalid || payload == nullptr) {
        ++_counters.rejected;
        return false;
    }
    if (truncated) {
        ++_counters.truncated;
        return false;
    }
    const bool selfId = _selfId[0] != '\0' && strcmp(id, _selfId) == 0;
    if (self != selfId) {  // self — только локально; remote не подменяет мастер
        ++_counters.rejected;
        return false;
    }
    if (kind == Kind::Descriptor && payload[0] == '\0') {
        Record* r = find(id);
        if (r != nullptr) {
            clearDescriptor(*r);
            ++_counters.accepted;
            return true;
        }
        ++_counters.rejected;
        return false;
    }
    Record* r = ensure(id, self);
    if (r == nullptr) return false;
    if (clientId != nullptr && clientId[0] != '\0') {
        if (strlen(clientId) >= sizeof(r->clientId)) {
            ++_counters.rejected;
            return false;
        }
        strcpy(r->clientId, clientId);
    }

    if (kind == Kind::Descriptor) {
        if (!parseDescriptor(*r, payload)) {
            ++_counters.rejected;
            return false;
        }
        stamp(r->descriptorMs, r->descriptorUnix, nowMs, nowUnix);
    } else if (kind == Kind::State) {
        if (strcmp(payload, "online") == 0) {
            r->availability = Availability::Online;
            r->lastOnlineMs = nowMs;
        } else if (strcmp(payload, "offline") == 0) {
            r->availability = Availability::Offline;
        } else {
            ++_counters.rejected;
            return false;
        }
        stamp(r->availabilityMs, r->availabilityUnix, nowMs, nowUnix);
    } else {
        if (!parseTelemetry(*r, payload)) {
            ++_counters.rejected;
            return false;
        }
        stamp(r->telemetryMs, r->telemetryUnix, nowMs, nowUnix);
        if (r->availability == Availability::Stale) {
            r->availability = Availability::Online;
            r->lastOnlineMs = nowMs;
        }
    }
    ++_counters.accepted;
    return true;
}

void Core::onRemoveClient(const char* clientId, bool hasClient,
                          uint32_t nowMs, uint32_t nowUnix) {
    if (clientId == nullptr || clientId[0] == '\0') return;
    if (hasClient) {
        ++_counters.removesIgnored;
        return;
    }
    for (auto& r : _records) {
        if (!r.used || r.self || strcmp(r.clientId, clientId) != 0) continue;
        r.availability = Availability::Offline;
        stamp(r.availabilityMs, r.availabilityUnix, nowMs, nowUnix);
    }
}

void Core::tick(uint32_t nowMs) {
    for (auto& r : _records) {
        if (!r.used || r.self || r.availability != Availability::Online) continue;
        if (!fresh(r.availabilityMs, nowMs) && !fresh(r.telemetryMs, nowMs)) {
            r.availability = Availability::Stale;
        }
    }
}

bool Core::validAlias(const char* alias) {
    if (alias == nullptr) return false;
    const size_t n = strlen(alias);
    if (n >= ALIAS_LEN) return false;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)alias[i] < 0x20) return false;
    return true;
}

bool Core::setAlias(const char* id, const char* alias) {
    Record* r = find(id);
    if (r == nullptr || !validAlias(alias)) return false;
    strcpy(r->alias, alias);
    return true;
}

const char* Core::resolveIp(const char* id) const {
    const Record* r = find(id);
    if (r == nullptr || !r->identified || !r->descriptorPresent ||
        !validIpv4(r->ip) || strcmp(r->ip, "0.0.0.0") == 0) return nullptr;
    return r->ip;
}

const char* Core::availabilityName(Availability value) {
    switch (value) {
        case Availability::Online: return "online";
        case Availability::Offline: return "offline";
        case Availability::Stale: return "stale";
        default: return "unknown";
    }
}

} // namespace fleet
