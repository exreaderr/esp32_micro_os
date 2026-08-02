// ============================================================================
// UpdateService.cpp — реализация OTA с A/B-откатом
// ============================================================================
#include "UpdateService.h"
#include "../core/Events.h"
#include <esp_ota_ops.h>
#include <esp_app_desc.h>

UpdateService& UpdateService::getInstance() {
    static UpdateService instance;
    return instance;
}

// ============================================================================
// INIT: определить состояние текущего раздела
// ============================================================================
void UpdateService::init() {
    _bootMs = millis();

    // --- Версия прошивки из app descriptor (зашита при сборке) ------------
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc != nullptr) {
        safeStrCopy(_fwVersion, sizeof(_fwVersion), desc->version);
    }

    // --- Состояние раздела: PENDING_VERIFY = свежая прошивка после OTA ----
    esp_ota_img_states_t state;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &state) == ESP_OK) {
        _pendingValidation = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }

    if (_pendingValidation) {
        log(LogLevel::Warning,
            "firmware %s is PENDING_VERIFY: will validate after %lu s stable",
            _fwVersion, (unsigned long)(OTA_VALIDATE_AFTER_MS / 1000));
        // Если эта прошивка упадёт/зависнет ДО валидации — загрузчик ESP32
        // при следующей загрузке сам вернёт предыдущий раздел. Ничего
        // делать не нужно: молчание = отказ.
    } else {
        log(LogLevel::Info, "firmware %s, partition 0x%lx, validated",
            _fwVersion, (unsigned long)(running ? running->address : 0));
    }
    _initialized = true;
}

void UpdateService::start() {
    _started = true;
}

void UpdateService::stop() {
    _started = false;
}

// ============================================================================
// TICK: авто-валидация свежей прошивки после 60 с стабильной работы
// ============================================================================
void UpdateService::tick() {
    if (!_pendingValidation) return;
    if (millis() - _bootMs < OTA_VALIDATE_AFTER_MS) return;

    esp_err_t rc = esp_ota_mark_app_valid_cancel_rollback();
    _pendingValidation = false;

    ShEventData d; d.clear();
    if (rc == ESP_OK) {
        log(LogLevel::Info, "firmware %s VALIDATED (rollback cancelled)",
            _fwVersion);
        postEvent(OTA_EVENT_SUCCESS, &d);
    } else {
        // Не удалось снять флаг — следующая загрузка сделает откат.
        // Фиксируем, но не паникуем: устройство работает.
        log(LogLevel::Error, "validate failed rc=%d, rollback will occur",
            (int)rc);
        safeStrCopy(d.payload, sizeof(d.payload), "VALIDATE_FAILED");
        postEvent(OTA_EVENT_FAILED, &d);
    }
}

// ============================================================================
// ОПЕРАЦИИ
// ============================================================================
void UpdateService::requestRollback() {
    log(LogLevel::Critical, "rollback requested -> previous partition, REBOOT");
    ShEventData d; d.clear();
    postEvent(OTA_EVENT_ROLLBACK, &d);
    delay(200);   // дать событию уйти в очередь шины
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Сюда не возвращаемся.
}

bool UpdateService::startHttpUpdate(const char* url) {
    (void)url;
    // Phase 2 (после NetworkManager): HTTPS-загрузка в неактивный раздел
    // (esp_https_ota), проверка маркера совместимости FW/FS (min_fs_version
    // из A1), подпись — точка подключения C2 (Secure Boot) для коммерческой
    // версии. Монолит: http://<mqtt_ip>:8123/local/ota/<hostname>/version.json
    log(LogLevel::Warning, "startHttpUpdate: Phase 2 (network required)");
    return false;
}
