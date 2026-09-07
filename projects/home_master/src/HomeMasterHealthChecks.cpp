// ============================================================================
// HomeMasterHealthChecks.cpp — проверки ПАЗ мастера (M0: hm.sd)
// 0.6.8: hm.fleet (присутствие парка по сессиям брокера) + гистерезис
// доменных проверок (вариант Б — в профиле, ядро не тронуто).
// 0.6.9 (ядро 5.8.7): ConfirmState ДЕМОНТИРОВАН — подтверждение переходов
// переехало в ядерный HealthMonitor (paz.confirm_bad / paz.confirm_ok,
// общий механизм для всех проверок всех профилей). Профильные проверки
// снова возвращают сырой вердикт прогона.
// ============================================================================
#include "HomeMasterHealthChecks.h"
#include "SdService.h"
#include "BackupService.h"
#include "OtaMirrorService.h"
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

// hm.ota — OTA-зеркало парка (0.6.1). Выключенный модуль — штатно (Ok).
// Беда = зеркало хоста ни разу не собрано и есть ошибка (устройство такой
// версии не получит вообще). Ошибка опроса при живом зеркале — не беда:
// раздача ранее снятого продолжается. Critical не ставим — функция
// сервисная, не защитная.
class OtaMirrorHealthCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "hm.ota"; }
    uint32_t intervalMs() const override { return 60000; }

    HealthResult run() override {
        OtaMirrorService& om = OtaMirrorService::getInstance();
        if (!om.isEnabled()) return HealthResult::ok();
        char first[40] = "";
        uint8_t bad = om.troubleCount(first, sizeof(first));
        if (bad > 0) return HealthResult::warning(first);
        return HealthResult::ok();
    }
};

// hm.fleet — присутствие парка (0.6.8, копилка M5). Закрывает дыру приёмки
// M3.3: «брокер видит отсутствие замка, а ПАЗ здорова» — ПАЗ следил за
// МАСТЕРОМ, но не за парком. Ожидаемый состав — реестр bk.hosts, факт
// присутствия — активные MQTT-сессии брокера (clientId = hostname).
//   · bk выключен или имён ещё нет (свежий мастер) — Ok, нет данных;
//   · ЧАСТЬ парка молчит — Warning с именем первого молчащего;
//   · молчат ВСЕ — Critical: это похоже не на «устройство уснуло», а на
//     сломанный брокер/сеть — ситуация защитная, не сервисная.
// Порог молчания — ядерный: paz.confirm_bad × интервал 60 с (дефолт
// ~3 минуты), с 5.8.7 настраивается в конфиге («Система»).
class FleetHealthCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "hm.fleet"; }
    uint32_t intervalMs() const override { return 60000; }

    HealthResult run() override {
        BackupService& bk = BackupService::getInstance();
        if (!bk.isEnabled()) return HealthResult::ok();
        char first[40] = "";
        uint8_t checkable = 0;
        uint8_t silent = bk.fleetSilence(first, sizeof(first), checkable);
        if (checkable == 0 || silent == 0) return HealthResult::ok();
        if (silent >= checkable) {
            char msg[40];
            snprintf(msg, sizeof(msg), "молчит весь парк (%u)",
                     (unsigned)checkable);
            return HealthResult::critical(msg);
        }
        return HealthResult::warning(first);
    }
};

void registerHomeMasterHealthChecks() {
    static SdHealthCheck sd;
    static BackupHealthCheck bk;
    static OtaMirrorHealthCheck otam;
    static FleetHealthCheck fleet;
    HealthMonitor::getInstance().registerCheck(&sd);
    HealthMonitor::getInstance().registerCheck(&bk);
    HealthMonitor::getInstance().registerCheck(&otam);
    HealthMonitor::getInstance().registerCheck(&fleet);
}
