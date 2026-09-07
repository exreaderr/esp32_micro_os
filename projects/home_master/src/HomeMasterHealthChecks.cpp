// ============================================================================
// HomeMasterHealthChecks.cpp — проверки ПАЗ мастера (M0: hm.sd)
// 0.6.8: hm.fleet (присутствие парка по сессиям брокера) + гистерезис
// доменных проверок (вариант Б — в профиле, ядро не тронуто).
// ============================================================================
#include "HomeMasterHealthChecks.h"
#include "SdService.h"
#include "BackupService.h"
#include "OtaMirrorService.h"
#include <services/ConfigService.h>
#include <services/TimeService.h>
#include <services/HealthMonitor.h>
#include <services/IHealthCheck.h>

// --- Подтверждение переходов (0.6.8, вариант Б) ------------------------------
// Долг из приёмки M3.3: «мигающий» хост (offline→ok→offline) заставлял
// доменные проверки клацать Warning/recovered каждые пару минут — счётчик
// предупреждений рос на шуме, а не на состояниях. Теперь беда признаётся
// бедой после FLEET_BAD_CONFIRM подряд плохих прогонов, восстановление —
// после FLEET_OK_CONFIRM подряд хороших. В ПАЗ уходят только устойчивые
// состояния. При переезде флота на ядро 5.8.7 механизм уйдёт в
// HealthMonitor (общий для всех проверок), отсюда — уберём.
class ConfirmState {
public:
    /// Подаём сырой вердикт прогона, получаем ПОДТВЕРЖДЁННОЕ состояние.
    bool feed(bool badNow) {
        if (badNow) {
            _okRun = 0;
            if (++_badRun >= BAD_CONFIRM) _state = true;
        } else {
            _badRun = 0;
            if (++_okRun >= OK_CONFIRM) _state = false;
        }
        return _state;
    }
private:
    static constexpr uint8_t BAD_CONFIRM = 3;   // ~3 мин при прогоне 60 с
    static constexpr uint8_t OK_CONFIRM  = 2;   // ~2 мин на возврат
    uint8_t _badRun = 0;
    uint8_t _okRun  = 0;
    bool    _state  = false;
};

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
        if (!bk.isEnabled()) { _c.feed(false); return HealthResult::ok(); }
        char first[40] = "";
        uint8_t bad = bk.troubleCount(first, sizeof(first));
        if (_c.feed(bad > 0)) return HealthResult::warning(first);
        return HealthResult::ok();
    }
private:
    ConfirmState _c;
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
        if (!om.isEnabled()) { _c.feed(false); return HealthResult::ok(); }
        char first[40] = "";
        uint8_t bad = om.troubleCount(first, sizeof(first));
        if (_c.feed(bad > 0)) return HealthResult::warning(first);
        return HealthResult::ok();
    }
private:
    ConfirmState _c;
};

// hm.fleet — присутствие парка (0.6.8, копилка M5). Закрывает дыру приёмки
// M3.3: «брокер видит отсутствие замка, а ПАЗ здорова» — ПАЗ следил за
// МАСТЕРОМ, но не за парком. Ожидаемый состав — реестр bk.hosts, факт
// присутствия — активные MQTT-сессии брокера (clientId = hostname).
//   · bk выключен или имён ещё нет (свежий мастер) — Ok, нет данных;
//   · ЧАСТЬ парка молчит — Warning с именем первого молчащего;
//   · молчат ВСЕ — Critical: это похоже не на «устройство уснуло», а на
//     сломанный брокер/сеть — ситуация защитная, не сервисная.
// Порог: 60 с × подтверждение 3 прогонами = тревога через ~3 мин молчания
// (константа профиля; в конфиг — при переезде на ядро 5.8.7, решение
// владельца 07.09.2026).
class FleetHealthCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "hm.fleet"; }
    uint32_t intervalMs() const override { return 60000; }

    HealthResult run() override {
        BackupService& bk = BackupService::getInstance();
        if (!bk.isEnabled()) { _c.feed(false); return HealthResult::ok(); }
        char first[40] = "";
        uint8_t checkable = 0;
        uint8_t silent = bk.fleetSilence(first, sizeof(first), checkable);
        if (checkable == 0) { _c.feed(false); return HealthResult::ok(); }
        if (!_c.feed(silent > 0)) return HealthResult::ok();
        if (silent >= checkable) {
            char msg[40];
            snprintf(msg, sizeof(msg), "молчит весь парк (%u)",
                     (unsigned)checkable);
            return HealthResult::critical(msg);
        }
        return HealthResult::warning(first);
    }
private:
    ConfirmState _c;
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
