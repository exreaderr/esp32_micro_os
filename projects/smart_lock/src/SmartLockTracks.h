// ============================================================================
// SmartLockTracks.h — КАРТА SD-КАРТЫ DFPLAYER (из монолита v2.5.0, часть 1)
// ============================================================================
// Физическая раскладка звуковых файлов. Семантическая привязка к событиям —
// в SmartLockSoundPack.h (данные ISoundPack); динамический контент (имена
// жильцов, папка 02) играется через AudioService::sayRaw().
//
// SD-карта:
//   /01/001..004.mp3  — приветствия по времени суток;
//   /02/001..099.mp3  — имена жильцов (track = voice_track из базы карт);
//   /03/001..028.mp3  — служебные фразы (enum Action ниже);
//   /04/099.mp3       — сирена ПАЗ (зацикленная, SndPriority::Alarm);
//   /ADVERT/001..003  — спец-фразы «прервать и продолжить» (системные/аварийные:
//                       001 дверь открыта долго, 002 питание пропало,
//                       003 тревога взлома). На клоне MP3-TF-16P аппаратный
//                       resume ненадёжен — AudioService эмулирует программно
//                       (audio.use_advert=false по умолчанию).
// ============================================================================
#pragma once
#include <cstdint>

namespace sl_track {

// --- Папки ------------------------------------------------------------------
constexpr uint8_t FOLDER_TIME   = 1;  // 001 утро / 002 день / 003 вечер / 004 ночь
constexpr uint8_t FOLDER_NAMES  = 2;  // 001–099 — имена жильцов (voice_track)
constexpr uint8_t FOLDER_ACTION = 3;  // служебные фразы (ниже)
constexpr uint8_t SIREN_FOLDER  = 4;  // сирена ПАЗ
constexpr uint8_t SIREN_TRACK   = 99; // /04/099.mp3 — НЕ корневой play(99):
                                      // глобальный индекс на клоне зависит от
                                      // очерёдности записи файлов — ненадёжен!

// --- Папка /ADVERT (спец-фразы; folder=0 в SoundPhrase, играет playAdvert) --
enum Advert : uint8_t {
    ADVERT_DOOR_TIMEOUT = 1,
    ADVERT_POWER_LOST   = 2,
    ADVERT_ALARM_FORCED = 3
};

// --- Папка 03: служебные фразы ----------------------------------------------
enum Action : uint8_t {
    MODE_NORMAL      = 1,
    MODE_ACCEPT      = 2,
    MODE_TRIGGER     = 3,
    KEY_ADDED        = 4,
    KEY_EXISTS       = 5,
    KEY_DELETED      = 6,
    BASE_CLEARED     = 7,
    MEMORY_FULL      = 8,
    ACCESS_GRANTED   = 9,
    ACCESS_DENIED    = 10,
    DOOR_TIMEOUT     = 11,
    ALARM_FORCED     = 12,
    SYS_START        = 13,
    NET_OK           = 14,
    NET_ERROR        = 15,
    MQTT_OK          = 16,
    MQTT_ERROR       = 17,
    POWER_LOST       = 18,
    POWER_RESTORED   = 19,
    BATTERY_LOW      = 20,
    OTA_START        = 21,
    OTA_SUCCESS      = 22,
    OTA_FAILED       = 23,
    BLOCKED_BY_ADMIN = 24,
    REMOTE_OPEN      = 25,
    MODE_LOCAL       = 26,
    MODE_NET_ONLY    = 27,
    MODE_NET_MQTT    = 28,
    BEEP             = 5   // синоним KEY_EXISTS — короткий бип (совместимость)
};

// Тихий режим (lock.quiet_mode из монолита): пропускаются ТОЛЬКО эти треки.
// Политику применяет SmartLockApp до вызова AudioService (это решение профиля,
// не ядра: у светофора тихий режим может значить другое).
inline bool allowedInQuietMode(uint8_t actionTrack) {
    return actionTrack == DOOR_TIMEOUT || actionTrack == ALARM_FORCED ||
           actionTrack == POWER_LOST;
}

// Приветствие по времени суток (папка 01)
inline uint8_t timeOfDayTrack(uint8_t hour) {
    if (hour >= 6  && hour < 12) return 1;  // Доброе утро
    if (hour >= 12 && hour < 18) return 2;  // Добрый день
    if (hour >= 18 && hour < 23) return 3;  // Добрый вечер
    return 4;                               // Здравствуйте (ночь)
}

} // namespace sl_track
