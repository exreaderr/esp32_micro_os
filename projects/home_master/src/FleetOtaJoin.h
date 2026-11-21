// ============================================================================
// FleetOtaJoin.h — чистая read-only связь descriptor и OTA HostState
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fleet_ota {

inline bool present(const char* value) {
    return value != nullptr && value[0] != '\0';
}

inline bool pairReady(bool linked, const char* mirrorKernel,
                      const char* mirrorProfileVer, uint32_t fwSize,
                      uint32_t fsSize, const char* lastErr) {
    return linked && present(mirrorKernel) && present(mirrorProfileVer) &&
           fwSize > 0 && fsSize > 0 && !present(lastErr);
}

inline const char* versionRelation(const char* deviceKernel,
                                   const char* deviceProfileVer,
                                   const char* mirrorKernel,
                                   const char* mirrorProfileVer) {
    if (!present(deviceKernel) || !present(deviceProfileVer) ||
        !present(mirrorKernel) || !present(mirrorProfileVer)) {
        return "unknown";
    }
    return strcmp(deviceKernel, mirrorKernel) == 0 &&
           strcmp(deviceProfileVer, mirrorProfileVer) == 0
               ? "same" : "different";
}

} // namespace fleet_ota
