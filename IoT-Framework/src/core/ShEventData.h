// ============================================================================
// ShEventData.h - УНИВЕРСАЛЬНАЯ СТРУКТУРА ДАННЫХ ДЛЯ СОБЫТИЙ МикроОС v5.0
// ============================================================================
// Описание: Потокобезопасная структура данных фиксированного размера для
//           обмена информацией между модулями через событийную шину.
//
// КЛЮЧЕВЫЕ ПРИНЦИПЫ:
// 1. Фиксированный размер - все поля статические, нет динамической памяти
// 2. Потокобезопасность - может передаваться через очередь FreeRTOS
// 3. Универсальность - покрывает все сценарии обмена (числа, строки, UID)
// 4. Zero-Heap Allocation - не использует new/delete/malloc/strdup
// 5. Размер структуры <= 128 байт для эффективной передачи
// ============================================================================
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <type_traits>

// ============================================================================
// 1. ПЕРЕЧЕНЬ ТИПОВ СОБЫТИЙ (ЕДИНЫЙ ИСТОЧНИК ИСТИНЫ)
// ============================================================================
/**
 * @brief Единый перечень типов событий для всей МикроОС
 *
 * @note Значения сгруппированы по функциональным блокам для удобства.
 *       ВСЕ определения констант EVENT_* должны быть ТОЛЬКО здесь!
 */
enum ShEventType : uint16_t {
    // === СИСТЕМНЫЕ СОБЫТИЯ (0x0000 - 0x00FF) ===
    EVENT_SYS_NONE = 0x0000,        // Пустое событие (заглушка)
    EVENT_SYS_BOOT = 0x0001,        // Запуск системы
    EVENT_SYS_READY = 0x0002,       // Система готова к работе
    EVENT_SYS_SHUTDOWN = 0x0003,    // Выключение системы
    EVENT_SYS_RESTART = 0x0004,     // Перезагрузка системы
    EVENT_SYS_HEARTBEAT = 0x0005,   // Периодический "тик" жизни модуля
    EVENT_SYS_ERROR = 0x0006,       // Общая системная ошибка

    // === МОДУЛИ (0x0100 - 0x01FF) ===
    EVENT_MODULE_REGISTERED = 0x0100, // Модуль зарегистрирован в ядре
    EVENT_MODULE_INIT_START = 0x0101, // Начало инициализации модуля
    EVENT_MODULE_INIT_DONE = 0x0102,  // Инициализация завершена
    EVENT_MODULE_ERROR = 0x0103,      // Ошибка в модуле

    // === КОНФИГУРАЦИЯ И ХРАНИЛИЩЕ (0x0200 - 0x02FF) ===
    EVENT_CONFIG_UPDATE = 0x0200,   // Обновление конфигурации
    EVENT_FILE_WRITE_REQ = 0x0210,  // Запрос на запись файла (в StorageServer)
    EVENT_FILE_READ_REQ = 0x0211,   // Запрос на чтение файла (в StorageServer)
    EVENT_FILE_WRITE_RESP = 0x0212, // Ответ на запись файла
    EVENT_FILE_READ_RESP = 0x0213,  // Ответ на чтение файла
    EVENT_FILE_SAVE_USERS = 0x0214, // Запрос на сохранение базы пользователей

    // === СЕТЕВЫЕ СОБЫТИЯ (0x0300 - 0x03FF) ===
    EVENT_NET_CONNECTED = 0x0300,   // Сеть подключена (получен IP)
    EVENT_NET_DISCONNECTED = 0x0301,// Сеть отключена
    EVENT_NET_IP_CHANGED = 0x0302,  // IP-адрес изменился
    EVENT_MQTT_CMD = 0x0310,        // Получена команда по MQTT
    EVENT_MQTT_PUBLISH = 0x0311,    // Опубликовать данные по MQTT
    EVENT_MQTT_CONNECTED = 0x0312,  // MQTT-клиент подключен к брокеру
    EVENT_MQTT_DISCONNECTED = 0x0313,// MQTT-клиент отключен от брокера

    // === МОНИТОРИНГ И ПАЗ (0x0400 - 0x04FF) ===
    EVENT_TEMP_CHANGED = 0x0400,    // Изменилась температура кристалла ESP32
    EVENT_HEALTH_WARNING = 0x0410,  // Предупреждение о состоянии системы
    EVENT_HEALTH_CRITICAL = 0x0411, // Критическое состояние системы
    EVENT_HEALTH_RESTORED = 0x0412, // Здоровье восстановлено
    EVENT_PAZ_ALARM = 0x0420,       // Сработал аварийный триггер ПАЗ
    EVENT_PAZ_PANIC = 0x0421,       // Режим паники (аварийная остановка)
    EVENT_PAZ_PANIC_CLEARED = 0x0422,// Режим паники снят

    // === БИЗНЕС-ЛОГИКА СКУД (0x0500 - 0x05FF) ===
    EVENT_CARD_READ = 0x0500,       // Считана карта (UID передан в payload)
    EVENT_ACCESS_GRANTED = 0x0501,  // Доступ разрешен
    EVENT_ACCESS_DENIED = 0x0502,   // Доступ запрещен
    EVENT_RELAY_ON = 0x0510,        // Включить реле (открыть замок)
    EVENT_RELAY_OFF = 0x0511,       // Выключить реле (закрыть замок)
    EVENT_DOOR_OPEN = 0x0520,       // Дверь открыта (сигнал с датчика)
    EVENT_DOOR_CLOSED = 0x0521,     // Дверь закрыта
    EVENT_DOOR_ALARM = 0x0522,      // Тревога открытой двери
    EVENT_EXIT_BUTTON = 0x0530,     // Нажата кнопка выхода

    // === ЛОГИРОВАНИЕ (0x0600 - 0x06FF) ===
    EVENT_LOG_MESSAGE = 0x0600,     // Лог-сообщение для DataLoggerManager
    EVENT_LOG_DEBUG = 0x0601,       // Отладочное сообщение
    EVENT_LOG_ERROR = 0x0602,       // Сообщение об ошибке
    EVENT_LOG_CRITICAL = 0x0603,    // Критическое сообщение

    // === ОБНОВЛЕНИЕ ПО ВОЗДУХУ (OTA) (0x0700 - 0x07FF) ===
    EVENT_OTA_START = 0x0700,       // Начало OTA-обновления
    EVENT_OTA_PROGRESS = 0x0701,    // Прогресс OTA-обновления
    EVENT_OTA_SUCCESS = 0x0702,     // OTA-обновление успешно
    EVENT_OTA_FAILED = 0x0703,      // OTA-обновление не удалось
    EVENT_OTA_CANCELLED = 0x0704,   // OTA-обновление отменено
    EVENT_OTA_UPDATE_FOUND = 0x0705,// Найдено обновление
    EVENT_OTA_ROLLBACK = 0x0706,    // Откат OTA
    EVENT_OTA_ROLLBACK_SUCCESS = 0x0707, // Откат успешен
    EVENT_OTA_ROLLBACK_FAILED = 0x0708,  // Откат не удался

    // === ВЕБ-СЕРВЕР (0x0800 - 0x08FF) ===
    EVENT_WEB_REQUEST = 0x0800,     // HTTP-запрос
    EVENT_WEB_AUTH_SUCCESS = 0x0801,// Успешная аутентификация
    EVENT_WEB_AUTH_FAILED = 0x0802, // Неудачная аутентификация
    EVENT_WEB_API_CALL = 0x0803,    // Вызов API
    EVENT_WEB_UPLOAD_START = 0x0804,// Начало загрузки файла
    EVENT_WEB_UPLOAD_PROGRESS = 0x0805, // Прогресс загрузки
    EVENT_WEB_UPLOAD_COMPLETE = 0x0806, // Загрузка завершена
    EVENT_WEB_ERROR = 0x0807,       // Ошибка веб-сервера
    EVENT_WEB_SESSION_CREATED = 0x0808, // Сессия создана
    EVENT_WEB_SESSION_DESTROYED = 0x0809, // Сессия уничтожена

    // === SMART LOCK (0x1200 - 0x12FF) ===
    EVENT_SMART_LOCK_OPEN = 0x1200,     // Замок открыт
    EVENT_SMART_LOCK_CLOSE = 0x1201,    // Замок закрыт
    EVENT_SMART_LOCK_MODE_CHANGE = 0x1202, // Режим изменен
    EVENT_SMART_LOCK_DOOR_OPEN = 0x1203,    // Дверь открыта
    EVENT_SMART_LOCK_DOOR_CLOSED = 0x1204,  // Дверь закрыта
    EVENT_SMART_LOCK_ALARM = 0x1205,    // Тревога
    EVENT_SMART_LOCK_CARD_GRANTED = 0x1206, // Доступ разрешен
    EVENT_SMART_LOCK_CARD_DENIED = 0x1207,  // Доступ запрещен
    EVENT_SMART_LOCK_CARD_READ = 0x1208,    // Карта прочитана

    // === ПОЛЬЗОВАТЕЛЬСКИЕ СОБЫТИЯ (0x1000 - 0xFFFF) ===
    EVENT_USER_BASE = 0x1000         // Базовый ID для пользовательских событий
};

// ============================================================================
// 2. УРОВНИ ЛОГИРОВАНИЯ
// ============================================================================
enum LogLevel : uint8_t {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARNING = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_CRITICAL = 4
};

// ============================================================================
// 3. СТРУКТУРА СОБЫТИЯ (FIXED-SIZE, ZERO-HEAP, <= 128 байт)
// ============================================================================
/**
 * @brief Структура события МикроОС (фиксированный размер)
 *
 * Размер структуры: 1 + 1 + 1 + union(64) = 67 байт (с учетом выравнивания ~68)
 * Вмещается в 128 байт, эффективно передается через очередь FreeRTOS.
 *
 * ПРИМЕРЫ ИСПОЛЬЗОВАНИЯ:
 *
 * 1. Отправить событие с температурой:
 *    ShEventData ev;
 *    ev.type = EVENT_TEMP_CHANGED;
 *    ev.senderId = TEMP_SENSOR_ID;
 *    ev.targetModuleId = 0xFF; // Всем
 *    ev.payload.floatVal = 25.5f;
 *    AppCore::getInstance().publishEvent(ev);
 *
 * 2. Отправить команду на открытие замка:
 *    ShEventData ev;
 *    ev.type = EVENT_RELAY_ON;
 *    ev.senderId = SMART_LOCK_ID;
 *    ev.targetModuleId = RELAY_MANAGER_ID;
 *    ev.payload.cmdData.value = 2000; // Длительность открытия в мс
 *    AppCore::getInstance().publishEvent(ev);
 *
 * 3. Отправить UID карты:
 *    ShEventData ev;
 *    ev.type = EVENT_CARD_READ;
 *    ev.senderId = WIEGAND_ID;
 *    ev.targetModuleId = ACCESS_MANAGER_ID;
 *    ev.payload.cardData.uidLen = 4;
 *    memcpy(ev.payload.cardData.uid, uid, 4);
 *    AppCore::getInstance().publishEvent(ev);
 */
struct ShEventData {
    // --- Основные поля (3 байта) ---
    ShEventType type;           // Тип события (2 байта)
    uint8_t senderId;           // ID модуля-отправителя (1 байт)
    uint8_t targetModuleId;     // ID модуля-получателя (0xFF = всем) (1 байт)

    // ========================================================================
    // Анонимное объединение (Payload) — все поля фиксированного размера
    // Общий размер union: 64 байта (максимальный контейнер)
    // ========================================================================
    union {
        // --- Контейнер 1: Целочисленные параметры (8 байт) ---
        struct {
            uint32_t val1;
            uint32_t val2;
        } intData;

        // --- Контейнер 2: Данные с плавающей точкой (4 байта) ---
        float floatVal;

        // --- Контейнер 3: Структурированный лог (1 + 48 = 49 байт) ---
        struct {
            LogLevel level;
            char msg[48];
        } logData;

        // --- Контейнер 4: Команда фиксированной длины (4 + 4 + 48 = 56 байт) ---
        struct {
            uint32_t command;
            int32_t value;
            char payload[48];
        } cmdData;

        // --- Контейнер 5: Данные карты (8 + 1 + 32 = 41 байт) ---
        struct {
            uint8_t uid[8];
            uint8_t uidLen;
            char userName[32];
        } cardData;

        // --- Контейнер 6: Работа с файлами (32 + 4 + 28 = 64 байта) ---
        // Оптимизировано: dataBuffer уменьшен до 28 байт для сохранения размера 64
        struct {
            char fileName[32];
            uint32_t dataSize;
            char dataBuffer[28];
        } fileData;

        // --- Контейнер 7: Сырой массив байт (64 байта) ---
        struct {
            uint8_t raw[64];
            uint8_t rawLen;
        } rawData;

        // --- Контейнер 8: Статусная строка (64 байта) ---
        char statusStr[64];

    } payload;

    // ========================================================================
    // 4. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ (inline для удобства)
    // ========================================================================

    /**
     * @brief Очищает всю структуру (заполняет нулями)
     */
    inline void clear() {
        memset(this, 0, sizeof(ShEventData));
    }

    /**
     * @brief Проверяет, является ли событие целевым для модуля
     * @param moduleId ID модуля, который проверяет событие
     * @return true если событие предназначено этому модулю или всем
     */
    inline bool isForMe(uint8_t moduleId) const {
        return (targetModuleId == 0xFF || targetModuleId == moduleId);
    }

    /**
     * @brief Проверяет, является ли событие широковещательным (для всех модулей)
     */
    inline bool isBroadcast() const {
        return (targetModuleId == 0xFF);
    }

    /**
     * @brief Проверяет, является ли событие системным
     */
    inline bool isSystemEvent() const {
        return (type < EVENT_USER_BASE);
    }

    /**
     * @brief Проверяет, является ли событие пользовательским
     */
    inline bool isUserEvent() const {
        return (type >= EVENT_USER_BASE);
    }
};

// ============================================================================
// 5. СТАТИЧЕСКИЕ ПРОВЕРКИ РАЗМЕРОВ (ВЫПОЛНЯЮТСЯ ПРИ КОМПИЛЯЦИИ)
// ============================================================================
// Проверяем, что структура действительно имеет фиксированный размер
// и помещается в очередь FreeRTOS.
static_assert(sizeof(ShEventData) <= 128,
              "ShEventData size must be <= 128 bytes for FreeRTOS queue");

static_assert(std::is_trivially_copyable<ShEventData>::value,
              "ShEventData must be trivially copyable for FreeRTOS queue");

static_assert(offsetof(ShEventData, payload) == 4,
              "Payload alignment issue - check struct packing");

// ============================================================================
// 6. КОНСТАНТЫ ДЛЯ СОВМЕСТИМОСТИ (АЛИАСЫ)
// ============================================================================
// Для обратной совместимости с кодом, использующим SH_EVENT_* имена
// Эти алиасы позволяют использовать как новые, так и старые имена констант

// Системные события
constexpr ShEventType SH_EVENT_SYS_BOOT = EVENT_SYS_BOOT;
constexpr ShEventType SH_EVENT_SYS_READY = EVENT_SYS_READY;
constexpr ShEventType SH_EVENT_SYS_SHUTDOWN = EVENT_SYS_SHUTDOWN;
constexpr ShEventType SH_EVENT_SYS_RESTART = EVENT_SYS_RESTART;
constexpr ShEventType SH_EVENT_MODULE_REGISTERED = EVENT_MODULE_REGISTERED;
constexpr ShEventType SH_EVENT_MODULE_INIT_START = EVENT_MODULE_INIT_START;
constexpr ShEventType SH_EVENT_MODULE_INIT_DONE = EVENT_MODULE_INIT_DONE;
constexpr ShEventType SH_EVENT_MODULE_ERROR = EVENT_MODULE_ERROR;
constexpr ShEventType SH_EVENT_MODULE_TICK = EVENT_SYS_HEARTBEAT;
constexpr ShEventType SH_EVENT_MODULE_STATUS = EVENT_SYS_HEARTBEAT;

// Сетевые события
constexpr ShEventType SH_EVENT_NET_CONNECTED = EVENT_NET_CONNECTED;
constexpr ShEventType SH_EVENT_NET_DISCONNECTED = EVENT_NET_DISCONNECTED;
constexpr ShEventType SH_EVENT_NET_IP_CHANGED = EVENT_NET_IP_CHANGED;

// Здоровье
constexpr ShEventType SH_EVENT_HEALTH_OK = EVENT_HEALTH_RESTORED;
constexpr ShEventType SH_EVENT_HEALTH_WARNING = EVENT_HEALTH_WARNING;
constexpr ShEventType SH_EVENT_HEALTH_CRITICAL = EVENT_HEALTH_CRITICAL;
constexpr ShEventType SH_EVENT_HEALTH_RESTORED = EVENT_HEALTH_RESTORED;

// Время
constexpr ShEventType SH_EVENT_TIME_SYNC = EVENT_CONFIG_UPDATE;
constexpr ShEventType SH_EVENT_TIME_CHANGED = EVENT_CONFIG_UPDATE;
constexpr ShEventType SH_EVENT_TEMPERATURE_UPDATE = EVENT_TEMP_CHANGED;

// Команды
constexpr ShEventType SH_EVENT_CMD_EXECUTE = EVENT_MQTT_CMD;
constexpr ShEventType SH_EVENT_CMD_RESPONSE = EVENT_MQTT_PUBLISH;

// Пользовательские события
constexpr uint16_t SH_EVENT_USER_BASE = EVENT_USER_BASE;