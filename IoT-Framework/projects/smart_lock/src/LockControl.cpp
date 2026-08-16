// ============================================================================
// LockControl.cpp — реализация исполнителя СКУД (см. шапку .h)
// ============================================================================
#include "LockControl.h"
#include "SmartLockProfile.h"
#include <services/ConfigService.h>
#include <core/Events.h>

LockControl& LockControl::getInstance() {
    static LockControl instance;
    return instance;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void LockControl::init() {
    const SmartLockPins& p = SmartLockProfile::pins();

    // Реле: сначала БЕЗОПАСНОЕ состояние, потом выход (иначе дёргание
    // замка при старте — урок монолита, стартовый фильтр CPC1008).
    pinMode(p.lockRelay, OUTPUT);
    relayWrite(false);

    pinMode(p.doorSense, INPUT);            // чистый вход (только 35/36/39)
    pinMode(p.buttonExit, INPUT_PULLUP);
    pinMode(p.localModeJumper, INPUT);      // внешняя подтяжка 10 кОм

    _doorOpen = digitalRead(p.doorSense) == HIGH;
    _doorRaw = _doorOpen;
    _doorSince = _doorOpen ? millis() : 0;
    _localJumper = digitalRead(p.localModeJumper) == LOW;   // замкнут на GND
    if (_localJumper) {
        log(LogLevel::Warning, "LOCAL MODE jumper set — строгий локальный режим");
    }

    _initialized = true;
    log(LogLevel::Info, "actuator ready: relay=%u door=%u btn=%u cycles=%lu",
        p.lockRelay, p.doorSense, p.buttonExit,
        (unsigned long)cfgGetUInt("lock.cycle_count", 0));
}

void LockControl::start() { _started = true; }

void LockControl::stop() {
    // Останов модуля (ребут/OTA) — реле в безопасное состояние
    emergencySecure();
    _started = false;
}

// ============================================================================
// TICK: импульс реле, геркон, кнопка (монолит: loop-автоматы)
// ============================================================================
void LockControl::tick() {
    const SmartLockPins& p = SmartLockProfile::pins();
    uint32_t now = millis();

    // --- Конец импульса открытия (TRIGGER-удержание импульс отменяет) ------
    if (_pulseUntil != 0 && !_triggerHold &&
        (int32_t)(now - _pulseUntil) >= 0) {
        finishPulse("timeout");
    }

    // --- Геркон (дебаунс 30 мс) ---------------------------------------------
    bool raw = digitalRead(p.doorSense) == HIGH;   // HIGH = дверь открыта
    if (raw != _doorRaw) { _doorRaw = raw; _doorRawSince = now; }
    if (raw != _doorOpen && now - _doorRawSince >= SL_DOOR_DEBOUNCE_MS) {
        _doorOpen = raw;
        _doorSince = raw ? now : 0;
        postEvent(raw ? sl_ev::doorOpen() : sl_ev::doorClosed(), nullptr);
    }

    // --- Кнопка EXIT (дебаунс 50 мс, фронт нажатия) ---------------------------
    bool btn = digitalRead(p.buttonExit) == HIGH;  // HIGH = отпущена
    if (btn != _btnRaw) { _btnRaw = btn; _btnRawSince = now; }
    if (btn != _btnStable && now - _btnRawSince >= SL_BTN_DEBOUNCE_MS) {
        _btnStable = btn;
        if (!btn) {
            // Сырой факт нажатия — разрешено ли по расписанию, решает
            // SmartLockApp (там Fail-Safe при мёртвом RTC)
            postEvent(sl_ev::exitButton(), nullptr);
        }
    }
}

// ============================================================================
// КОМАНДЫ
// ============================================================================
void LockControl::openPulse(SlOpenSource source, const char* cardId) {
    uint32_t openMs = cfgGetUInt("lock.open_ms", 3000);
    relayWrite(true);
    _pulseUntil = millis() + openMs;

    ShEventData d;
    d.clear();
    d.code = (int32_t)source;
    if (cardId) safeStrCopy(d.payload, sizeof(d.payload), cardId);
    else        safeStrCopy(d.payload, sizeof(d.payload), "-");
    postEvent(ACCESS_EVENT_UNLOCKED, &d);   // -> аудит B3 + MQTT-зеркало E2

    log(LogLevel::Info, "UNLOCK src=%d card=%s pulse=%lu ms",
        (int)source, d.payload, (unsigned long)openMs);
}

void LockControl::setTriggerHold(bool hold) {
    _triggerHold = hold;
    if (hold) {
        relayWrite(true);
        _pulseUntil = 0;                    // импульс не нужен — держим
        ShEventData d;
        d.clear();
        d.code = (int32_t)SlOpenSource::TRIGGER;
        safeStrCopy(d.payload, sizeof(d.payload), "TRIGGER");
        postEvent(ACCESS_EVENT_UNLOCKED, &d);
    } else {
        finishPulse("trigger_off");
    }
}

void LockControl::emergencySecure() {
    if (_relayActive) {
        log(LogLevel::Critical, "EMERGENCY SECURE: relay forced off");
    }
    _triggerHold = false;
    _pulseUntil = 0;
    relayWrite(false);
}

// ============================================================================
// ВНУТРЕННЯЯ КУХНЯ
// ============================================================================
void LockControl::relayWrite(bool active) {
    const SmartLockPins& p = SmartLockProfile::pins();
    bool failSecure = cfgGetBool("lock.fail_secure", false);
    // Fail-Secure (электромеханический): HIGH = открыт.
    // Fail-Safe (электромагнитный):  LOW = открыт (обесточен = открыт,
    // но "безопасное состояние при простое" — закрыт: fail-safe закрыт
    // подачей питания).
    int level = active ? (failSecure ? HIGH : LOW)
                       : (failSecure ? LOW : HIGH);
    digitalWrite(p.lockRelay, level);
    _relayActive = active;
    _relaySince = active ? millis() : 0;
}

void LockControl::finishPulse(const char* why) {
    _pulseUntil = 0;
    if (!_relayActive) return;
    relayWrite(false);
    postEvent(ACCESS_EVENT_LOCKED, nullptr);

    // Счётчик циклов — ресурс замка. 5.1.1: РЕВЕРТ на прямую идиому
    // cfgGetUInt + setInternal. Батч 5.1.0 через CounterService решал
    // НЕсуществующую проблему: ConfigService и так пишет NVS отложенно
    // (scheduleSave, дебаунс 1 с), а панель/apiStatus читают КОНФИГ —
    // с RAM-тенью счётчик «замерзал» до 10 циклов/10 минут, и живой
    // оператор счёл это поломкой («перестал реагировать»). Живое поле
    // важнее микроэкономии. CounterService остаётся для standalone-
    // счётчиков и PCNT (энергия, вода), где нет живого поля конфига.
    uint32_t cycles = ConfigService::getInstance().getUInt("lock.cycle_count") + 1;
    char cbuf[12];
    snprintf(cbuf, sizeof(cbuf), "%lu", (unsigned long)cycles);
    ConfigService::getInstance().setInternal("lock.cycle_count", cbuf);
    log(LogLevel::Info, "LOCKED (%s), cycles=%lu", why, (unsigned long)cycles);
}
