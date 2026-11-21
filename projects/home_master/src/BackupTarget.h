// ============================================================================
// BackupTarget.h — M3.4a: чистый контракт ручных backup-целей
// ----------------------------------------------------------------------------
// legacy: "10.146.75.53" -> сеть и каталог остаются теми же.
// fleet:  "id:3e0f02ded999" -> IP даёт FleetRegistry, каталог id-<deviceId>.
// Здесь нет Arduino/FS/сети: разбор и storage key проверяются host-тестом.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace backup_target {

inline bool validDeviceId(const char* id) {
    if (id == nullptr) return false;
    for (uint8_t i = 0; i < 12; ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return id[12] == '\0';
}

inline bool isFleetId(const char* target) {
    return target != nullptr && target[0] == 'i' && target[1] == 'd' &&
           target[2] == ':' && validDeviceId(target + 3);
}

inline const char* deviceId(const char* target) {
    return isFleetId(target) ? target + 3 : nullptr;
}

inline bool storageKey(const char* target, char* out, size_t cap) {
    if (target == nullptr || out == nullptr || cap == 0 || target[0] == '\0') {
        return false;
    }
    if (!isFleetId(target)) {
        const size_t n = strlen(target);
        if (n >= cap) return false;
        memcpy(out, target, n + 1);
        return true;
    }
    const char* id = target + 3;
    if (cap < 16) return false;       // "id-" + 12 HEX + '\0'
    memcpy(out, "id-", 3);
    for (uint8_t i = 0; i < 12; ++i) {
        const char c = id[i];
        out[3 + i] = (c >= 'A' && c <= 'F') ? (char)(c + ('a' - 'A')) : c;
    }
    out[15] = '\0';
    return true;
}

} // namespace backup_target
