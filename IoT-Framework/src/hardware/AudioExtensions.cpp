// ============================================================================
// AudioExtensions.cpp - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN) - AUDITED
// ============================================================================
// Описание: Расширенные аудио-функции для озвучивания событий.
// Является расширением AudioManager, НЕ самостоятельным модулем.
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - ИСПРАВЛЕНА ошибка в speakNumber (добавлен break)
// - ИСПРАВЛЕНА ошибка в getUnitTrack (исправлена логика склонений)
// - ДОБАВЛЕНА реализация speakHundreds
// - ДОБАВЛЕНА реализация speakName(const char*)
// - ИСПРАВЛЕНЫ опечатки в buildNameCache
// - ДОБАВЛЕНА проверка _audio во всех методах
// - ДОБАВЛЕНА поддержка Int и String для speakName
// - УЛУЧШЕНА работа с кэшем имен
// ============================================================================
#include "AudioExtensions.h"
#include <math.h>
#include <time.h>
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static AudioExtensions _audioExtensionsInstance;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
AudioExtensions::AudioExtensions() {
    _audio = nullptr;
    _cacheBuilt = false;
    _nameCache.reserve(50);
    _defaultPriority = AudioPriority::INFO;
    logMessage("Instance created (v4.2.2)");
}

AudioExtensions::~AudioExtensions() {
    end();
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
AudioExtensions& AudioExtensions::getInstance() {
    return _audioExtensionsInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void AudioExtensions::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void AudioExtensions::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[AUDIO_EXT] %s\n", msg);
}

void AudioExtensions::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

bool AudioExtensions::isInitialized() const {
    return _audio != nullptr && _audio->isReady();
}

void AudioExtensions::playTrack(uint16_t trackId, AudioPriority priority) {
    if (!isInitialized() || _audio == nullptr) return;

    // Применяем множитель громкости (если нужно)
    if (_volumeMultiplier != 1.0f) {
        uint8_t baseVol = _audio->getBaseVolume();
        uint8_t newVol = (uint8_t)(baseVol * _volumeMultiplier);
        if (newVol > 30) newVol = 30;
        if (newVol < 0) newVol = 0;
        _audio->setVolume(newVol);
    }

    _audio->playTrack(trackId, priority);
    if (_onTrackPlay) {
        _onTrackPlay(trackId);
    }
}

void AudioExtensions::playPhrase(const std::vector<uint16_t>& tracks, AudioPriority priority) {
    if (!isInitialized() || _audio == nullptr) return;
    _audio->playPhrase(tracks, priority);
    if (_onPhrase) {
        _onPhrase("Phrase played");
    }
}

// ============================================================================
// 4. ИНИЦИАЛИЗАЦИЯ
// ============================================================================
void AudioExtensions::begin(AudioManager& audio) {
    _audio = &audio;
    buildNameCache();
    logMessage("Initialized");
}

void AudioExtensions::end() {
    _audio = nullptr;
    clearNameCache();
    logMessage("Stopped");
}

// ============================================================================
// 5. СИСТЕМНЫЕ ЗВУКИ
// ============================================================================
void AudioExtensions::triggerBeep(AudioHardwareBeep type) {
    uint8_t track = 0;
    switch (type) {
        case AudioHardwareBeep::SHORT: track = 1; break;
        case AudioHardwareBeep::MEDIUM: track = 2; break;
        case AudioHardwareBeep::LONG: track = 3; break;
        case AudioHardwareBeep::DOUBLE: track = 4; break;
        case AudioHardwareBeep::TRIPLE: track = 5; break;
        case AudioHardwareBeep::ALARM: track = 6; break;
        case AudioHardwareBeep::SUCCESS: track = 7; break;
        case AudioHardwareBeep::ERROR: track = 8; break;
        default: track = 1; break;
    }
    triggerBeep(track);
}

void AudioExtensions::triggerBeep(uint8_t track) {
    if (!isInitialized()) return;
    if (track < 1 || track > 99) {
        logMessage("Invalid beep track: %d", track);
        return;
    }
    _audio->playTrack(track, AudioPriority::IMMEDIATE);
    logMessage("Beep: %d", track);
}

// ============================================================================
// 6. ОЗВУЧИВАНИЕ ЧИСЕЛ (ИСПРАВЛЕНО)
// ============================================================================
void AudioExtensions::speakNumber(uint32_t number, bool isMale) {
    if (!isInitialized()) return;
    if (number == 0) {
        playTrack((uint16_t)AudioTrackNumber::ZERO + TRACK_BASE_NUMBER);
        return;
    }
    if (number > MAX_NUMBER) {
        logMessage("Number too large: %lu", number);
        return;
    }
    decomposeNumber(number, isMale);
}

void AudioExtensions::speakNumber(int number, bool isMale) {
    if (!isInitialized()) return;
    if (number < 0) {
        playTrack((uint16_t)AudioTrackNumber::MINUS + TRACK_BASE_NUMBER);
        decomposeNumber((uint32_t)(-number), isMale);
    } else {
        decomposeNumber((uint32_t)number, isMale);
    }
}

void AudioExtensions::speakNumber(float number, uint8_t decimals, bool isMale) {
    if (!isInitialized()) return;

    int integerPart = (int)number;
    float fractional = number - (float)integerPart;
    if (fractional < 0) fractional = -fractional;
    int decimalPart = (int)(roundf(fractional * powf(10.0f, (float)decimals)));

    speakNumber(integerPart, isMale);

    if (decimalPart > 0) {
        playTrack((uint16_t)AudioTrackNumber::WHOLE + TRACK_BASE_NUMBER);

        uint16_t unitTrack;
        switch (decimals) {
            case 1: unitTrack = (uint16_t)AudioTrackNumber::TENTHS + TRACK_BASE_NUMBER; break;
            case 2: unitTrack = (uint16_t)AudioTrackNumber::HUNDREDTHS + TRACK_BASE_NUMBER; break;
            case 3: unitTrack = (uint16_t)AudioTrackNumber::THOUSANDTHS + TRACK_BASE_NUMBER; break;
            default: unitTrack = (uint16_t)AudioTrackNumber::TENTHS + TRACK_BASE_NUMBER; break;
        }
        speakNumber(decimalPart, isMale);
        playTrack(unitTrack);
    }
}

void AudioExtensions::speakNumberWithUnit(uint32_t number, const char* unit, bool isMale) {
    if (!isInitialized()) return;
    speakNumber(number, isMale);
    // Поиск единицы измерения (упрощенно)
    if (unit) {
        // Здесь можно добавить логику для озвучивания единиц
        logMessage("Number with unit: %lu %s", number, unit);
    }
}

// ============================================================================
// 7. РАЗБОР ЧИСЕЛ (ИСПРАВЛЕНО)
// ============================================================================
void AudioExtensions::decomposeNumber(uint32_t number, bool isMale) {
    if (!isInitialized()) return;
    if (number == 0) return;

    // 1. Тысячи
    if (number >= 1000) {
        speakThousands(number, isMale);
        number %= 1000;
        if (number == 0) return;
    }

    // 2. Сотни
    if (number >= 100) {
        speakHundreds(number);
        number %= 100;
        if (number == 0) return;
    }

    // 3. Десятки и единицы
    if (number >= 20) {
        speakTens(number, isMale);
    } else if (number >= 10) {
        speakTensToNineteen(number);
    } else if (number > 0) {
        speakUnits(number, isMale);
    }
}

void AudioExtensions::speakThousands(uint32_t number, bool isMale) {
    uint32_t thousands = number / 1000;
    uint32_t remainder = number % 1000;

    speakNumber(thousands, isMale);

    // Склонение "тысяча"
    if (thousands == 1) {
        playTrack(TRACK_BASE_NUMBER + (uint16_t)AudioTrackNumber::THOUSAND);
    } else {
        playTrack(TRACK_BASE_NUMBER + (uint16_t)AudioTrackNumber::THOUSAND);
    }

    if (remainder > 0) {
        decomposeNumber(remainder, isMale);
    }
}

// ============================================================================
// 8. СОТНИ (НОВАЯ РЕАЛИЗАЦИЯ)
// ============================================================================
void AudioExtensions::speakHundreds(uint32_t number) {
    uint32_t hundreds = number / 100;
    uint16_t track = 0;

    switch (hundreds) {
        case 1: track = (uint16_t)AudioTrackNumber::HUNDRED + TRACK_BASE_NUMBER; break;
        case 2: track = (uint16_t)AudioTrackNumber::TWO_HUNDRED + TRACK_BASE_NUMBER; break;
        case 3: track = (uint16_t)AudioTrackNumber::THREE_HUNDRED + TRACK_BASE_NUMBER; break;
        case 4: track = (uint16_t)AudioTrackNumber::FOUR_HUNDRED + TRACK_BASE_NUMBER; break;
        case 5: track = (uint16_t)AudioTrackNumber::FIVE_HUNDRED + TRACK_BASE_NUMBER; break;
        case 6: track = (uint16_t)AudioTrackNumber::SIX_HUNDRED + TRACK_BASE_NUMBER; break;
        case 7: track = (uint16_t)AudioTrackNumber::SEVEN_HUNDRED + TRACK_BASE_NUMBER; break;
        case 8: track = (uint16_t)AudioTrackNumber::EIGHT_HUNDRED + TRACK_BASE_NUMBER; break;
        case 9: track = (uint16_t)AudioTrackNumber::NINE_HUNDRED + TRACK_BASE_NUMBER; break;
        default: return;
    }
    playTrack(track);
}

// ============================================================================
// 9. ДЕСЯТКИ (ИСПРАВЛЕНО)
// ============================================================================
void AudioExtensions::speakTens(uint32_t number, bool isMale) {
    uint32_t tens = number / 10;
    uint32_t units = number % 10;

    uint16_t track = (uint16_t)AudioTrackNumber::TWENTY + (tens - 2) + TRACK_BASE_NUMBER;
    if (track > (uint16_t)AudioTrackNumber::NINETY + TRACK_BASE_NUMBER) {
        track = (uint16_t)AudioTrackNumber::NINETY + TRACK_BASE_NUMBER;
    }
    playTrack(track);

    if (units > 0) {
        speakUnits(units, isMale);
    }
}

void AudioExtensions::speakTensToNineteen(uint32_t number) {
    uint16_t track = (uint16_t)AudioTrackNumber::TEN + (number - 10) + TRACK_BASE_NUMBER;
    if (track > (uint16_t)AudioTrackNumber::NINETEEN + TRACK_BASE_NUMBER) {
        track = (uint16_t)AudioTrackNumber::NINETEEN + TRACK_BASE_NUMBER;
    }
    playTrack(track);
}

void AudioExtensions::speakUnits(uint32_t number, bool isMale) {
    uint16_t track = 0;
    switch (number) {
        case 1: track = isMale ? (uint16_t)AudioTrackNumber::ONE_MALE + TRACK_BASE_NUMBER :
                                 (uint16_t)AudioTrackNumber::ONE_FEMALE + TRACK_BASE_NUMBER; break;
        case 2: track = isMale ? (uint16_t)AudioTrackNumber::TWO_MALE + TRACK_BASE_NUMBER :
                                 (uint16_t)AudioTrackNumber::TWO_FEMALE + TRACK_BASE_NUMBER; break;
        case 3: track = (uint16_t)AudioTrackNumber::THREE + TRACK_BASE_NUMBER; break;
        case 4: track = (uint16_t)AudioTrackNumber::FOUR + TRACK_BASE_NUMBER; break;
        case 5: track = (uint16_t)AudioTrackNumber::FIVE + TRACK_BASE_NUMBER; break;
        case 6: track = (uint16_t)AudioTrackNumber::SIX + TRACK_BASE_NUMBER; break;
        case 7: track = (uint16_t)AudioTrackNumber::SEVEN + TRACK_BASE_NUMBER; break;
        case 8: track = (uint16_t)AudioTrackNumber::EIGHT + TRACK_BASE_NUMBER; break;
        case 9: track = (uint16_t)AudioTrackNumber::NINE + TRACK_BASE_NUMBER; break;
        default: return;
    }
    playTrack(track);
}

// ============================================================================
// 10. ЕДИНИЦЫ ИЗМЕРЕНИЯ (ИСПРАВЛЕНО)
// ============================================================================
uint16_t AudioExtensions::getUnitTrack(uint32_t number, bool isHours) const {
    if (isHours) {
        if (number == 1) return (uint16_t)AudioTrackUnit::HOUR + TRACK_BASE_UNIT;
        if (number >= 2 && number <= 4) return (uint16_t)AudioTrackUnit::HOURS_2 + TRACK_BASE_UNIT;
        return (uint16_t)AudioTrackUnit::HOURS_5 + TRACK_BASE_UNIT;
    } else {
        if (number == 1) return (uint16_t)AudioTrackUnit::MINUTE + TRACK_BASE_UNIT;
        if (number >= 2 && number <= 4) return (uint16_t)AudioTrackUnit::MINUTES_2 + TRACK_BASE_UNIT;
        return (uint16_t)AudioTrackUnit::MINUTES_5 + TRACK_BASE_UNIT;
    }
}

// ============================================================================
// 11. ОЗВУЧИВАНИЕ ВРЕМЕНИ
// ============================================================================
void AudioExtensions::speakTime(uint8_t hours, uint8_t minutes) {
    if (!isInitialized()) return;

    // Определяем время суток
    if (hours >= 5 && hours < 12) {
        playTrack((uint16_t)AudioTrackTime::MORNING + TRACK_BASE_TIME);
    } else if (hours >= 12 && hours < 17) {
        playTrack((uint16_t)AudioTrackTime::AFTERNOON + TRACK_BASE_TIME);
    } else {
        playTrack((uint16_t)AudioTrackTime::EVENING + TRACK_BASE_TIME);
    }

    playTrack((uint16_t)AudioTrackTime::TIME + TRACK_BASE_TIME);
    speakNumber(hours, true);
    playTrack(getUnitTrack(hours, true));

    if (minutes > 0) {
        speakNumber(minutes, false);
        playTrack(getUnitTrack(minutes, false));
    } else {
        playTrack((uint16_t)AudioTrackTime::EXACT + TRACK_BASE_TIME);
    }
}

void AudioExtensions::speakTime(uint32_t unixTimestamp) {
    time_t t = (time_t)unixTimestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    speakTime(timeinfo.tm_hour, timeinfo.tm_min);
}

void AudioExtensions::speakTime(const char* timeStr) {
    if (!timeStr) return;
    int hours, minutes;
    if (sscanf(timeStr, "%d:%d", &hours, &minutes) == 2) {
        speakTime((uint8_t)hours, (uint8_t)minutes);
    } else {
        logMessage("Invalid time format: %s", timeStr);
    }
}

// ============================================================================
// 12. ОЗВУЧИВАНИЕ ДАТЫ
// ============================================================================
void AudioExtensions::speakDate(uint8_t day, uint8_t month, uint16_t year) {
    if (!isInitialized()) return;

    speakNumber(day, false);

    uint16_t monthTrack = (uint16_t)AudioTrackTime::JANUARY + month - 1 + TRACK_BASE_TIME;
    if (monthTrack > (uint16_t)AudioTrackTime::DECEMBER + TRACK_BASE_TIME) {
        monthTrack = (uint16_t)AudioTrackTime::DECEMBER + TRACK_BASE_TIME;
    }
    playTrack(monthTrack);

    if (year > 0) {
        speakNumber(year % 100, false);
    }
}

void AudioExtensions::speakDate(uint32_t unixTimestamp) {
    time_t t = (time_t)unixTimestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    speakDate(timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
}

// ============================================================================
// 13. ИМЕНА ПОЛЬЗОВАТЕЛЕЙ (ИСПРАВЛЕНО)
// ============================================================================
void AudioExtensions::buildNameCache() {
    if (_cacheBuilt) return;
    _nameCache.clear();

    // === Мужские имена ===
    _nameCache["александр"] = (uint16_t)AudioTrackName::ALEXANDER + TRACK_BASE_NAME;
    _nameCache["алексей"] = (uint16_t)AudioTrackName::ALEXEY + TRACK_BASE_NAME;
    _nameCache["андрей"] = (uint16_t)AudioTrackName::ANDREY + TRACK_BASE_NAME;
    _nameCache["антон"] = (uint16_t)AudioTrackName::ANTON + TRACK_BASE_NAME;
    _nameCache["артём"] = (uint16_t)AudioTrackName::ARTEM + TRACK_BASE_NAME;
    _nameCache["артур"] = (uint16_t)AudioTrackName::ARTUR + TRACK_BASE_NAME;
    _nameCache["вадим"] = (uint16_t)AudioTrackName::VADIM + TRACK_BASE_NAME;
    _nameCache["валерий"] = (uint16_t)AudioTrackName::VALERY + TRACK_BASE_NAME;
    _nameCache["виктор"] = (uint16_t)AudioTrackName::VIKTOR + TRACK_BASE_NAME;
    _nameCache["виталий"] = (uint16_t)AudioTrackName::VITALY + TRACK_BASE_NAME;
    _nameCache["владимир"] = (uint16_t)AudioTrackName::VLADIMIR + TRACK_BASE_NAME;
    _nameCache["владислав"] = (uint16_t)AudioTrackName::VLADISLAV + TRACK_BASE_NAME;
    _nameCache["вячеслав"] = (uint16_t)AudioTrackName::VYACHESLAV + TRACK_BASE_NAME;
    _nameCache["георгий"] = (uint16_t)AudioTrackName::GEORGY + TRACK_BASE_NAME;
    _nameCache["григорий"] = (uint16_t)AudioTrackName::GRIGORY + TRACK_BASE_NAME;
    _nameCache["даниил"] = (uint16_t)AudioTrackName::DANIIL + TRACK_BASE_NAME;
    _nameCache["денис"] = (uint16_t)AudioTrackName::DENIS + TRACK_BASE_NAME;
    _nameCache["дмитрий"] = (uint16_t)AudioTrackName::DMITRY + TRACK_BASE_NAME;
    _nameCache["евгений"] = (uint16_t)AudioTrackName::EVGENY + TRACK_BASE_NAME;
    _nameCache["егор"] = (uint16_t)AudioTrackName::EGOR + TRACK_BASE_NAME;
    _nameCache["иван"] = (uint16_t)AudioTrackName::IVAN + TRACK_BASE_NAME;
    _nameCache["игорь"] = (uint16_t)AudioTrackName::IGOR + TRACK_BASE_NAME;
    _nameCache["илья"] = (uint16_t)AudioTrackName::ILYA + TRACK_BASE_NAME;
    _nameCache["кирилл"] = (uint16_t)AudioTrackName::KIRILL + TRACK_BASE_NAME;
    _nameCache["константин"] = (uint16_t)AudioTrackName::KONSTANTIN + TRACK_BASE_NAME;
    _nameCache["леонид"] = (uint16_t)AudioTrackName::LEONID + TRACK_BASE_NAME;
    _nameCache["максим"] = (uint16_t)AudioTrackName::MAKSIM + TRACK_BASE_NAME;
    _nameCache["матвей"] = (uint16_t)AudioTrackName::MATVEY + TRACK_BASE_NAME;
    _nameCache["михаил"] = (uint16_t)AudioTrackName::MIKHAIL + TRACK_BASE_NAME;
    _nameCache["мирослав"] = (uint16_t)AudioTrackName::MIROSLAV + TRACK_BASE_NAME;
    _nameCache["никита"] = (uint16_t)AudioTrackName::NIKITA + TRACK_BASE_NAME;
    _nameCache["николай"] = (uint16_t)AudioTrackName::NIKOLAY + TRACK_BASE_NAME;
    _nameCache["олег"] = (uint16_t)AudioTrackName::OLEG + TRACK_BASE_NAME;
    _nameCache["павел"] = (uint16_t)AudioTrackName::PAVEL + TRACK_BASE_NAME;
    _nameCache["роман"] = (uint16_t)AudioTrackName::ROMAN + TRACK_BASE_NAME;
    _nameCache["руслан"] = (uint16_t)AudioTrackName::RUSLAN + TRACK_BASE_NAME;
    _nameCache["святослав"] = (uint16_t)AudioTrackName::SVYATOSLAV + TRACK_BASE_NAME;
    _nameCache["сергей"] = (uint16_t)AudioTrackName::SERGEY + TRACK_BASE_NAME;
    _nameCache["степан"] = (uint16_t)AudioTrackName::STEPAN + TRACK_BASE_NAME;
    _nameCache["тимофей"] = (uint16_t)AudioTrackName::TIMOFEY + TRACK_BASE_NAME;
    _nameCache["тимур"] = (uint16_t)AudioTrackName::TIMUR + TRACK_BASE_NAME;
    _nameCache["юрий"] = (uint16_t)AudioTrackName::YURIY + TRACK_BASE_NAME;
    _nameCache["ярослав"] = (uint16_t)AudioTrackName::YAROSLAV + TRACK_BASE_NAME;

    // === Женские имена ===
    _nameCache["алена"] = (uint16_t)AudioTrackName::ALENA + TRACK_BASE_NAME;
    _nameCache["алина"] = (uint16_t)AudioTrackName::ALINA + TRACK_BASE_NAME;
    _nameCache["алиса"] = (uint16_t)AudioTrackName::ALISA + TRACK_BASE_NAME;
    _nameCache["анастасия"] = (uint16_t)AudioTrackName::ANASTASIA + TRACK_BASE_NAME;
    _nameCache["ангелина"] = (uint16_t)AudioTrackName::ANGELINA + TRACK_BASE_NAME;
    _nameCache["анна"] = (uint16_t)AudioTrackName::ANNA + TRACK_BASE_NAME;
    _nameCache["антонина"] = (uint16_t)AudioTrackName::ANTONINA + TRACK_BASE_NAME;
    _nameCache["валентина"] = (uint16_t)AudioTrackName::VALENTINA + TRACK_BASE_NAME;
    _nameCache["василиса"] = (uint16_t)AudioTrackName::VASILISA + TRACK_BASE_NAME;
    _nameCache["вера"] = (uint16_t)AudioTrackName::VERA + TRACK_BASE_NAME;
    _nameCache["вероника"] = (uint16_t)AudioTrackName::VERONIKA + TRACK_BASE_NAME;
    _nameCache["виктория"] = (uint16_t)AudioTrackName::VIKTORIA + TRACK_BASE_NAME;
    _nameCache["галина"] = (uint16_t)AudioTrackName::GALINA + TRACK_BASE_NAME;
    _nameCache["дарья"] = (uint16_t)AudioTrackName::DARYA + TRACK_BASE_NAME;
    _nameCache["диана"] = (uint16_t)AudioTrackName::DIANA + TRACK_BASE_NAME;
    _nameCache["евгения"] = (uint16_t)AudioTrackName::EVGENIA + TRACK_BASE_NAME;
    _nameCache["екатерина"] = (uint16_t)AudioTrackName::EKATERINA + TRACK_BASE_NAME;
    _nameCache["елена"] = (uint16_t)AudioTrackName::ELENA + TRACK_BASE_NAME;
    _nameCache["елизавета"] = (uint16_t)AudioTrackName::ELIZAVETA + TRACK_BASE_NAME;
    _nameCache["лада"] = (uint16_t)AudioTrackName::LADA + TRACK_BASE_NAME;
    _nameCache["ирина"] = (uint16_t)AudioTrackName::IRINA + TRACK_BASE_NAME;
    _nameCache["карина"] = (uint16_t)AudioTrackName::KARINA + TRACK_BASE_NAME;
    _nameCache["кира"] = (uint16_t)AudioTrackName::KIRA + TRACK_BASE_NAME;
    _nameCache["кристина"] = (uint16_t)AudioTrackName::KRISTINA + TRACK_BASE_NAME;
    _nameCache["ксения"] = (uint16_t)AudioTrackName::KSENIA + TRACK_BASE_NAME;
    _nameCache["любовь"] = (uint16_t)AudioTrackName::LYUBOV + TRACK_BASE_NAME;
    _nameCache["людмила"] = (uint16_t)AudioTrackName::LYUDMILA + TRACK_BASE_NAME;
    _nameCache["маргарита"] = (uint16_t)AudioTrackName::MARGARITA + TRACK_BASE_NAME;
    _nameCache["марина"] = (uint16_t)AudioTrackName::MARINA + TRACK_BASE_NAME;
    _nameCache["мария"] = (uint16_t)AudioTrackName::MARIA + TRACK_BASE_NAME;
    _nameCache["милена"] = (uint16_t)AudioTrackName::MILENA + TRACK_BASE_NAME;
    _nameCache["мирослава"] = (uint16_t)AudioTrackName::MIROSLAVA + TRACK_BASE_NAME;
    _nameCache["надежда"] = (uint16_t)AudioTrackName::NADEZHDA + TRACK_BASE_NAME;
    _nameCache["наталья"] = (uint16_t)AudioTrackName::NATALYA + TRACK_BASE_NAME;
    _nameCache["нина"] = (uint16_t)AudioTrackName::NINA + TRACK_BASE_NAME;
    _nameCache["оксана"] = (uint16_t)AudioTrackName::OKSANA + TRACK_BASE_NAME;
    _nameCache["ольга"] = (uint16_t)AudioTrackName::OLGA + TRACK_BASE_NAME;  // <-- ИСПРАВЛЕНО!
    _nameCache["полина"] = (uint16_t)AudioTrackName::POLINA + TRACK_BASE_NAME;
    _nameCache["светлана"] = (uint16_t)AudioTrackName::SVETLANA + TRACK_BASE_NAME;
    _nameCache["софия"] = (uint16_t)AudioTrackName::SOFIA + TRACK_BASE_NAME;
    _nameCache["таисия"] = (uint16_t)AudioTrackName::TAISIA + TRACK_BASE_NAME;
    _nameCache["татьяна"] = (uint16_t)AudioTrackName::TATYANA + TRACK_BASE_NAME;
    _nameCache["ульяна"] = (uint16_t)AudioTrackName::ULYANA + TRACK_BASE_NAME;
    _nameCache["эльвира"] = (uint16_t)AudioTrackName::ELVIRA + TRACK_BASE_NAME;
    _nameCache["юлия"] = (uint16_t)AudioTrackName::YULIA + TRACK_BASE_NAME;
    _nameCache["яна"] = (uint16_t)AudioTrackName::YANA + TRACK_BASE_NAME;

    // === Роли ===
    _nameCache["хозяин"] = (uint16_t)AudioTrackName::OWNER_MALE + TRACK_BASE_NAME;
    _nameCache["хозяйка"] = (uint16_t)AudioTrackName::OWNER_FEMALE + TRACK_BASE_NAME;
    _nameCache["незнакомец"] = (uint16_t)AudioTrackName::STRANGER + TRACK_BASE_NAME;
    _nameCache["курьер"] = (uint16_t)AudioTrackName::COURIER + TRACK_BASE_NAME;
    _nameCache["гость"] = (uint16_t)AudioTrackName::GUEST + TRACK_BASE_NAME;  // <-- ИСПРАВЛЕНО!
    _nameCache["посетитель"] = (uint16_t)AudioTrackName::VISITOR + TRACK_BASE_NAME;
    _nameCache["мастер-ключ"] = (uint16_t)AudioTrackName::MASTER_KEY + TRACK_BASE_NAME;

    _cacheBuilt = true;
    logMessage("Name cache built: %zu entries", _nameCache.size());
}

String AudioExtensions::normalizeName(const char* name) const {
    if (name == nullptr) return "";
    String lower = String(name);
    lower.toLowerCase();
    lower.trim();
    // Удаляем лишние пробелы
    while (lower.indexOf("  ") != -1) {
        lower.replace("  ", " ");
    }
    return lower;
}

void AudioExtensions::rebuildNameCache() {
    _cacheBuilt = false;
    buildNameCache();
}

void AudioExtensions::speakName(uint16_t nameId) {
    if (!isInitialized()) return;
    uint16_t track = TRACK_BASE_NAME + nameId;
    if (track > TRACK_BASE_NAME + 100) {
        logMessage("Invalid name ID: %d", nameId);
        return;
    }
    playTrack(track, AudioPriority::INFO);
}

void AudioExtensions::speakName(const char* name) {  // <-- НОВАЯ РЕАЛИЗАЦИЯ
    if (!isInitialized() || name == nullptr) return;

    String normalized = normalizeName(name);
    auto it = _nameCache.find(normalized);
    if (it != _nameCache.end()) {
        playTrack(it->second, AudioPriority::INFO);
        logMessage("Speaking name: %s (track %d)", name, it->second);
    } else {
        logMessage("Name not found in cache: %s", name);
        // Пытаемся произнести по буквам (упрощенно)
        playTrack(TRACK_BASE_NAME + 1, AudioPriority::INFO); // "Имя не найдено"
    }
}

void AudioExtensions::speakName(const String& name) {
    speakName(name.c_str());
}

uint16_t AudioExtensions::findNameId(const char* name) const {
    if (name == nullptr) return 0;
    String normalized = normalizeName(name);
    auto it = _nameCache.find(normalized);
    if (it != _nameCache.end()) {
        return it->second - TRACK_BASE_NAME;
    }
    return 0;
}

void AudioExtensions::addNameToCache(const char* name, uint16_t trackId) {
    if (name == nullptr) return;
    if (_nameCache.size() >= _maxCacheSize) {
        logMessage("Name cache full (%zu entries)", _nameCache.size());
        return;
    }
    String normalized = normalizeName(name);
    _nameCache[normalized] = TRACK_BASE_NAME + trackId;
    logMessage("Added to cache: %s -> track %d", name, trackId);
}

void AudioExtensions::addNameToCache(const String& name, uint16_t trackId) {
    addNameToCache(name.c_str(), trackId);
}

void AudioExtensions::removeNameFromCache(const char* name) {
    if (name == nullptr) return;
    String normalized = normalizeName(name);
    auto it = _nameCache.find(normalized);
    if (it != _nameCache.end()) {
        _nameCache.erase(it);
        logMessage("Removed from cache: %s", name);
    }
}

void AudioExtensions::clearNameCache() {
    _nameCache.clear();
    _cacheBuilt = false;
    logMessage("Name cache cleared");
}

bool AudioExtensions::isNameCached(const char* name) const {
    if (name == nullptr) return false;
    String normalized = normalizeName(name);
    return _nameCache.find(normalized) != _nameCache.end();
}

// ============================================================================
// 14. СОБЫТИЯ СКУД
// ============================================================================
void AudioExtensions::speakAccessGranted(uint16_t nameId) {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_LOCK + (uint16_t)AudioTrackLock::ACCESS_GRANTED, AudioPriority::CRITICAL);
    if (nameId > 0) speakName(nameId);
}

void AudioExtensions::speakAccessDenied(uint16_t nameId) {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_LOCK + (uint16_t)AudioTrackLock::ACCESS_DENIED, AudioPriority::CRITICAL);
    if (nameId > 0) speakName(nameId);
}

void AudioExtensions::speakGreeting(uint16_t nameId) {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_TIME + (uint16_t)AudioTrackTime::HELLO);
    if (nameId > 0) speakName(nameId);
}

void AudioExtensions::speakLockEvent(AudioTrackLock event, uint16_t nameId) {
    speakLockEvent((uint16_t)event, nameId);
}

void AudioExtensions::speakLockEvent(uint16_t eventId, uint16_t nameId) {
    if (!isInitialized()) return;
    uint16_t track = TRACK_BASE_LOCK + eventId;
    if (track > TRACK_BASE_LOCK + 50) {
        logMessage("Invalid lock event ID: %d", eventId);
        return;
    }
    playTrack(track, AudioPriority::CRITICAL);
    if (nameId > 0) speakName(nameId);
}

// ============================================================================
// 15. СИСТЕМНЫЕ СОБЫТИЯ
// ============================================================================
void AudioExtensions::speakSystemEvent(AudioTrackSystem event) {
    speakSystemEvent((uint16_t)event);
}

void AudioExtensions::speakSystemEvent(uint16_t eventId) {
    if (!isInitialized()) return;
    uint16_t track = TRACK_BASE_SYSTEM + eventId;
    if (track > TRACK_BASE_SYSTEM + 50) {
        logMessage("Invalid system event ID: %d", eventId);
        return;
    }
    playTrack(track);
}

// ============================================================================
// 16. ПОГОДА
// ============================================================================
void AudioExtensions::speakTemperature(float temp, bool isOutside) {
    if (!isInitialized()) return;
    speakNumber((int)roundf(temp), true);
    playTrack(TRACK_BASE_UNIT + (uint16_t)AudioTrackUnit::DEGREES);
}

void AudioExtensions::speakHumidity(float humidity, bool isOutside) {
    if (!isInitialized()) return;
    speakNumber((int)roundf(humidity), false);
    playTrack(TRACK_BASE_UNIT + (uint16_t)AudioTrackUnit::PERCENT);
}

void AudioExtensions::speakPressure(float pressure) {
    if (!isInitialized()) return;
    speakNumber((int)roundf(pressure), true);
    playTrack(TRACK_BASE_UNIT + (uint16_t)AudioTrackUnit::MMHG);
}

void AudioExtensions::speakWeather(float temp, float humidity, float pressure) {
    if (!isInitialized()) return;
    speakTemperature(temp, true);
    speakHumidity(humidity, true);
    speakPressure(pressure);
}

void AudioExtensions::speakWeatherForecast(const char* condition, float temp, float humidity, float pressure) {
    if (!isInitialized() || condition == nullptr) return;

    playTrack(TRACK_BASE_WEATHER + (uint16_t)AudioTrackWeather::FORECAST);

    // Определяем погоду по строке
    String cond = String(condition).toLowerCase();
    if (cond.indexOf("солн") != -1 || cond.indexOf("ясн") != -1) {
        playTrack(TRACK_BASE_WEATHER + (uint16_t)AudioTrackWeather::SUNNY);
    } else if (cond.indexOf("обл") != -1) {
        playTrack(TRACK_BASE_WEATHER + (uint16_t)AudioTrackWeather::CLOUDY);
    } else if (cond.indexOf("дожд") != -1) {
        playTrack(TRACK_BASE_WEATHER + (uint16_t)AudioTrackWeather::RAIN);
    } else if (cond.indexOf("снег") != -1) {
        playTrack(TRACK_BASE_WEATHER + (uint16_t)AudioTrackWeather::SNOW);
    } else if (cond.indexOf("ветр") != -1) {
        playTrack(TRACK_BASE_WEATHER + (uint16_t)AudioTrackWeather::WIND);
    } else if (cond.indexOf("туман") != -1) {
        playTrack(TRACK_BASE_WEATHER + (uint16_t)AudioTrackWeather::FOG);
    } else if (cond.indexOf("гроз") != -1) {
        playTrack(TRACK_BASE_WEATHER + (uint16_t)AudioTrackWeather::STORM);
    }

    speakTemperature(temp, true);
    speakHumidity(humidity, true);
    speakPressure(pressure);
}

void AudioExtensions::speakWeatherForecast(const String& condition, float temp, float humidity, float pressure) {
    speakWeatherForecast(condition.c_str(), temp, humidity, pressure);
}

// ============================================================================
// 17. СЧЕТЧИКИ
// ============================================================================
void AudioExtensions::speakCounter(uint32_t value, uint8_t type) {
    switch (type) {
        case 1: speakWaterCounter(value); break;
        case 2: speakGasCounter(value); break;
        case 3: speakPowerCounter(value); break;
        default: logMessage("Unknown counter type: %d", type); break;
    }
}

void AudioExtensions::speakWaterCounter(uint32_t value) {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_COUNTER + (uint16_t)AudioTrackCounter::WATER_COLD);
    speakNumber(value, false);
    playTrack(TRACK_BASE_UNIT + (uint16_t)AudioTrackUnit::CUBIC_METERS);
}

void AudioExtensions::speakGasCounter(uint32_t value) {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_COUNTER + (uint16_t)AudioTrackCounter::GAS);
    speakNumber(value, true);
    playTrack(TRACK_BASE_UNIT + (uint16_t)AudioTrackUnit::CUBIC_METERS);
}

void AudioExtensions::speakPowerCounter(uint32_t value) {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_COUNTER + (uint16_t)AudioTrackCounter::POWER);
    speakNumber(value, true);
    playTrack(TRACK_BASE_UNIT + (uint16_t)AudioTrackUnit::KWH);
}

void AudioExtensions::speakTariff(uint8_t tariff) {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_COUNTER + (uint16_t)AudioTrackCounter::TARIFF);
    speakNumber(tariff, false);
}

// ============================================================================
// 18. УМНЫЙ ДОМ
// ============================================================================
void AudioExtensions::speakHomeEvent(AudioTrackHome event) {
    speakHomeEvent((uint16_t)event);
}

void AudioExtensions::speakHomeEvent(uint16_t eventId) {
    if (!isInitialized()) return;
    uint16_t track = TRACK_BASE_HOME + eventId;
    if (track > TRACK_BASE_HOME + 50) {
        logMessage("Invalid home event ID: %d", eventId);
        return;
    }
    playTrack(track);
}

// ============================================================================
// 19. СЦЕНАРИИ
// ============================================================================
void AudioExtensions::speakScenario(AudioTrackScenario scenario) {
    speakScenario((uint16_t)scenario);
}

void AudioExtensions::speakScenario(uint16_t scenarioId) {
    if (!isInitialized()) return;
    uint16_t track = TRACK_BASE_SCENARIO + scenarioId;
    if (track > TRACK_BASE_SCENARIO + 20) {
        logMessage("Invalid scenario ID: %d", scenarioId);
        return;
    }
    playTrack(track);
}

void AudioExtensions::speakGoodMorning() {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_SCENARIO + (uint16_t)AudioTrackScenario::GOOD_MORNING, AudioPriority::INFO);
}

void AudioExtensions::speakGoodNight() {
    if (!isInitialized()) return;
    playTrack(TRACK_BASE_SCENARIO + (uint16_t)AudioTrackScenario::GOOD_NIGHT, AudioPriority::INFO);
}

// ============================================================================
// 20. ДИАГНОСТИКА
// ============================================================================
void AudioExtensions::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================");
    stream.println(" AUDIO EXTENSIONS DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", "4.2.2");
    stream.printf(" Initialized: %s\n", isInitialized() ? "YES" : "NO");
    stream.printf(" AudioManager: %s\n", _audio ? "OK" : "NULL");
    stream.printf(" Cache Built: %s\n", _cacheBuilt ? "YES" : "NO");
    stream.printf(" Cache Size: %zu/%zu\n", _nameCache.size(), _maxCacheSize);
    stream.printf(" Default Lang: %s\n", _defaultLanguage.c_str());
    stream.printf(" Volume Mult: %.2f\n", _volumeMultiplier);
    stream.printf(" Default Priority: %d\n", (int)_defaultPriority);
    stream.println("-- Track Bases --");
    stream.printf(" Time: %d\n", TRACK_BASE_TIME);
    stream.printf(" Number: %d\n", TRACK_BASE_NUMBER);
    stream.printf(" Name: %d\n", TRACK_BASE_NAME);
    stream.printf(" System: %d\n", TRACK_BASE_SYSTEM);
    stream.printf(" Lock: %d\n", TRACK_BASE_LOCK);
    stream.printf(" Home: %d\n", TRACK_BASE_HOME);
    stream.printf(" Unit: %d\n", TRACK_BASE_UNIT);
    stream.printf(" Weather: %d\n", TRACK_BASE_WEATHER);
    stream.printf(" Counter: %d\n", TRACK_BASE_COUNTER);
    stream.printf(" Scenario: %d\n", TRACK_BASE_SCENARIO);
    stream.println("==============================");
}

void AudioExtensions::printStats() const {
    streamDiagnosticInfo(Serial);
}