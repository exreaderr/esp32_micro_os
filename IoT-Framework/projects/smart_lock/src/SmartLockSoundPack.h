// ============================================================================
// SmartLockSoundPack.h — ЗВУКОВОЕ НАПОЛНЕНИЕ СКУД (ISoundPack, чистые данные)
// ============================================================================
// Привязка семантических имён к раскладке SD (SmartLockTracks.h). Кода нет:
// смена озвучки/языка = другой SoundPack, прошивка не меняется.
//
// Динамический контент (имя жильца, папка 02) в таблице отсутствует
// намеренно: трек зависит от записи в базе карт и играется через
// AudioService::sayRaw(FOLDER_NAMES, user->track, ...).
// ============================================================================
#pragma once

#include <services/ISoundPack.h>
#include "SmartLockTracks.h"

class SmartLockSoundPack : public ISoundPack {
public:
    const char* packName() const override { return "smart_lock_ru_v1"; }

    const SoundPhrase* phrases(uint8_t& count) const override {
        using P = SndPriority;
        static const SoundPhrase TABLE[] = {
            // --- Слоты ядра (ядро озвучивает системные события само) ------
            { "sys.boot",          sl_track::FOLDER_ACTION, sl_track::SYS_START,
              (uint8_t)P::Normal,    0 },
            { "sys.alarm",         sl_track::SIREN_FOLDER,  sl_track::SIREN_TRACK,
              (uint8_t)P::Alarm,     SND_FLAG_LOOP },

            // --- Режимы СКУД ------------------------------------------------
            { "sl.mode.normal",    3, sl_track::MODE_NORMAL,  (uint8_t)P::Normal, 0 },
            { "sl.mode.accept",    3, sl_track::MODE_ACCEPT,  (uint8_t)P::Normal, 0 },
            { "sl.mode.trigger",   3, sl_track::MODE_TRIGGER, (uint8_t)P::Normal, 0 },

            // --- База карт ----------------------------------------------------
            { "sl.key.added",      3, sl_track::KEY_ADDED,    (uint8_t)P::Normal, 0 },
            { "sl.key.exists",     3, sl_track::KEY_EXISTS,   (uint8_t)P::Normal, 0 },
            { "sl.key.deleted",    3, sl_track::KEY_DELETED,  (uint8_t)P::Normal, 0 },
            { "sl.base.cleared",   3, sl_track::BASE_CLEARED, (uint8_t)P::Important, 0 },
            { "sl.memory.full",    3, sl_track::MEMORY_FULL,  (uint8_t)P::Important, 0 },

            // --- Доступ ---------------------------------------------------------
            { "sl.access.granted", 3, sl_track::ACCESS_GRANTED, (uint8_t)P::Normal, 0 },
            { "sl.access.denied",  3, sl_track::ACCESS_DENIED,  (uint8_t)P::Normal, 0 },
            { "sl.blocked.admin",  3, sl_track::BLOCKED_BY_ADMIN, (uint8_t)P::Normal, 0 },
            { "sl.remote.open",    3, sl_track::REMOTE_OPEN,    (uint8_t)P::Normal, 0 },
            { "sl.beep",           3, sl_track::BEEP,           (uint8_t)P::Ambient, 0 },

            // --- Приветствия (цепочка: время суток -> имя -> доступ разрешён) --
            { "sl.greet.morning",  1, 1, (uint8_t)P::Ambient, 0 },
            { "sl.greet.day",      1, 2, (uint8_t)P::Ambient, 0 },
            { "sl.greet.evening",  1, 3, (uint8_t)P::Ambient, 0 },
            { "sl.greet.night",    1, 4, (uint8_t)P::Ambient, 0 },

            // --- Сеть/система (штатные) ------------------------------------------
            { "sl.net.ok",         3, sl_track::NET_OK,     (uint8_t)P::Ambient, 0 },
            { "sl.mqtt.ok",        3, sl_track::MQTT_OK,    (uint8_t)P::Ambient, 0 },
            { "sl.power.restored", 3, sl_track::POWER_RESTORED, (uint8_t)P::Ambient, 0 },
            { "sl.ota.start",      3, sl_track::OTA_START,   (uint8_t)P::Ambient, 0 },
            { "sl.ota.success",    3, sl_track::OTA_SUCCESS, (uint8_t)P::Normal, 0 },
            { "sl.ota.failed",     3, sl_track::OTA_FAILED,  (uint8_t)P::Important, 0 },

            // --- Аварийные: папка /ADVERT (прервать и продолжить) ---------------
            // folder=0: адресуется только трек (AudioService -> playAdvert).
            { "sl.door.timeout",   0, sl_track::ADVERT_DOOR_TIMEOUT,
              (uint8_t)P::Important, SND_FLAG_ADVERT },
            { "sl.power.lost",     0, sl_track::ADVERT_POWER_LOST,
              (uint8_t)P::Important, SND_FLAG_ADVERT },
            { "sl.alarm.forced",   0, sl_track::ADVERT_ALARM_FORCED,
              (uint8_t)P::Alarm,     SND_FLAG_ADVERT },
        };
        count = sizeof(TABLE) / sizeof(TABLE[0]);
        return TABLE;
    }
};
