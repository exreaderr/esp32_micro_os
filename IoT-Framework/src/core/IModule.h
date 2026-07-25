// ============================================================================
// IModule.h — МикроОС v5.0
// Интерфейс модуля с расширяемой системой событий
// ============================================================================
#pragma once

#include <Arduino.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <functional>
#include <cstdint>
#include <cstring>

// -----------------------------------------------------------------------------
// БАЗОВЫЕ КОНСТАНТЫ
// -----------------------------------------------------------------------------
constexpr uint8_t MODULE_NAME_MAX_LEN = 32;
constexpr uint8_t MODULE_VERSION_MAX_LEN = 16;
constexpr uint8_t PAYLOAD_MAX_LEN = 256;
constexpr uint32_t MODULE_ID_DEVICE_APP = 0xFF;  // ID для бизнес-логики устройства

// -----------------------------------------------------------------------------
// СИСТЕМНЫЕ СОБЫТИЯ (0x0000-0x00FF)
// -----------------------------------------------------------------------------
enum SystemEvents : int32_t {
    SH_EVENT_NONE = 0,
    SH_EVENT_SYS_BOOT = 0x0001,
    SH_EVENT_SYS_READY = 0x0002,
    SH_EVENT_SYS_RESTART = 0x0003,
    SH_EVENT_SYS_SHUTDOWN = 0x0004,
    SH_EVENT_SYS_PANIC = 0x0005,
    SH_EVENT_MODULE_TICK = 0x0008,
    SH_EVENT_LOG_ENTRY = 0x0010,
    SH_EVENT_LOG_DEBUG = 0x0011,
    SH_EVENT_LOG_INFO = 0x0012,
    SH_EVENT_LOG_WARNING = 0x0013,
    SH_EVENT_LOG_ERROR = 0x0014,
    SH_EVENT_CONFIG_CHANGED = 0x0020,
    SH_EVENT_CONFIG_CUSTOM_CHANGED = 0x0021,
    SH_EVENT_HEALTH_OK = 0x0030,
    SH_EVENT_HEALTH_WARNING = 0x0031,
    SH_EVENT_HEALTH_CRITICAL = 0x0032,
    SH_EVENT_ALERT_RAISED = 0x0040,
    SH_EVENT_ALERT_RESOLVED = 0x0041,
};

// -----------------------------------------------------------------------------
// СОБЫТИЯ ЯДРА (0x0100-0x01FF)
// -----------------------------------------------------------------------------
enum CoreEvents : int32_t {
    SH_EVENT_CMD_EXECUTE = 0x0100,
    SH_EVENT_CMD_RESPONSE = 0x0101,
    SH_EVENT_CMD_ERROR = 0x0102,
    SH_EVENT_NET_CONNECTED = 0x0150,
    SH_EVENT_NET_DISCONNECTED = 0x0151,
    SH_EVENT_NET_IP_ASSIGNED = 0x0152,
    SH_EVENT_FS_MOUNTED = 0x0160,
    SH_EVENT_FS_FULL = 0x0161,
    SH_EVENT_FS_ERROR = 0x0162,
};

// -----------------------------------------------------------------------------
// СОБЫТИЯ УСТРОЙСТВА (0xF000-0xFFFF) — для кастомных событий бизнес-логики
// -----------------------------------------------------------------------------
constexpr int32_t EVENT_TYPE_DEVICE_BASE = 0xF000;

// -----------------------------------------------------------------------------
// СТРУКТУРА ДАННЫХ СОБЫТИЯ
// -----------------------------------------------------------------------------
struct ShEventData {
    uint32_t sourceModule = 0;
    uint32_t targetModule = 0;  // 0 = всем подписчикам
    int32_t command = 0;
    char payload[PAYLOAD_MAX_LEN] = {0};
    uint8_t payloadLen = 0;
    int32_t value = 0;
    uint32_t timestamp = 0;

    void clear() {
        sourceModule = 0;
        targetModule = 0;
        command = 0;
        memset(payload, 0, PAYLOAD_MAX_LEN);
        payloadLen = 0;
        value = 0;
        timestamp = 0;
    }
};

// -----------------------------------------------------------------------------
// ИНТЕРФЕЙС КАСТОМНОГО PAYLOAD
// -----------------------------------------------------------------------------
class ICustomEventPayload {
public:
    virtual ~ICustomEventPayload() = default;
    virtual void toJson(JsonDocument& doc) const = 0;
    virtual size_t toBinary(uint8_t* buffer, size_t maxSize) const = 0;
    virtual int32_t getTypeId() const = 0;
    virtual uint32_t getSourceModule() const = 0;
};

// -----------------------------------------------------------------------------
// ПРОТОТИПЫ ФУНКЦИЙ СОБЫТИЙНОЙ ШИНЫ
// -----------------------------------------------------------------------------
ESP_EVENT_DECLARE_BASE(SH_SYS_EVENTS);
ESP_EVENT_DECLARE_BASE(SH_APP_EVENTS);

// -----------------------------------------------------------------------------
// ИНТЕРФЕЙС МОДУЛЯ
// -----------------------------------------------------------------------------
class IModule {
public:
    // Жизненный цикл
    virtual void init() = 0;
    virtual void init(const ShEventData* config) { init(); }
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void tick() = 0;

    // Событийная шина
    virtual void onEvent(int32_t eventId, const ShEventData* data) = 0;

    // Информация о модуле
    virtual uint32_t getModuleId() const = 0;
    virtual const char* getModuleName() const = 0;
    virtual const char* getVersion() const = 0;

    // Статус (thread-safe через передачу буфера)
    virtual void getStatus(char* buffer, size_t bufferSize) const = 0;

    // Потокобезопасность
    virtual bool isReady() const = 0;
    virtual bool isHealthy() const = 0;

    // Приоритет и интервал tick()
    virtual uint8_t getPriority() const { return 128; }
    virtual uint32_t getTickIntervalMs() const { return 1000; }

    // Виртуальный деструктор
    virtual ~IModule() = default;

protected:
    // Отправка событий (через EventBus, а не напрямую в AppCore)
    void postEvent(int32_t eventId, const ShEventData* data) const;
    void postEventFromISR(int32_t eventId, const ShEventData* data,
                          BaseType_t* higherPriorityTaskWoken) const;

    // Логирование через событийную шину (targetModule = 0 — всем подписчикам)
    void logEvent(LogLevel level, const char* tag, const char* msg) const;

    // Безопасное копирование строк
    static bool safeStrCopy(char* dest, size_t destSize, const char* src);
    static bool safeStrCat(char* dest, size_t destSize, const char* src);

    // Constant-time сравнение (для безопасности)
    static bool secureCompare(const uint8_t* a, const uint8_t* b, size_t len);

    // ISR-безопасность
    static bool isInISR() { return xPortInIsrContext(); }

private:
    // Callback для отправки событий (устанавливается AppCore при регистрации)
    static std::function<bool(int32_t, const ShEventData*, bool)> _eventPoster;
    static std::function<bool(int32_t, const ShEventData*, BaseType_t*)> _eventPosterISR;

    friend class AppCore;
};

// -----------------------------------------------------------------------------
// УРОВНИ ЛОГИРОВАНИЯ
// -----------------------------------------------------------------------------
enum LogLevel : uint8_t {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3,
    LOG_CRITICAL = 4
};