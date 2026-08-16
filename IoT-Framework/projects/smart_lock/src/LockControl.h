// ============================================================================
// LockControl.h — ИСПОЛНИТЕЛЬНАЯ ЧАСТЬ СКУД (профильный модуль, «руки»)
// ============================================================================
// Владелец ВСЕХ GPIO профиля: реле замка, геркон двери, кнопка EXIT,
// джампер локального режима. Перенос автомата замка из монолита v2.5.0
// (openLock/closeLock, части 3 и 6).
//
// Граница ответственности (урок v4.2.2: AccessControl знал и про карты,
// и про веб): LockControl НЕ знает, КТО и ПОЧЕМУ открывает. Он умеет:
//   · импульс открытия (lock.open_ms) с полярностью lock.fail_secure;
//   · удержание TRIGGER (замок открыт до команды);
//   · аварийное обесточивание emergencySecure() — для ПАЗ (RelayStuckCheck);
//   · счётчик циклов (ресурс замка) в lock.cycle_count;
//   · публикация фактов: ACCESS_EVENT_UNLOCKED/LOCKED (ядро — аудит B3
//     и MQTT-зеркало E2 подхватывают сами), sl_ev::doorOpen/doorClosed,
//     sl_ev::exitButton (сырой факт нажатия — политика в SmartLockApp).
// ============================================================================
#pragma once

#include <core/ModuleBase.h>
#include "SmartLockEvents.h"

// Дебаунс кнопки EXIT (монолит: 50 мс)
constexpr uint32_t SL_BTN_DEBOUNCE_MS  = 50;
constexpr uint32_t SL_DOOR_DEBOUNCE_MS = 30;

class LockControl : public ModuleBase {
public:
    static LockControl& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "LockControl"; }
    const char* getVersion() const override { return "5.1.1"; }
    ModuleId getModuleId() const override { return 0x1000; }   // профиль

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;                 // импульс, геркон, кнопка
    uint32_t getTickIntervalMs() const override { return 20; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- КОМАНДЫ ИСПОЛНИТЕЛЮ (вызывает SmartLockApp / API) ------------------
    /// Импульс открытия: реле активно lock.open_ms, затем — безопасное
    /// состояние. cardId — для аудита (payload события), может быть nullptr.
    void openPulse(SlOpenSource source, const char* cardId);
    /// Удержание TRIGGER: открыть до setTriggerHold(false).
    void setTriggerHold(bool hold);
    bool isTriggerHold() const { return _triggerHold; }
    /// Аварийно обесточить реле (ПАЗ: залипание). Идемпотентно.
    void emergencySecure();

    // --- ФАКТЫ (для API, health-checks, SmartLockApp) ------------------------
    bool isRelayActive() const { return _relayActive; }
    uint32_t relayActiveSinceMs() const { return _relayActive ? _relaySince : 0; }
    bool isDoorOpen() const { return _doorOpen; }
    uint32_t doorOpenSinceMs() const { return _doorOpen ? _doorSince : 0; }
    bool isLocalJumperSet() const { return _localJumper; }

private:
    LockControl() = default;

    void relayWrite(bool active);       // с учётом полярности lock.fail_secure
    void finishPulse(const char* why);  // конец импульса -> LOCKED + циклы

    bool _relayActive  = false;
    bool _triggerHold  = false;
    uint32_t _relaySince = 0;
    uint32_t _pulseUntil = 0;           // 0 — импульса нет

    bool _doorOpen = false;
    uint32_t _doorSince = 0;
    bool _doorRaw = false;              // мгновенное (до дебаунса)
    uint32_t _doorRawSince = 0;

    bool _btnRaw = true;                // INPUT_PULLUP: true = отпущена
    uint32_t _btnRawSince = 0;
    bool _btnStable = true;

    bool _localJumper = false;
};
