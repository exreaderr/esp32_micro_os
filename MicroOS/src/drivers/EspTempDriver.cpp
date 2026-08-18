// ============================================================================
// EspTempDriver.cpp — реализация драйвера температуры кристалла
// ============================================================================
#include "EspTempDriver.h"
#include "../core/Events.h"
#include <Arduino.h>
#include <math.h>

// Доступ к сенсору — через Arduino API temperatureRead() (esp32-hal.h).
// ПРИЧИНА (выявлена линковкой на core 3.3.11): новый ESP-IDF драйвер
// esp_driver_tsens НЕ собран для классического ESP32 (libesp_driver_tsens.a
// есть только под s2/s3/c3/c6/h2/p4 — новый драйвер не поддерживает чип ESP32).
// temperatureRead() работает на всех целях, включая WT32-ETH01.

EspTempDriver& EspTempDriver::getInstance() {
    static EspTempDriver instance;
    return instance;
}

// ============================================================================
// INIT: сенсор не требует установки — пробное чтение проверяет живость
// ============================================================================
bool EspTempDriver::init() {
    float t = temperatureRead();
    // На классическом ESP32 сенсор грубый (±5°C), но чтение всегда даёт
    // конечное значение; NAN означало бы отсутствие сенсора на цели.
    _healthy = !isnan(t);
    if (_healthy) { _lastTempC = t; _readSeq++; }
    _bootGraceUntilMs = millis() + ESP_TEMP_BOOT_GRACE_MS;
    return _healthy;
}

// ============================================================================
// POLL: чтение + публикация факта и переходов состояния
// ============================================================================
void EspTempDriver::poll() {
    float tempC = temperatureRead();

    if (isnan(tempC)) {
        if (_healthy) {
            _healthy = false;   // isHealthy() -> HealthMonitor заметит
        }
        return;
    }
    _healthy = true;
    _lastTempC = tempC;
    _readSeq++;   // пульс: дежурный смотрит на него, а не на значение

    // --- ФАКТ: всегда публикуем измерение (для телеметрии/графиков) -------
    // code = температура x10 в целых (52.4°C -> 524) — без float в payload.
    ShEventData d; d.clear();
    d.sourceModule = 0;   // драйвер — не модуль; источник виден по eventId
    d.code = (int32_t)(tempC * 10.0f);
    EventBus::getInstance().post(DRV_EVENT_TEMP_UPDATE, &d);

    // --- ПЕРЕХОДЫ: только при смене состояния (с гистерезисом) -------------
    // Пороги — поля _warnC/_critC (sys.temp_* из конфига; умолчания —
    // константы монолита). Урок 5.0.x: закрытый корпус + PoE-линия держат
    // кристалл за 75°C — «крит: 1» жил постоянно; порог обязан быть ручкой.
    TempState newState = _state;
    switch (_state) {
        case TempState::Normal:
            if (tempC >= _critC)      newState = TempState::Critical;
            else if (tempC >= _warnC) newState = TempState::Warning;
            break;
        case TempState::Warning:
            if (tempC >= _critC)      newState = TempState::Critical;
            else if (tempC < _warnC - _hystC)  newState = TempState::Normal;
            break;
        case TempState::Critical:
            if (tempC < _critC - _hystC)       newState = TempState::Warning;
            break;
    }
    if (newState != _state) {
        // Бут-грейс: пороги из конфига могли ещё не доехать до драйвера
        // (TelemetryService применяет в init-фазе) — переход запоминаем,
        // публикацию откладываем; после окна poll сам опубликует факт.
        if ((int32_t)(millis() - _bootGraceUntilMs) >= 0) {
            publishTransition(newState, tempC);
        } else {
            _state = newState;   // молча: публикация после грейс-окна
        }
    }

    // --- ТЕРМИЧЕСКАЯ ПАНИКА (залежь №2) -------------------------------------
    // Отдельно от стейт-машины: crit — «всё плохо, пора бить тревогу»,
    // panic — «кристалл варится, это почти наверняка кончится аварией».
    // Одноразовый фронт + свой гистерезис: HealthMonitor запишет в журнал
    // паник — после остывания/ребута будет видно, ЧТО именно случилось.
    if (tempC >= _panicC && !_panicActive) {
        _panicActive = true;
        ShEventData d; d.clear();
        d.code = (int32_t)(tempC * 10.0f);
        EventBus::getInstance().post(DRV_EVENT_TEMP_PANIC, &d);
    } else if (_panicActive && tempC < _panicC - 3.0f) {
        _panicActive = false;
    }
}

// ============================================================================
// ПЕРЕХОДЫ СОСТОЯНИЯ
// ============================================================================
void EspTempDriver::publishTransition(TempState newState, float tempC) {
    _state = newState;

    ShEventData d; d.clear();
    d.code = (int32_t)(tempC * 10.0f);

    switch (newState) {
        case TempState::Warning:
            EventBus::getInstance().post(DRV_EVENT_TEMP_WARNING, &d);
            break;
        case TempState::Critical:
            EventBus::getInstance().post(DRV_EVENT_TEMP_CRITICAL, &d);
            break;
        case TempState::Normal:
            // Возврат в норму: HealthMonitor услышит RECOVERED сам
            // (его IHealthCheck читает getState()); отдельное событие
            // не публикуем — шина для фактов, а не для каждого чиха.
            break;
    }
}
