// ============================================================================
// FleetRegistryJson.cpp — полный, отказобезопасный JSON пассивного реестра
// ============================================================================
#include "FleetRegistryJson.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace fleet {
namespace {

class Writer {
public:
    Writer(char* out, size_t cap) : _out(out), _cap(cap) {
        if (_out != nullptr && _cap > 0) _out[0] = '\0';
    }

    void raw(const char* text) {
        if (!_ok) return;
        if (text == nullptr) { _ok = false; return; }
        const size_t n = strlen(text);
        if (_out == nullptr || _len > _cap || n >= _cap - _len) {
            _ok = false;
            return;
        }
        memcpy(_out + _len, text, n);
        _len += n;
        _out[_len] = '\0';
    }

    void string(const char* text) {
        static const char HEX[] = "0123456789abcdef";
        if (text == nullptr) { _ok = false; return; }
        raw("\"");
        for (const unsigned char* p = (const unsigned char*)text; *p != 0; ++p) {
            const unsigned char c = *p;
            if (c == '"' || c == '\\') {
                char escaped[3] = {'\\', (char)c, '\0'};
                raw(escaped);
            } else if (c < 0x20) {
                char escaped[7] = {'\\', 'u', '0', '0',
                                   HEX[(c >> 4) & 0x0F], HEX[c & 0x0F], '\0'};
                raw(escaped);
            } else {
                char one[2] = {(char)c, '\0'};
                raw(one);
            }
        }
        raw("\"");
    }

    void nullableString(const char* text) {
        if (text == nullptr || text[0] == '\0') raw("null");
        else string(text);
    }

    void fmt(const char* format, ...) {
        if (!_ok) return;
        if (_out == nullptr || _cap == 0 || _len > _cap) {
            _ok = false;
            return;
        }
        va_list ap;
        va_start(ap, format);
        const int n = vsnprintf(_out + _len, _cap - _len, format, ap);
        va_end(ap);
        if (n < 0 || (size_t)n >= _cap - _len) { _ok = false; return; }
        _len += (size_t)n;
    }

    void nullableU32(uint32_t value) {
        if (value == 0) raw("null");
        else fmt("%lu", (unsigned long)value);
    }

    void age(uint32_t stamp, uint32_t nowMs) {
        if (stamp == 0) raw("null");
        else fmt("%lu", (unsigned long)((uint32_t)(nowMs - stamp) / 1000UL));
    }

    size_t finish() {
        if (!_ok) {
            if (_out != nullptr && _cap > 0) _out[0] = '\0';
            return 0;
        }
        return _len;
    }

private:
    char* _out;
    size_t _cap;
    size_t _len = 0;
    bool _ok = true;
};

void integrationJson(Writer& w, const Record& r,
                     const DeviceIntegration& x) {
    w.raw(",\"web_url\":");
    if (r.identified && r.descriptorPresent && validIpv4(r.ip)) {
        char url[24];
        const int n = snprintf(url, sizeof(url), "http://%s/", r.ip);
        if (n <= 0 || (size_t)n >= sizeof(url)) w.raw("null");
        else w.string(url);
    } else {
        w.raw("null");
    }

    w.fmt(",\"backup\":{\"selected\":%u,\"target\":",
          x.backupSelected ? 1u : 0u);
    w.nullableString(x.backupTarget);
    w.raw(",\"resolved_ip\":"); w.nullableString(x.backupResolvedIp);
    w.raw(",\"last_ok\":"); w.nullableU32(x.backupLastOk);
    w.raw(",\"err\":"); w.string(x.backupErr);
    w.fmt(",\"blocked\":%u,\"storage\":", x.backupBlocked ? 1u : 0u);
    w.nullableString(x.backupStorage);
    w.raw(",\"resolution\":"); w.nullableString(x.backupResolution);
    w.raw("}");

    w.fmt(",\"ota\":{\"configured\":%u,\"linked\":%u,\"pair_ready\":%u,"
          "\"host\":", x.otaConfigured ? 1u : 0u, x.otaLinked ? 1u : 0u,
          x.otaPairReady ? 1u : 0u);
    w.nullableString(x.otaHost);
    w.raw(",\"fw_size\":"); w.nullableU32(x.otaFwSize);
    w.raw(",\"fs_size\":"); w.nullableU32(x.otaFsSize);
    w.raw(",\"mirror_kernel\":"); w.nullableString(x.otaMirrorKernel);
    w.raw(",\"mirror_profile_ver\":"); w.nullableString(x.otaMirrorProfileVer);
    w.raw(",\"version_relation\":");
    w.string(x.otaVersionRelation[0] != '\0' ? x.otaVersionRelation : "unknown");
    w.raw(",\"last_ok\":"); w.nullableU32(x.otaLastOk);
    w.raw(",\"err\":"); w.string(x.otaErr);
    w.raw(",\"board_compatible\":null}");
}

void telemetryJson(Writer& w, const Telemetry& t) {
    w.fmt("{\"uptime\":%lu,\"heap\":%lu,\"heapMin\":%lu,"
          "\"busDropped\":%lu,\"busHwm\":%u,\"cpuTenths\":%ld,"
          "\"degradation\":%u,\"gwRtt\":%lu,\"bootloop\":",
          (unsigned long)t.uptime, (unsigned long)t.heap,
          (unsigned long)t.heapMin, (unsigned long)t.busDropped,
          (unsigned)t.busHwm, (long)t.cpuTenths, (unsigned)t.degradation,
          (unsigned long)t.gwRtt);
    if (t.bootloopPresent) w.fmt("%u", (unsigned)t.bootloop);
    else w.raw("null");
    w.raw(",\"auditLost\":");
    if (t.auditLostPresent) w.fmt("%lu", (unsigned long)t.auditLost);
    else w.raw("null");
}

void recordJson(Writer& w, const Record& r, uint32_t nowMs, bool first,
                IntegrationProvider provider, void* context) {
    w.fmt("%s{\"id\":", first ? "" : ",");
    w.string(r.id);
    w.raw(",\"alias\":");      w.string(r.alias);
    w.fmt(",\"self\":%u,\"identified\":%u,\"descriptorPresent\":%u,"
          "\"availability\":", r.self ? 1u : 0u, r.identified ? 1u : 0u,
          r.descriptorPresent ? 1u : 0u);
    w.string(Core::availabilityName(r.availability));
    w.raw(",\"profile\":");    w.string(r.profile);
    w.raw(",\"kernel\":");     w.string(r.kernel);
    w.raw(",\"profileVer\":"); w.string(r.profileVer);
    w.raw(",\"host\":");       w.string(r.host);
    w.raw(",\"ip\":");         w.string(r.ip);
    w.raw(",\"clientId\":");   w.string(r.clientId);
    w.fmt(",\"descriptorMs\":%lu,\"descriptorUnix\":%lu,"
          "\"descriptorAgeSec\":", (unsigned long)r.descriptorMs,
          (unsigned long)r.descriptorUnix);
    w.age(r.descriptorMs, nowMs);
    w.fmt(",\"availabilityMs\":%lu,\"availabilityUnix\":%lu,"
          "\"availabilityAgeSec\":", (unsigned long)r.availabilityMs,
          (unsigned long)r.availabilityUnix);
    w.age(r.availabilityMs, nowMs);
    w.fmt(",\"telemetryMs\":%lu,\"telemetryUnix\":%lu,"
          "\"telemetryAgeSec\":", (unsigned long)r.telemetryMs,
          (unsigned long)r.telemetryUnix);
    w.age(r.telemetryMs, nowMs);
    w.fmt(",\"lastOnlineMs\":%lu,\"identityConflicts\":%u,\"telemetry\":",
          (unsigned long)r.lastOnlineMs, (unsigned)r.identityConflicts);
    if (!r.telemetry.present) w.raw("null");
    else {
        telemetryJson(w, r.telemetry);
        w.raw("}");
    }

    DeviceIntegration integration;
    if (provider != nullptr) provider(r, integration, context);
    integrationJson(w, r, integration);
    w.raw("}");
}

} // namespace

size_t writeRegistryJson(const Core& core, uint32_t nowMs, uint32_t nowUnix,
                         char* out, size_t cap, bool aliasPersist,
                         IntegrationProvider provider, void* context) {
    Writer w(out, cap);
    uint8_t identified = 0, online = 0, offline = 0, stale = 0, unknown = 0;
    for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
        const Record& r = core.records()[i];
        if (!r.used) continue;
        if (r.identified) ++identified;
        switch (r.availability) {
            case Availability::Online: ++online; break;
            case Availability::Offline: ++offline; break;
            case Availability::Stale: ++stale; break;
            default: ++unknown; break;
        }
    }

    w.fmt("{\"v\":1,\"nowMs\":%lu,\"generatedUnix\":%lu,"
          "\"staleAfterSec\":%lu,\"count\":%u,\"aliasPersist\":%u,"
          "\"summary\":{\"records\":%u,\"identified\":%u,\"online\":%u,"
          "\"offline\":%u,\"stale\":%u,\"unknown\":%u},\"devices\":[",
          (unsigned long)nowMs, (unsigned long)nowUnix,
          (unsigned long)(STALE_AFTER_MS / 1000UL), (unsigned)core.count(),
          aliasPersist ? 1u : 0u, (unsigned)core.count(), (unsigned)identified,
          (unsigned)online, (unsigned)offline, (unsigned)stale, (unsigned)unknown);
    bool first = true;
    for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
        const Record& r = core.records()[i];
        if (!r.used) continue;
        recordJson(w, r, nowMs, first, provider, context);
        first = false;
    }
    const Counters& c = core.counters();
    w.fmt("],\"counters\":{\"accepted\":%lu,\"rejected\":%lu,"
          "\"truncated\":%lu,\"identityConflicts\":%lu,"
          "\"recordsRejected\":%lu,\"removesIgnored\":%lu}}",
          (unsigned long)c.accepted, (unsigned long)c.rejected,
          (unsigned long)c.truncated, (unsigned long)c.identityConflicts,
          (unsigned long)c.recordsRejected, (unsigned long)c.removesIgnored);
    return w.finish();
}

} // namespace fleet
