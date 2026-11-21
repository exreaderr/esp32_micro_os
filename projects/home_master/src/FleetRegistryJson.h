// ============================================================================
// FleetRegistryJson.h — чистая сериализация RAM-реестра M3.4a
// Полный валидный JSON или 0 + пустой выход. Без Arduino и динамического DOM.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include "FleetRegistryCore.h"

namespace fleet {

// Read-only проекция существующих backup/OTA сервисов для одной записи.
// Provider заполняет структуру синхронно; writer никаких действий не вызывает.
struct DeviceIntegration {
    bool backupSelected = false;
    char backupTarget[20] = "";
    char backupResolvedIp[IP_LEN] = "";
    uint32_t backupLastOk = 0;
    char backupErr[32] = "";
    bool backupBlocked = false;
    char backupStorage[24] = "";
    char backupResolution[12] = "";

    bool otaConfigured = false;
    bool otaLinked = false;
    bool otaPairReady = false;
    char otaHost[HOST_LEN] = "";
    uint32_t otaFwSize = 0;
    uint32_t otaFsSize = 0;
    char otaMirrorKernel[VERSION_LEN] = "";
    char otaMirrorProfileVer[VERSION_LEN] = "";
    char otaVersionRelation[10] = "unknown";
    uint32_t otaLastOk = 0;
    char otaErr[24] = "";
};

using IntegrationProvider = void (*)(const Record& record,
                                     DeviceIntegration& integration,
                                     void* context);

size_t writeRegistryJson(const Core& core, uint32_t nowMs, uint32_t nowUnix,
                         char* out, size_t cap, bool aliasPersist = true,
                         IntegrationProvider provider = nullptr,
                         void* context = nullptr);

} // namespace fleet
