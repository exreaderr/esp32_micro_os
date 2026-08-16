// ============================================================================
// SmartLockHealthChecks.cpp — проверки ПАЗ СКУД (см. шапку .h)
// ============================================================================
#include "SmartLockHealthChecks.h"
#include "SmartLockApp.h"
#include "CardStore.h"
#include "LockControl.h"
#include <services/HealthMonitor.h>
#include <services/ConfigService.h>
#include <services/StorageService.h>

// ============================================================================
// 1. ЗАЛИПАНИЕ РЕЛЕ ЗАМКА (монолит: автомат защиты реле, часть 3)
// ============================================================================
// Правило монолита: реле физически активно дольше lock.open_ms + 2000 мс →
// принудительно обесточить в безопасное состояние + CRITICAL.
// Стартовый фильтр 3 с (стабилизация CPC1008/ADuM) — внутри проверки.
class RelayStuckCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "lock.relay_stuck"; }

    HealthResult run() override {
        if (millis() < 3000) return HealthResult::ok();   // стартовый фильтр

        LockControl& lock = LockControl::getInstance();
        // TRIGGER-удержание — ЛЕГАЛЬНОЕ долгое открытие, не залипание
        if (!lock.isRelayActive() || lock.isTriggerHold()) {
            return HealthResult::ok();
        }
        uint32_t since = lock.relayActiveSinceMs();
        if (since == 0) return HealthResult::ok();

        uint32_t limit = cfgGetUInt("lock.open_ms", 3000) + 2000;
        if (millis() - since > limit) {
            // Аварийное действие — через владельца исполнителя, не в пин
            lock.emergencySecure();
            return HealthResult::critical("RELAY_STICK_ALARM");
        }
        return HealthResult::ok();
    }
};

// ============================================================================
// 2. ТРЕВОГИ ДВЕРИ (монолит: door_alarm + ПАЗ-взлом, часть 6)
// ============================================================================
// Таймеры ведёт SmartLockApp; здесь — сводный вердикт для HealthMonitor:
// взлом — CRITICAL (сирена уже звучит), долго открыта — WARNING.
class DoorAlarmCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "lock.door_alarm"; }
    HealthResult run() override {
        SmartLockApp& app = SmartLockApp::getInstance();
        if (app.isForcedAlarm()) return HealthResult::critical("FORCED_ENTRY");
        if (app.isDoorAlarm())   return HealthResult::warning("DOOR_OPEN_TOO_LONG");
        return HealthResult::ok();
    }
};

// ============================================================================
// 3. ЦЕЛОСТНОСТЬ БАЗЫ КАРТ (монолит: checkLittleFSHealth, часть 3)
// ============================================================================
// Раз в сутки: users.json цел + бэкап свежий. При порче — перезагрузка
// базы владельцем (CardStore::load делает эскалацию .bak → NVS сам).
class UserDbCheck : public IHealthCheck {
public:
    const char* checkName() const override { return "lock.user_db"; }
    uint32_t intervalMs() const override { return 24UL * 60 * 60 * 1000; }

    HealthResult run() override {
        if (!StorageService::getInstance().fileValid("/users.json")) {
            bool ok = CardStore::getInstance().load();   // .bak -> NVS эскалация
            return ok ? HealthResult::warning("DB_RESTORED_FROM_BACKUP")
                      : HealthResult::critical("DB_LOST");
        }
        return HealthResult::ok();
    }
};

// ============================================================================
// РЕГИСТРАЦИЯ
// ============================================================================
void registerSmartLockHealthChecks() {
    HealthMonitor& hm = HealthMonitor::getInstance();
    // Статические объекты: владение у профиля, ПАЗ не удаляет (контракт)
    static RelayStuckCheck relay;
    static DoorAlarmCheck  door;
    static UserDbCheck     db;
    hm.registerCheck(&relay);
    hm.registerCheck(&door);
    hm.registerCheck(&db);
}
