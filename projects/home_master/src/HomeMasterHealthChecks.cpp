// ============================================================================
// HomeMasterHealthChecks.cpp — проверки ПАЗ мастера (M0: hm.sd)
// ============================================================================
#include "HomeMasterHealthChecks.h"
#include "SdService.h"
#include "BackupService.h"
#include <services/ConfigService.h>
#include <services/TimeService.h>
#include <services/HealthMonitor.h>
#include <services/IHealthCheck.h>

// hm.sd — хранилище мастера. Отсутствие карты = деградация (Warning),
// сбой монтирования при вставленной карте = Critical (карта/слот дохлый),
// осознанно выключенная в конфиге = штатно (Ok).
class SdHealthCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "hm.sd"; }
    uint32_t intervalMs() const override { return 30000; }   // редкая

    HealthResult run() override {
        switch (SdService::getInstance().state()) {
            case SdState::Mounted:
            case SdState::Disabled:
                return HealthResult::ok();
            case SdState::NoCard:
                return HealthResult::warning("SD-карта отсутствует");
            case SdState::Failed:
                return HealthResult::critical("SD не монтируется");
        }
        return HealthResult::ok();
    }
};

// hm.bk — бэкапы парка (M3.3). Выключенный модуль — штатно (Ok).
// Беда хоста (blocked/offline/old_fw) — Warning: мастер жив, но парк без
// свежих снимков; Critical не ставим — функция не защитная, а сервисная.
class BackupHealthCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "hm.bk"; }
    uint32_t intervalMs() const override { return 60000; }

    HealthResult run() override {
        BackupService& bk = BackupService::getInstance();
        if (!bk.isEnabled()) return HealthResult::ok();
        char first[40] = "";
        uint8_t bad = bk.troubleCount(first, sizeof(first));
        if (bad > 0) return HealthResult::warning(first);
        return HealthResult::ok();
    }
};

void registerHomeMasterHealthChecks() {
    static SdHealthCheck sd;
    static BackupHealthCheck bk;
    HealthMonitor::getInstance().registerCheck(&sd);
    HealthMonitor::getInstance().registerCheck(&bk);
}
