// ============================================================================
// SmartLockEvents.h — СОБЫТИЯ ПРОФИЛЯ SMART_LOCK (диапазон 0x1000+)
// ============================================================================
// База диапазона НЕ константа: её выдаёт ResourceManager::claimEventRange()
// при регистрации модулей (урок v4.2.2: жёстко прошитые ID = коллизии).
// Обращение: sl_ev::doorOpen() и т.д. До claimEventRange база = 0 — любое
// использование до регистрации является ошибкой программирования (conformance
// стенд D1 проверяет, что диапазон получен).
//
// Что здесь, а что в ядре (реестр Events.h):
//   · факты доступа/замка  -> ЯДЕРНЫЕ ACCESS_EVENT_GRANTED/DENIED/LOCKED/
//                             UNLOCKED (0x0400+): их автоматически подхватывают
//                             аудит (B3) и MQTT-зеркало (E2) без правок ядра;
//   · сенсорика и политика СКУД -> ЗДЕСЬ (дверь, кнопка, режимы, база карт).
// ============================================================================
#pragma once

#include <cstdint>

namespace sl_ev {

// База диапазона (inline-переменная C++17 — профиль header-only).
// Записывается один раз из SmartLockProfile::registerModules().
inline int32_t g_base = 0;

// Смещения внутри диапазона (шаг claimEventRange = 0x40 — запас 64 ID).
inline int32_t doorOpen()       { return g_base + 0x00; } // геркон: дверь открыта
inline int32_t doorClosed()     { return g_base + 0x01; } // геркон: дверь закрыта
inline int32_t exitButton()     { return g_base + 0x02; } // кнопка EXIT (code: 1=разрешено, 0=запрещено расписанием)
inline int32_t modeChanged()    { return g_base + 0x03; } // code: LockMode (0/1/2)
inline int32_t doorAlarm()      { return g_base + 0x04; } // дверь открыта дольше lock.door_alarm_min
inline int32_t doorAlarmOff()   { return g_base + 0x05; } // тревога снята (дверь закрыта)
inline int32_t webReadCard()    { return g_base + 0x06; } // карта поймана для веб-UI (payload: HEX)
inline int32_t masterLearned()  { return g_base + 0x07; } // мастер-ключ создан на пустой базе
inline int32_t cardAdded()      { return g_base + 0x08; } // payload: HEX, code: KeyType
inline int32_t cardRemoved()    { return g_base + 0x09; } // payload: HEX (в т.ч. сгоревший одноразовый)
inline int32_t dbIntegrityFail(){ return g_base + 0x0A; } // users.json повреждён (ПАЗ, code: 1=восстановлен из бэкапа)
inline int32_t forcedEntry()    { return g_base + 0x0B; } // дверь открыта без разблокировки (взлом)

} // namespace sl_ev

// ============================================================================
// РЕЖИМЫ СКУД (монолит: enum LockMode)
// ============================================================================
// NORMAL  — обычный проход по картам;
// ACCEPT  — авто-запись каждой новой карты в базу (мастер-ключом включается);
// TRIGGER — замок открыт постоянно, до следующего поднесения мастер-ключа.
enum class LockMode : uint8_t { NORMAL = 0, ACCEPT = 1, TRIGGER = 2 };

// ============================================================================
// ТИПЫ КЛЮЧЕЙ (монолит: enum KeyType) — порядок НЕ менять (совместимость
// users.json v2.5.0).
// ============================================================================
enum class KeyType : uint8_t {
    MASTER    = 0,   // мастер-ключ: цикл режимов, доступ всегда
    PERMANENT = 1,   // постоянный жилец
    TEMPORARY = 2,   // временный: действует до expiry (unix)
    ONETIME   = 3    // одноразовый: сгорает после первого прохода
};

// Источники открытия замка (code события ядра ACCESS_EVENT_UNLOCKED).
enum class SlOpenSource : int32_t {
    CARD = 1, BUTTON = 2, WEB = 3, TRIGGER = 4, MQTT = 5
};
