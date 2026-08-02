// ============================================================================
// UpdateService.h — OTA ОБНОВЛЕНИЕ С A/B-ОТКАТОМ (предложение A1)
// ============================================================================
// Фаза 1. Каркас защиты от «кирпича» после прошивки. ESP32 имеет штатную
// двухраздельную схему (ota_0/ota_1): новая прошивка пишется в неактивный
// раздел и стартует с флагом PENDING_VERIFY. Дальше два исхода:
//
//   · прошивка проработала VALIDATE_AFTER_MS без аварий -> помечаем VALID
//     (esp_ota_mark_app_valid_cancel_rollback) — она становится постоянной;
//   · прошивка упала/зависла до валидации -> загрузчик ESP32 САМ откатывается
//     на предыдущий раздел при следующей загрузке.
//
// В связке с bootloop-счётчиком Kernel'а это даёт полную цепочку A1:
//   bootloop >= 3 -> Safe Mode; свежая нестабильная прошивка -> авто-откат.
//
// Загрузка прошивки по сети (HTTP/HA, как в монолите v2.5.0) — Phase 2,
// когда появится NetworkManager. Здесь: механика разделов + совместимость.
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include <esp_ota_ops.h>

// Сколько свежая прошивка должна проработать, чтобы считаться валидной.
// Совпадает с KERNEL_STABLE_MS (60 с стабильной работы).
constexpr uint32_t OTA_VALIDATE_AFTER_MS = 60000;

class UpdateService : public ModuleBase {
public:
    static UpdateService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "UpdateService"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0008; }

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 5000; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- СОСТОЯНИЕ ПРОШИВКИ ---------------------------------------------------
    /// true — текущая прошивка ещё не валидирована (свежая, после OTA).
    bool isPendingValidation() const { return _pendingValidation; }

    /// Версия прошивки (из app description: PROJECT_VER).
    const char* firmwareVersion() const { return _fwVersion; }

    // --- ОПЕРАЦИИ ------------------------------------------------------------
    /// Принудительный откат на предыдущий раздел + ребут
    /// (админская команда / ПАЗ при критической деградации свежей прошивки).
    void requestRollback();

    /// Phase 2: загрузка по HTTP(S) с проверкой совместимости FW/FS
    /// (маркер min_fs_version из A1). Сейчас — заглушка, возвращает false.
    bool startHttpUpdate(const char* url);

private:
    UpdateService() = default;

    bool _pendingValidation = false;   // раздел в состоянии PENDING_VERIFY
    uint32_t _bootMs = 0;              // отсчёт стабильной работы
    char _fwVersion[32] = "unknown";   // версия из esp_app_desc_t
};
