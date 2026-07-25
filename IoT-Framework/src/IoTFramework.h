// ============================================================================
// IoTFramework.h - ГЛАВНЫЙ ЗАГОЛОВОЧНЫЙ ФАЙЛ МикроОС v5.0
// ============================================================================
// Описание: Единая точка входа для всех модулей МикроОС.
//           Подключает ядро, аппаратуру, сеть, безопасность и системные службы.
//
// ПРАВИЛА ИСПОЛЬЗОВАНИЯ:
// 1. Этот файл подключается в главном .ino файле проекта.
// 2. Бизнес-логика (SmartLock, SmartLight и т.д.) подключается отдельно.
// 3. Все модули доступны через синглтоны (ClassName::getInstance()).
// 4. НЕ ИСПОЛЬЗОВАТЬ глобальные переменные (MQTT, OTA, WebServerCore и т.д.).
//
// ВЕРСИЯ: 5.0.0
// ============================================================================
#pragma once

// ============================================================================
// 1. ПРОВЕРКА СОВМЕСТИМОСТИ
// ============================================================================
#if __cplusplus < 201703L
#error "MicroOS v5.0 requires C++17 or higher"
#endif

// ============================================================================
// 2. ЯДРО СИСТЕМЫ
// ============================================================================
#include "core/AppCore.h"          // Оркестратор, ядро системы
#include "core/IModule.h"          // Базовый интерфейс модулей
#include "core/ShEventData.h"      // Структура данных для событий
#include "core/ConfigManager.h"    // Управление конфигурацией
#include "core/LogManager.h"       // Централизованное логирование
#include "core/DeviceManager.h"    // Управление аппаратными устройствами

// ============================================================================
// 3. АППАРАТУРНЫЕ МОДУЛИ
// ============================================================================
#include "hardware/RTCManager.h"       // Часы реального времени
#include "hardware/WiegandManager.h"   // Считыватель карт Wiegand
#include "hardware/AudioManager.h"     // Управление аудио (DFPlayer Mini)
#include "hardware/AudioExtensions.h"  // Расширенные аудио-функции
#include "hardware/TempSensorManager.h"// Датчик температуры ESP32

// ============================================================================
// 4. СЕТЕВЫЕ МОДУЛИ
// ============================================================================
#include "network/NetworkManager.h"    // Управление Ethernet (WT32-ETH01)
#include "network/MqttManager.h"       // MQTT-клиент
#include "network/WebServerManager.h"  // Веб-сервер и REST API
#include "network/OTAManager.h"        // Обновление прошивки по воздуху

// ============================================================================
// 5. МОДУЛИ БЕЗОПАСНОСТИ
// ============================================================================
#include "security/UserManager.h"      // Базовая аутентификация (LIGHT)
#include "security/AccessManager.h"    // Полное управление доступом (FULL)

// ============================================================================
// 6. СИСТЕМНЫЕ СЛУЖБЫ
// ============================================================================
#include "system/PazManager.h"         // Противоаварийная защита (ПАЗ)
#include "system/StorageServer.h"      // Централизованный доступ к LittleFS
#include "system/DataLoggerManager.h"  // Логирование данных для аналитики

// ============================================================================
// 7. ВЕРСИЯ СИСТЕМЫ
// ============================================================================
#define MICRO_OS_VERSION "5.0.0"

/**
 * @brief Возвращает версию МикроОС в виде строки
 * @return Строка с версией (формат "major.minor.patch")
 */
inline const char* getMicroOSVersion() {
    return MICRO_OS_VERSION;
}

/**
 * @brief Возвращает версию МикроОС в виде числа для сравнения
 * @return Числовое представление версии (например, 5000 для 5.0.0)
 */
inline uint32_t getMicroOSVersionCode() {
    return 5000; // 5.0.0
}

// ============================================================================
// 8. МАКРОСЫ ДЛЯ ЛОГИРОВАНИЯ (ПРОВЕРКА СОВМЕСТИМОСТИ)
// ============================================================================
/**
 * @brief Макросы логирования с проверкой существования LogManager
 *
 * ВНИМАНИЕ: Эти макросы используют синглтон LogManager.
 *           Убедитесь, что LogManager инициализирован перед использованием.
 *
 * Пример использования:
 *   LOG_DEBUG("TAG", "Value: %d", value);
 *   LOG_INFO("TAG", "Started");
 *   LOG_ERROR("TAG", "Error code: %d", code);
 */

// --- Базовые макросы ---
#define LOG_DEBUG(tag, msg) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            LogManager::getInstance().addLog(LOG_DEBUG, tag, msg); \
        } \
    } while(0)

#define LOG_INFO(tag, msg) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            LogManager::getInstance().addLog(LOG_INFO, tag, msg); \
        } \
    } while(0)

#define LOG_WARN(tag, msg) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            LogManager::getInstance().addLog(LOG_WARNING, tag, msg); \
        } \
    } while(0)

#define LOG_ERROR(tag, msg) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            LogManager::getInstance().addLog(LOG_ERROR, tag, msg); \
        } \
    } while(0)

#define LOG_CRIT(tag, msg) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            LogManager::getInstance().addLog(LOG_CRITICAL, tag, msg); \
        } \
    } while(0)

// --- Форматированные макросы ---
#define LOG_DEBUG_F(tag, format, ...) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            char _buf[128]; \
            snprintf(_buf, sizeof(_buf), format, ##__VA_ARGS__); \
            LogManager::getInstance().addLog(LOG_DEBUG, tag, _buf); \
        } \
    } while(0)

#define LOG_INFO_F(tag, format, ...) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            char _buf[128]; \
            snprintf(_buf, sizeof(_buf), format, ##__VA_ARGS__); \
            LogManager::getInstance().addLog(LOG_INFO, tag, _buf); \
        } \
    } while(0)

#define LOG_WARN_F(tag, format, ...) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            char _buf[128]; \
            snprintf(_buf, sizeof(_buf), format, ##__VA_ARGS__); \
            LogManager::getInstance().addLog(LOG_WARNING, tag, _buf); \
        } \
    } while(0)

#define LOG_ERROR_F(tag, format, ...) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            char _buf[128]; \
            snprintf(_buf, sizeof(_buf), format, ##__VA_ARGS__); \
            LogManager::getInstance().addLog(LOG_ERROR, tag, _buf); \
        } \
    } while(0)

#define LOG_CRIT_F(tag, format, ...) \
    do { \
        if (LogManager::getInstance().isInitialized()) { \
            char _buf[128]; \
            snprintf(_buf, sizeof(_buf), format, ##__VA_ARGS__); \
            LogManager::getInstance().addLog(LOG_CRITICAL, tag, _buf); \
        } \
    } while(0)

// --- Условное логирование ---
#define LOG_DEBUG_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_DEBUG_F(tag, msg, ##__VA_ARGS__); } } while(0)

#define LOG_INFO_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_INFO_F(tag, msg, ##__VA_ARGS__); } } while(0)

#define LOG_WARN_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_WARN_F(tag, msg, ##__VA_ARGS__); } } while(0)

#define LOG_ERROR_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_ERROR_F(tag, msg, ##__VA_ARGS__); } } while(0)

#define LOG_CRIT_IF(cond, tag, msg, ...) \
    do { if (cond) { LOG_CRIT_F(tag, msg, ##__VA_ARGS__); } } while(0)

// ============================================================================
// 9. ВЕРСИЯ ПРОШИВКИ ПО УМОЛЧАНИЮ
// ============================================================================
/**
 * @brief Версия прошивки по умолчанию.
 *        Должна быть переопределена в каждом проекте.
 *
 * @example В файле проекта:
 *   #define CURRENT_VERSION "2.5.0"
 */
#ifndef CURRENT_VERSION
#define CURRENT_VERSION "1.0.0"
#endif

// ============================================================================
// 10. КОМПАКТНЫЕ АЛИАСЫ ДЛЯ ЧАСТО ИСПОЛЬЗУЕМЫХ МОДУЛЕЙ
// ============================================================================
// ВНИМАНИЕ: Эти алиасы предназначены для УДОБСТВА, а не для обязательного использования.
//           Рекомендуется использовать полные имена для ясности кода.
//
// Пример использования:
//   Core.registerModule(&Logger.getInstance());
//   Core.registerModule(&Mqtt.getInstance());
//
// Вместо:
//   AppCore::getInstance().registerModule(&LogManager::getInstance());
//   AppCore::getInstance().registerModule(&MqttManager::getInstance());

// --- Ядро ---
using Core = AppCore;
using Logger = LogManager;
using Config = ConfigManager;
using Devices = DeviceManager;

// --- Аппаратура ---
using RTC = RTCManager;
using Wiegand = WiegandManager;
using Audio = AudioManager;
using AudioExt = AudioExtensions;
using TempSensor = TempSensorManager;

// --- Сеть ---
// using Network = NetworkManager;
// using Mqtt = MqttManager;
// using WebServer = WebServerManager;
// using OTA = OTAManager;

// --- Безопасность ---
using AccessCtrl = AccessManager;

// --- Система ---
using PAZ = PazManager;
using Storage = StorageServer;
using DataLog = DataLoggerManager;

// ============================================================================
// 11. СТАТИЧЕСКАЯ ПРОВЕРКА СОВМЕСТИМОСТИ (выполняется во время компиляции)
// ============================================================================
// Проверяем, что все необходимые типы доступны
namespace MicroOS {
    namespace CompileTime {
        // Проверка наличия IModule
        static_assert(std::is_abstract<IModule>::value,
                      "IModule must be an abstract class");

        // Проверка размера ShEventData (должен быть фиксированным)
        static_assert(sizeof(ShEventData) <= 256,
                      "ShEventData size exceeds 256 bytes");

        // Проверка, что AppCore является синглтоном
        static_assert(!std::is_copy_constructible<AppCore>::value,
                      "AppCore must not be copyable");
        static_assert(!std::is_copy_assignable<AppCore>::value,
                      "AppCore must not be copy-assignable");
    }
}

// ============================================================================
// 12. ВСПОМОГАТЕЛЬНЫЙ МАКРОС ДЛЯ РЕГИСТРАЦИИ МОДУЛЯ
// ============================================================================
/**
 * @brief Макрос для удобной регистрации модуля в ядре
 * @param module Экземпляр модуля (синглтон)
 *
 * @example
 *   REGISTER_MODULE(LogManager::getInstance());
 *   REGISTER_MODULE(MqttManager::getInstance());
 */
#define REGISTER_MODULE(module) \
    AppCore::getInstance().registerModule(&(module))

// ============================================================================
// 13. МАКРОС ДЛЯ ПУБЛИКАЦИИ СОБЫТИЙ (НОВАЯ ШИНА)
// ============================================================================
/**
 * @brief Макрос для публикации события через новую шину
 * @param event Событие для публикации
 *
 * @example
 *   ShEventData ev;
 *   ev.type = EVENT_CARD_READ;
 *   ev.senderId = _moduleId;
 *   ev.targetModuleId = 0xFF;
 *   PUBLISH_EVENT(ev);
 */
#define PUBLISH_EVENT(event) \
    AppCore::getInstance().publishEvent(event)

// ============================================================================
// 14. МАКРОС ДЛЯ ОТЛАДКИ (ВЫВОД В SERIAL)
// ============================================================================
/**
 * @brief Макрос для отладочного вывода в Serial (без логирования)
 * @param ... Форматированная строка как в printf
 *
 * @note Использовать ТОЛЬКО для отладки!
 *       Для продакшена используйте LOG_DEBUG.
 */
#define DEBUG_PRINT(...) \
    do { \
        Serial.printf(__VA_ARGS__); \
        Serial.println(); \
    } while(0)

/**
 * @brief Макрос для отладочного вывода в Serial (без перевода строки)
 */
#define DEBUG_PRINT_NO_NL(...) \
    Serial.printf(__VA_ARGS__)

// ============================================================================
// 15. ЗАВЕРШЕНИЕ
// ============================================================================
// Убеждаемся, что файл включен только один раз
#pragma once

// Конец файла IoTFramework.h