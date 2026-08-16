// ============================================================================
// HomeMasterHealthChecks.cpp — проверки ПАЗ мастера (M0: hm.sd)
// ============================================================================
#include "HomeMasterHealthChecks.h"
#include "SdService.h"
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

void registerHomeMasterHealthChecks() {
    static SdHealthCheck sd;
    HealthMonitor::getInstance().registerCheck(&sd);
}
