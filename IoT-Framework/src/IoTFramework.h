// ============================================================================
// IoTFramework.h - ГЛАВНЫЙ ЗАГОЛОВОЧНЫЙ ФАЙЛ МИКРООС
// ============================================================================
// Только системные модули. Бизнес-логика подключается отдельно в проекте.
// ============================================================================

#pragma once

// === ЯДРО ===
#include "core/AppCore.h"
#include "core/ConfigManager.h"
#include "core/DeviceManager.h"
#include "core/IModule.h"
#include "core/LogManager.h"

// === АППАРАТУРА ===
#include "hardware/AudioManager.h"
#include "hardware/AudioExtensions.h"
#include "hardware/RTCManager.h"
#include "hardware/TempSensorManager.h"
#include "hardware/WiegandManager.h"

// === СЕТЬ ===
#include "network/MqttManager.h"
#include "network/OTAManager.h"
#include "network/WebServerManager.h"

// === БЕЗОПАСНОСТЬ ===
#include "security/UserManager.h"

// === СИСТЕМА ===
#include "system/PazManager.h"
#include "system/DataLoggerManager.h"

// ============================================================================
// ГЛОБАЛЬНАЯ ВЕРСИЯ
// ============================================================================

#define MICRO_OS_VERSION "4.2.2"

// ============================================================================
// МАКРОСЫ ЛОГИРОВАНИЯ
// ============================================================================

#define LOG_DEBUG(tag, msg) LOGGER.addLog(LOG_DEBUG, tag, msg)
#define LOG_INFO(tag, msg) LOGGER.addLog(LOG_INFO, tag, msg)
#define LOG_WARN(tag, msg) LOGGER.addLog(LOG_WARNING, tag, msg)
#define LOG_ERROR(tag, msg) LOGGER.addLog(LOG_ERROR, tag, msg)
#define LOG_CRIT(tag, msg) LOGGER.addLog(LOG_CRITICAL, tag, msg)

#define LOG_DEBUG_F(tag, format, ...) LOGGER.addLogF(LOG_DEBUG, tag, format, ##__VA_ARGS__)
#define LOG_INFO_F(tag, format, ...) LOGGER.addLogF(LOG_INFO, tag, format, ##__VA_ARGS__)
#define LOG_WARN_F(tag, format, ...) LOGGER.addLogF(LOG_WARNING, tag, format, ##__VA_ARGS__)
#define LOG_ERROR_F(tag, format, ...) LOGGER.addLogF(LOG_ERROR, tag, format, ##__VA_ARGS__)
#define LOG_CRIT_F(tag, format, ...) LOGGER.addLogF(LOG_CRITICAL, tag, format, ##__VA_ARGS__)

// Версия прошивки (должна быть переопределена в каждом проекте)
#ifndef CURRENT_VERSION
#define CURRENT_VERSION "4.2.2"
#endif