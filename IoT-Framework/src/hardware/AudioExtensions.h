// ============================================================================
// AudioExtensions.h - ULTIMATE MICRO-OS V4.2.2 (EVENT-DRIVEN)
// ============================================================================
// Описание: Расширенные аудио-функции для озвучивания:
// - Чисел (0-999999)
// - Времени (часы, минуты)
// - Даты (день, месяц, год)
// - Имен пользователей (кэширование)
// - Событий СКУД, Умного дома, погоды, счетчиков
// - Системных звуков
// - Сценариев
//
// ВАЖНО: Модуль является расширением AudioManager и НЕ является
// самостоятельным модулем МикроОС. Он использует AudioManager для
// воспроизведения и не регистрируется в AppCore как отдельный модуль.
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
// - ДОБАВЛЕНЫ расширенные комментарии для музыкальных треков
// ============================================================================
#pragma once

#include "AudioManager.h"
#include <map>
#include <vector>
#include <functional>
#include <cstdarg>
#include <cstring>
#include <cmath>

// Только IModule, без AppCore!
#include "core/IModule.h"

// ============================================================================
// 1. БАЗОВЫЕ АДРЕСА ТРЕКОВ НА SD-КАРТЕ
// ============================================================================
/**
 * @brief Базовая адресация треков на SD-карте DFPlayer
 *
 * Структура папок на SD-карте:
 *
 * 📁 01 - Время и календарь (001-033)
 * 📁 02 - Числительные (034-084)
 * 📁 03 - Имена пользователей (085-175)
 * 📁 04 - Системные события (176-206)
 * 📁 05 - События СКУД (207-235)
 * 📁 06 - Умный дом (236-257)
 * 📁 07 - Единицы измерения (258-270)
 * 📁 08 - Погода (271-282)
 * 📁 09 - Счетчики (283-295)
 * 📁 10 - Сценарии (296-305)
 */
static constexpr uint16_t TRACK_BASE_TIME = 1;       // 001-033
static constexpr uint16_t TRACK_BASE_NUMBER = 34;    // 034-084
static constexpr uint16_t TRACK_BASE_NAME = 85;      // 085-175
static constexpr uint16_t TRACK_BASE_SYSTEM = 176;   // 176-206
static constexpr uint16_t TRACK_BASE_LOCK = 207;     // 207-235
static constexpr uint16_t TRACK_BASE_HOME = 236;     // 236-257
static constexpr uint16_t TRACK_BASE_UNIT = 258;     // 258-270
static constexpr uint16_t TRACK_BASE_WEATHER = 271;  // 271-282
static constexpr uint16_t TRACK_BASE_COUNTER = 283;  // 283-295
static constexpr uint16_t TRACK_BASE_SCENARIO = 296; // 296-305

// ============================================================================
// 2. ПАПКА 01: ВРЕМЯ И КАЛЕНДАРЬ (001-033)
// ============================================================================
/**
 * @brief Треки для озвучивания времени и календаря
 *
 * Аудиофайлы должны быть в папке "01" на SD-карте:
 * 001.mp3 - Доброе утро (утреннее приветствие)
 * 002.mp3 - Добрый день (дневное приветствие)
 * 003.mp3 - Добрый вечер (вечернее приветствие)
 * 004.mp3 - Здравствуйте (приветствие ночью)
 * 005.mp3 - Время
 * 006.mp3 - Будильник установлен
 * 007.mp3 - Будильник через 5 минут
 * 008.mp3 - Будильник через 10 минут
 * 009.mp3 - Будильник через 15 минут
 * 010.mp3 - Будильник выключен
 * 011.mp3 - Таймер установлен
 * 012.mp3 - Таймер завершен
 * 013.mp3 - Тревога
 * 014.mp3 - Ровно (для точного времени)
 * 015-021.mp3 - Дни недели (Понедельник - Воскресенье)
 * 022-033.mp3 - Месяцы (Январь - Декабрь)
 */
enum class AudioTrackTime : uint16_t {
    MORNING = 1,        // 001 - Доброе утро
    AFTERNOON = 2,      // 002 - Добрый день
    EVENING = 3,        // 003 - Добрый вечер
    HELLO = 4,          // 004 - Здравствуйте
    TIME = 5,           // 005 - Время
    ALARM_SET = 6,      // 006 - Будильник установлен
    ALARM_SNOOZE_5 = 7, // 007 - Будильник через 5 минут
    ALARM_SNOOZE_10 = 8,// 008 - Будильник через 10 минут
    ALARM_SNOOZE_15 = 9,// 009 - Будильник через 15 минут
    ALARM_OFF = 10,     // 010 - Будильник выключен
    TIMER_SET = 11,     // 011 - Таймер установлен
    TIMER_END = 12,     // 012 - Таймер завершен
    ALARM_TRIGGER = 13, // 013 - Тревога
    EXACT = 14,         // 014 - Ровно
    MONDAY = 15,        // 015 - Понедельник
    TUESDAY = 16,       // 016 - Вторник
    WEDNESDAY = 17,     // 017 - Среда
    THURSDAY = 18,      // 018 - Четверг
    FRIDAY = 19,        // 019 - Пятница
    SATURDAY = 20,      // 020 - Суббота
    SUNDAY = 21,        // 021 - Воскресенье
    JANUARY = 22,       // 022 - Январь
    FEBRUARY = 23,      // 023 - Февраль
    MARCH = 24,         // 024 - Март
    APRIL = 25,         // 025 - Апрель
    MAY = 26,           // 026 - Май
    JUNE = 27,          // 027 - Июнь
    JULY = 28,          // 028 - Июль
    AUGUST = 29,        // 029 - Август
    SEPTEMBER = 30,     // 030 - Сентябрь
    OCTOBER = 31,       // 031 - Октябрь
    NOVEMBER = 32,      // 032 - Ноябрь
    DECEMBER = 33       // 033 - Декабрь
};

// ============================================================================
// 3. ПАПКА 02: ЧИСЛИТЕЛЬНЫЕ (034-084)
// ============================================================================
/**
 * @brief Треки для озвучивания чисел
 *
 * 034.mp3 - Ноль
 * 035.mp3 - Один (мужской)
 * 036.mp3 - Одна (женский)
 * 037.mp3 - Два (мужской)
 * 038.mp3 - Две (женский)
 * 039.mp3 - Три
 * 040.mp3 - Четыре
 * 041.mp3 - Пять
 * 042.mp3 - Шесть
 * 043.mp3 - Семь
 * 044.mp3 - Восемь
 * 045.mp3 - Девять
 * 046.mp3 - Десять
 * 047.mp3 - Одиннадцать
 * 048.mp3 - Двенадцать
 * 049.mp3 - Тринадцать
 * 050.mp3 - Четырнадцать
 * 051.mp3 - Пятнадцать
 * 052.mp3 - Шестнадцать
 * 053.mp3 - Семнадцать
 * 054.mp3 - Восемнадцать
 * 055.mp3 - Девятнадцать
 * 056.mp3 - Двадцать
 * 057.mp3 - Тридцать
 * 058.mp3 - Сорок
 * 059.mp3 - Пятьдесят
 * 060.mp3 - Шестьдесят
 * 061.mp3 - Семьдесят
 * 062.mp3 - Восемьдесят
 * 063.mp3 - Девяносто
 * 064.mp3 - Сто
 * 065.mp3 - Двести
 * 066.mp3 - Триста
 * 067.mp3 - Четыреста
 * 068.mp3 - Пятьсот
 * 069.mp3 - Шестьсот
 * 070.mp3 - Семьсот
 * 071.mp3 - Восемьсот
 * 072.mp3 - Девятьсот
 * 073.mp3 - Тысяча
 * 074.mp3 - Минус
 * 075.mp3 - Целая
 * 076.mp3 - Десятых
 * 077.mp3 - Сотых
 * 078.mp3 - Тысячных
 */
enum class AudioTrackNumber : uint16_t {
    ZERO = 1,           // 034 - Ноль
    ONE_MALE = 2,       // 035 - Один (мужской)
    ONE_FEMALE = 3,     // 036 - Одна (женский)
    TWO_MALE = 4,       // 037 - Два (мужской)
    TWO_FEMALE = 5,     // 038 - Две (женский)
    THREE = 6,          // 039 - Три
    FOUR = 7,           // 040 - Четыре
    FIVE = 8,           // 041 - Пять
    SIX = 9,            // 042 - Шесть
    SEVEN = 10,         // 043 - Семь
    EIGHT = 11,         // 044 - Восемь
    NINE = 12,          // 045 - Девять
    TEN = 13,           // 046 - Десять
    ELEVEN = 14,        // 047 - Одиннадцать
    TWELVE = 15,        // 048 - Двенадцать
    THIRTEEN = 16,      // 049 - Тринадцать
    FOURTEEN = 17,      // 050 - Четырнадцать
    FIFTEEN = 18,       // 051 - Пятнадцать
    SIXTEEN = 19,       // 052 - Шестнадцать
    SEVENTEEN = 20,     // 053 - Семнадцать
    EIGHTEEN = 21,      // 054 - Восемнадцать
    NINETEEN = 22,      // 055 - Девятнадцать
    TWENTY = 23,        // 056 - Двадцать
    THIRTY = 24,        // 057 - Тридцать
    FORTY = 25,         // 058 - Сорок
    FIFTY = 26,         // 059 - Пятьдесят
    SIXTY = 27,         // 060 - Шестьдесят
    SEVENTY = 28,       // 061 - Семьдесят
    EIGHTY = 29,        // 062 - Восемьдесят
    NINETY = 30,        // 063 - Девяносто
    HUNDRED = 31,       // 064 - Сто
    TWO_HUNDRED = 32,   // 065 - Двести
    THREE_HUNDRED = 33, // 066 - Триста
    FOUR_HUNDRED = 34,  // 067 - Четыреста
    FIVE_HUNDRED = 35,  // 068 - Пятьсот
    SIX_HUNDRED = 36,   // 069 - Шестьсот
    SEVEN_HUNDRED = 37, // 070 - Семьсот
    EIGHT_HUNDRED = 38, // 071 - Восемьсот
    NINE_HUNDRED = 39,  // 072 - Девятьсот
    THOUSAND = 40,      // 073 - Тысяча
    MINUS = 41,         // 074 - Минус
    WHOLE = 42,         // 075 - Целая
    TENTHS = 43,        // 076 - Десятых
    HUNDREDTHS = 44,    // 077 - Сотых
    THOUSANDTHS = 45    // 078 - Тысячных
};

// ============================================================================
// 4. ПАПКА 03: ИМЕНА ПОЛЬЗОВАТЕЛЕЙ (085-175)
// ============================================================================
/**
 * @brief Треки для озвучивания имен (85-175)
 *
 * Мужские имена: 85-128
 * Женские имена: 129-172
 * Роли: 173-175
 */
enum class AudioTrackName : uint16_t {
    // === Мужские имена (85-128) ===
    ALEXANDER = 1,      // 085 - Александр
    ALEXEY = 2,         // 086 - Алексей
    ANDREY = 3,         // 087 - Андрей
    ANTON = 4,          // 088 - Антон
    ARTEM = 5,          // 089 - Артём
    ARTUR = 6,          // 090 - Артур
    VADIM = 7,          // 091 - Вадим
    VALERY = 8,         // 092 - Валерий
    VIKTOR = 9,         // 093 - Виктор
    VITALY = 10,        // 094 - Виталий
    VLADIMIR = 11,      // 095 - Владимир
    VLADISLAV = 12,     // 096 - Владислав
    VYACHESLAV = 13,    // 097 - Вячеслав
    GEORGY = 14,        // 098 - Георгий
    GRIGORY = 15,       // 099 - Григорий
    DANIIL = 16,        // 100 - Даниил
    DENIS = 17,         // 101 - Денис
    DMITRY = 18,        // 102 - Дмитрий
    EVGENY = 19,        // 103 - Евгений
    EGOR = 20,          // 104 - Егор
    IVAN = 21,          // 105 - Иван
    IGOR = 22,          // 106 - Игорь
    ILYA = 23,          // 107 - Илья
    KIRILL = 24,        // 108 - Кирилл
    KONSTANTIN = 25,    // 109 - Константин
    LEONID = 26,        // 110 - Леонид
    MAKSIM = 27,        // 111 - Максим
    MATVEY = 28,        // 112 - Матвей
    MIKHAIL = 29,       // 113 - Михаил
    MIROSLAV = 30,      // 114 - Мирослав
    NIKITA = 31,        // 115 - Никита
    NIKOLAY = 32,       // 116 - Николай
    OLEG = 33,          // 117 - Олег
    PAVEL = 34,         // 118 - Павел
    ROMAN = 35,         // 119 - Роман
    RUSLAN = 36,        // 120 - Руслан
    SVYATOSLAV = 37,    // 121 - Святослав
    SERGEY = 38,        // 122 - Сергей
    STEPAN = 39,        // 123 - Степан
    TIMOFEY = 40,       // 124 - Тимофей
    TIMUR = 41,         // 125 - Тимур
    YURIY = 42,         // 126 - Юрий
    YAROSLAV = 43,      // 127 - Ярослав
    // Резерв 128

    // === Женские имена (129-172) ===
    ALENA = 44,         // 129 - Алена
    ALINA = 45,         // 130 - Алина
    ALISA = 46,         // 131 - Алиса
    ANASTASIA = 47,     // 132 - Анастасия
    ANGELINA = 48,      // 133 - Ангелина
    ANNA = 49,          // 134 - Анна
    ANTONINA = 50,      // 135 - Антонина
    VALENTINA = 51,     // 136 - Валентина
    VASILISA = 52,      // 137 - Василиса
    VERA = 53,          // 138 - Вера
    VERONIKA = 54,      // 139 - Вероника
    VIKTORIA = 55,      // 140 - Виктория
    GALINA = 56,        // 141 - Галина
    DARYA = 57,         // 142 - Дарья
    DIANA = 58,         // 143 - Диана
    EVGENIA = 59,       // 144 - Евгения
    EKATERINA = 60,     // 145 - Екатерина
    ELENA = 61,         // 146 - Елена
    ELIZAVETA = 62,     // 147 - Елизавета
    LADA = 63,          // 148 - Лада
    IRINA = 64,         // 149 - Ирина
    KARINA = 65,        // 150 - Карина
    KIRA = 66,          // 151 - Кира
    KRISTINA = 67,      // 152 - Кристина
    KSENIA = 68,        // 153 - Ксения
    LYUBOV = 69,        // 154 - Любовь
    LYUDMILA = 70,      // 155 - Людмила
    MARGARITA = 71,     // 156 - Маргарита
    MARINA = 72,        // 157 - Марина
    MARIA = 73,         // 158 - Мария
    MILENA = 74,        // 159 - Милена
    MIROSLAVA = 75,     // 160 - Мирослава
    NADEZHDA = 76,      // 161 - Надежда
    NATALYA = 77,       // 162 - Наталья
    NINA = 78,          // 163 - Нина
    OKSANA = 79,        // 164 - Оксана
    OLGA = 80,          // 165 - Ольга
    POLINA = 81,        // 166 - Полина
    SVETLANA = 82,      // 167 - Светлана
    SOFIA = 83,         // 168 - София
    TAISIA = 84,        // 169 - Таисия
    TATYANA = 85,       // 170 - Татьяна
    ULYANA = 86,        // 171 - Ульяна
    ELVIRA = 87,        // 172 - Эльвира
    YULIA = 88,         // 173 - Юлия
    YANA = 89,          // 174 - Яна
    // Резерв 175

    // === Роли (173-175) ===
    OWNER_MALE = 90,    // 176 - Хозяин
    OWNER_FEMALE = 91,  // 177 - Хозяйка
    STRANGER = 92,      // 178 - Незнакомец
    COURIER = 93,       // 179 - Курьер
    GUEST = 94,         // 180 - Гость
    VISITOR = 95,       // 181 - Посетитель
    MASTER_KEY = 96     // 182 - Мастер-ключ
};

// ============================================================================
// 5. ПАПКА 04: СИСТЕМНЫЕ СОБЫТИЯ (176-206)
// ============================================================================
enum class AudioTrackSystem : uint16_t {
    SYS_START = 1,          // 177 - Система запущена
    SYS_READY = 2,          // 178 - Система готова
    REBOOT = 3,             // 179 - Перезагрузка
    RESET = 4,              // 180 - Сброс
    UPDATE = 5,             // 181 - Обновление
    SAVED = 6,              // 182 - Сохранено
    ERROR = 7,              // 183 - Ошибка
    WARNING = 8,            // 184 - Внимание
    ATTENTION = 9,          // 185 - Внимание
    OPERATION_SUCCESS = 10, // 186 - Операция выполнена успешно
    OPERATION_FAILED = 11,  // 187 - Операция не выполнена
    NET_CHECK = 12,         // 188 - Проверка сети
    NET_CONNECTED = 13,     // 189 - Сеть подключена
    NET_CONNECTING = 14,    // 190 - Подключение к сети
    NET_ERROR = 15,         // 191 - Ошибка сети
    NET_OK = 16,            // 192 - Сеть в порядке
    NET_LOST = 17,          // 193 - Сеть потеряна
    MQTT_OK = 18,           // 194 - MQTT подключен
    MODE_LOCAL = 19,        // 195 - Локальный режим
    MODE_NETWORK = 20,      // 196 - Сетевой режим
    OTA_START = 21,         // 197 - Начало обновления
    OTA_SUCCESS = 22,       // 198 - Обновление успешно
    OTA_FAILED = 23,        // 199 - Обновление не удалось
    POWER_LOST = 24,        // 200 - Питание потеряно
    POWER_RESTORED = 25,    // 201 - Питание восстановлено
    BATTERY_LOW = 26,       // 202 - Низкий заряд батареи
    VOICE_ON = 27,          // 203 - Голос включен
    VOICE_OFF = 28,         // 204 - Голос выключен
    CONNECTED = 29,         // 205 - Подключено
    DISCONNECTED = 30,      // 206 - Отключено
    WAITING = 31            // 207 - Ожидание
};

// ============================================================================
// 6. ПАПКА 05: СОБЫТИЯ СКУД (207-235)
// ============================================================================
enum class AudioTrackLock : uint16_t {
    ACCESS_GRANTED = 1,     // 208 - Доступ разрешен
    ACCESS_DENIED = 2,      // 209 - Доступ запрещен
    DOOR_OPEN = 3,          // 210 - Дверь открыта
    DOOR_CLOSED = 4,        // 211 - Дверь закрыта
    DOOR_TIMEOUT = 5,       // 212 - Дверь не закрыта
    DOOR_ALARM = 6,         // 213 - Тревога двери
    KEY_ADDED = 7,          // 214 - Ключ добавлен
    KEY_EXISTS = 8,         // 215 - Ключ уже существует
    KEY_DELETED = 9,        // 216 - Ключ удален
    KEY_NOT_FOUND = 10,     // 217 - Ключ не найден
    DB_CLEARED = 11,        // 218 - База данных очищена
    MEMORY_FULL = 12,       // 219 - Память заполнена
    PIN_REQUIRED = 13,      // 220 - Требуется PIN-код
    PIN_ACCEPTED = 14,      // 221 - PIN-код принят
    PIN_DENIED = 15,        // 222 - PIN-код отклонен
    PIN_BLOCKED = 16,       // 223 - PIN-код заблокирован
    REMOTE_OPEN = 17,       // 224 - Удаленное открытие
    BLOCKED_BY_ADMIN = 18,  // 225 - Заблокировано администратором
    MODE_NORMAL = 19,       // 226 - Обычный режим
    MODE_ACCEPT = 20,       // 227 - Режим пропуска
    MODE_TRIGGER = 21,      // 228 - Режим триггера
    MODE_MAINTENANCE_ON = 22, // 229 - Технический режим включен
    MODE_MAINTENANCE_OFF = 23,// 230 - Технический режим выключен
    MODE_EMERGENCY_ON = 24, // 231 - Аварийный режим включен
    MODE_EMERGENCY_OFF = 25,// 232 - Аварийный режим выключен
    SYSTEM_READY = 26,      // 233 - Система готова
    REBOOTING = 27,         // 234 - Перезагрузка
    SETTINGS_SAVED = 28,    // 235 - Настройки сохранены
    DB_UPDATE = 29          // 236 - База данных обновлена
};

// ============================================================================
// 7. ПАПКА 06: УМНЫЙ ДОМ (236-257)
// ============================================================================
enum class AudioTrackHome : uint16_t {
    GOOD_MORNING = 1,       // 237 - Доброе утро
    GOOD_NIGHT = 2,         // 238 - Доброй ночи
    AWAY = 3,               // 239 - Ушел
    BACK = 4,               // 240 - Вернулся
    LIGHT_ON = 5,           // 241 - Свет включен
    LIGHT_OFF = 6,          // 242 - Свет выключен
    BRIGHTNESS_SET = 7,     // 243 - Яркость установлена
    COLOR_TEMP_SET = 8,     // 244 - Цветовая температура установлена
    HEATING = 9,            // 245 - Нагрев
    COOLING = 10,           // 246 - Охлаждение
    FAN_ON = 11,            // 247 - Вентилятор включен
    FAN_OFF = 12,           // 248 - Вентилятор выключен
    TEMP_SET = 13,          // 249 - Температура установлена
    HUMIDITY_SET = 14,      // 250 - Влажность установлена
    CURTAINS_OPEN = 15,     // 251 - Шторы открыты
    CURTAINS_CLOSED = 16,   // 252 - Шторы закрыты
    BLINDS_UP = 17,         // 253 - Жалюзи подняты
    BLINDS_DOWN = 18,       // 254 - Жалюзи опущены
    AUTO_MODE = 19,         // 255 - Автоматический режим
    MANUAL_MODE = 20,       // 256 - Ручной режим
    NIGHT_SCENE = 21,       // 257 - Ночной сценарий
    DAY_SCENE = 22          // 258 - Дневной сценарий
};

// ============================================================================
// 8. ПАПКА 07: ЕДИНИЦЫ ИЗМЕРЕНИЯ (258-270)
// ============================================================================
enum class AudioTrackUnit : uint16_t {
    PERCENT = 1,            // 259 - Процентов
    DEGREES = 2,            // 260 - Градусов
    MMHG = 3,               // 261 - Миллиметров ртутного столба
    HUMIDITY = 4,           // 262 - Процентов влажности
    CUBIC_METERS = 5,       // 263 - Кубических метров
    KWH = 6,                // 264 - Киловатт-часов
    HOUR = 7,               // 265 - Час
    HOURS_2 = 8,            // 266 - Часа
    HOURS_5 = 9,            // 267 - Часов
    MINUTE = 10,            // 268 - Минута
    MINUTES_2 = 11,         // 269 - Минуты
    MINUTES_5 = 12,         // 270 - Минут
    WATT = 13               // 271 - Ватт
};

// ============================================================================
// 9. ПАПКА 08: ПОГОДА (271-282)
// ============================================================================
enum class AudioTrackWeather : uint16_t {
    FORECAST = 1,           // 272 - Прогноз погоды
    TODAY = 2,              // 273 - Сегодня
    TOMORROW = 3,           // 274 - Завтра
    SUNNY = 4,              // 275 - Солнечно
    CLOUDY = 5,             // 276 - Облачно
    RAIN = 6,               // 277 - Дождь
    SNOW = 7,               // 278 - Снег
    WIND = 8,               // 279 - Ветер
    FOG = 9,                // 280 - Туман
    STORM = 10,             // 281 - Гроза
    HAIL = 11,              // 282 - Град
    CLEAR = 12              // 283 - Ясно
};

// ============================================================================
// 10. ПАПКА 09: СЧЕТЧИКИ (283-295)
// ============================================================================
enum class AudioTrackCounter : uint16_t {
    WATER_COLD = 1,         // 284 - Холодная вода
    WATER_HOT = 2,          // 285 - Горячая вода
    GAS = 3,                // 286 - Газ
    POWER = 4,              // 287 - Электроэнергия
    TARIFF = 5,             // 288 - Тариф
    DAY = 6,                // 289 - День
    MONTH = 7,              // 290 - Месяц
    YEAR = 8,               // 291 - Год
    LIMIT_REACHED = 9,      // 292 - Лимит достигнут
    LIMIT_EXCEEDED = 10,    // 293 - Лимит превышен
    LEAK_WARNING = 11,      // 294 - Внимание утечка
    WATER_LEAK = 12,        // 295 - Утечка воды
    GAS_LEAK = 13           // 296 - Утечка газа
};

// ============================================================================
// 11. ПАПКА 10: СЦЕНАРИИ (296-305)
// ============================================================================
enum class AudioTrackScenario : uint16_t {
    GOOD_MORNING = 1,       // 297 - Доброе утро (сценарий)
    GOOD_NIGHT = 2,         // 298 - Доброй ночи (сценарий)
    AWAY = 3,               // 299 - Уход (сценарий)
    BACK = 4                // 300 - Возврат (сценарий)
};

// ============================================================================
// 12. ТИПЫ ЗВУКОВ
// ============================================================================
enum class AudioHardwareBeep : uint8_t {
    SHORT = 1,      // Короткий звук
    MEDIUM = 2,     // Средний звук
    LONG = 3,       // Длинный звук
    DOUBLE = 4,     // Двойной звук
    TRIPLE = 5,     // Тройной звук
    ALARM = 6,      // Тревога
    SUCCESS = 7,    // Успех
    ERROR = 8       // Ошибка
};

// ============================================================================
// 13. ОСНОВНОЙ КЛАСС
// ============================================================================
/**
 * @brief Расширения аудио-системы
 *
 * Не является самостоятельным модулем. Использует AudioManager.
 * Обеспечивает:
 * - Озвучивание чисел (0-999999)
 * - Озвучивание времени и даты
 * - Озвучивание имен пользователей (с кэшированием)
 * - Озвучивание событий СКУД
 * - Озвучивание системных событий
 * - Озвучивание погоды
 * - Озвучивание счетчиков
 * - Системные звуки
 */
class AudioExtensions {
public:
    // === КОЛБЭКИ ===
    typedef std::function<void(const char* phrase)> OnPhraseCallback;
    typedef std::function<void(uint16_t trackId)> OnTrackPlayCallback;

    // === СИНГЛТОН ===
    static AudioExtensions& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР ===
    AudioExtensions();
    ~AudioExtensions();

    // === ИНИЦИАЛИЗАЦИЯ ===
    void begin(AudioManager& audio);
    void end();
    bool isReady() const { return _audio != nullptr && _audio->isReady(); }

    // === СИСТЕМНЫЕ ЗВУКИ ===
    void triggerBeep(AudioHardwareBeep type);
    void triggerBeep(uint8_t track);

    // === ОЗВУЧИВАНИЕ ЧИСЕЛ ===
    void speakNumber(uint32_t number, bool isMale = true);
    void speakNumber(int number, bool isMale = true);
    void speakNumber(float number, uint8_t decimals = 1, bool isMale = true);
    void speakNumberWithUnit(uint32_t number, const char* unit, bool isMale = true);

    // === ОЗВУЧИВАНИЕ ВРЕМЕНИ ===
    void speakTime(uint8_t hours, uint8_t minutes);
    void speakTime(uint32_t unixTimestamp);
    void speakTime(const char* timeStr);

    // === ОЗВУЧИВАНИЕ ДАТЫ ===
    void speakDate(uint8_t day, uint8_t month, uint16_t year);
    void speakDate(uint32_t unixTimestamp);

    // === ОЗВУЧИВАНИЕ ИМЕН ===
    void speakName(uint16_t nameId);
    void speakName(const char* name);
    void speakName(const String& name);
    uint16_t findNameId(const char* name) const;

    // === ОЗВУЧИВАНИЕ СОБЫТИЙ СКУД ===
    void speakAccessGranted(uint16_t nameId = 0);
    void speakAccessDenied(uint16_t nameId = 0);
    void speakGreeting(uint16_t nameId = 0);
    void speakLockEvent(AudioTrackLock event, uint16_t nameId = 0);
    void speakLockEvent(uint16_t eventId, uint16_t nameId = 0);

    // === ОЗВУЧИВАНИЕ СИСТЕМНЫХ СОБЫТИЙ ===
    void speakSystemEvent(AudioTrackSystem event);
    void speakSystemEvent(uint16_t eventId);

    // === ОЗВУЧИВАНИЕ ПОГОДЫ ===
    void speakTemperature(float temp, bool isOutside = true);
    void speakHumidity(float humidity, bool isOutside = true);
    void speakPressure(float pressure);
    void speakWeather(float temp, float humidity, float pressure);
    void speakWeatherForecast(const char* condition, float temp, float humidity, float pressure);
    void speakWeatherForecast(const String& condition, float temp, float humidity, float pressure);

    // === ОЗВУЧИВАНИЕ СЧЕТЧИКОВ ===
    void speakCounter(uint32_t value, uint8_t type);
    void speakWaterCounter(uint32_t value);
    void speakGasCounter(uint32_t value);
    void speakPowerCounter(uint32_t value);
    void speakTariff(uint8_t tariff);

    // === ОЗВУЧИВАНИЕ СОБЫТИЙ УМНОГО ДОМА ===
    void speakHomeEvent(AudioTrackHome event);
    void speakHomeEvent(uint16_t eventId);

    // === ОЗВУЧИВАНИЕ СЦЕНАРИЕВ ===
    void speakScenario(AudioTrackScenario scenario);
    void speakScenario(uint16_t scenarioId);
    void speakGoodMorning();
    void speakGoodNight();

    // === УПРАВЛЕНИЕ КЭШЕМ ИМЕН ===
    void addNameToCache(const char* name, uint16_t trackId);
    void addNameToCache(const String& name, uint16_t trackId);
    void removeNameFromCache(const char* name);
    void clearNameCache();
    size_t getNameCacheSize() const { return _nameCache.size(); }
    bool isNameCached(const char* name) const;
    void rebuildNameCache();

    // === НАСТРОЙКИ ===
    void setMaxCacheSize(size_t size) { _maxCacheSize = size; }
    void setDefaultLanguage(const char* lang) {
        if (lang) _defaultLanguage = lang;
    }
    void setVolumeMultiplier(float multiplier) { _volumeMultiplier = multiplier; }
    void setPriority(AudioPriority priority) { _defaultPriority = priority; }

    // === ДИАГНОСТИКА ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

    // === КОЛБЭКИ ===
    void setOnPhrase(OnPhraseCallback cb) { _onPhrase = cb; }
    void setOnTrackPlay(OnTrackPlayCallback cb) { _onTrackPlay = cb; }

private:
    // === ВНУТРЕННИЕ МЕТОДЫ ===
    void decomposeNumber(uint32_t number, bool isMale);
    uint16_t getUnitTrack(uint32_t number, bool isHours) const;
    void speakThousands(uint32_t number, bool isMale);
    void speakHundreds(uint32_t number);
    void speakTens(uint32_t number, bool isMale);
    void speakTensToNineteen(uint32_t number);
    void speakUnits(uint32_t number, bool isMale);
    void buildNameCache();
    String normalizeName(const char* name) const;
    bool isInitialized() const;
    void playTrack(uint16_t trackId, AudioPriority priority = AudioPriority::INFO);
    void playPhrase(const std::vector<uint16_t>& tracks, AudioPriority priority = AudioPriority::INFO);
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    uint8_t getGenderForNumber(uint32_t number, bool isMale) const;

    // === ДАННЫЕ ===
    AudioManager* _audio = nullptr;
    std::map<String, uint16_t> _nameCache;
    bool _cacheBuilt = false;
    size_t _maxCacheSize = 200;
    float _volumeMultiplier = 1.0f;
    String _defaultLanguage = "ru";
    AudioPriority _defaultPriority = AudioPriority::INFO;

    // Колбэки
    OnPhraseCallback _onPhrase = nullptr;
    OnTrackPlayCallback _onTrackPlay = nullptr;

    // Константы
    static constexpr uint8_t BEEP_SHORT_MS = 50;
    static constexpr uint8_t BEEP_MEDIUM_MS = 100;
    static constexpr uint16_t BEEP_LONG_MS = 500;
    static constexpr uint32_t MAX_NUMBER = 999999;
};

// #endif // AUDIOEXTENSIONS_H