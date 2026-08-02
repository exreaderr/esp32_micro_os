// ============================================================================
// ModuleBase.cpp — реализация базового класса модуля
// ============================================================================
#include "ModuleBase.h"
#include "Events.h"

// ============================================================================
// ЛОГИРОВАНИЕ
// ============================================================================
// Приёмник — файловая статика (урок IRAM: никаких статических локалов с
// guard-переменными на горячих путях). Устанавливается LogService'ом.
// ============================================================================
static ModuleBase::LogSink s_logSink = nullptr;

void ModuleBase::setLogSink(LogSink sink) { s_logSink = sink; }

void ModuleBase::log(LogLevel level, const char* fmt, ...) const {
    // Буквенный код уровня: D/I/W/E/C
    static const char LEVEL_CHARS[] = { 'D', 'I', 'W', 'E', 'C' };
    char lvl = LEVEL_CHARS[(uint8_t)level <= 4 ? (uint8_t)level : 1];

    // Печать через один фиксированный буфер на стеке — без String/heap.
    // 192 байта достаточно для любой разумной строки; длинное обрезается.
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    Serial.printf("[%08lu] [%c] [%s] %s\n",
                  (unsigned long)millis(), lvl, getName(), body);

    // Фаза 3: дубль в приёмник (LogService: RAM-кольцо -> файл в tick).
    // Приёмник вызывается ПОСЛЕ Serial — даже если он упадёт/заблокируется,
    // консольный след уже остался.
    if (s_logSink != nullptr) {
        s_logSink(level, getName(), body);
    }
}

// ============================================================================
// СТРОКИ
// ============================================================================
void ModuleBase::safeStrCopy(char* dst, size_t dstSize, const char* src) {
    if (dst == nullptr || dstSize == 0) return;
    size_t i = 0;
    for (; src && src[i] && i < dstSize - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// ============================================================================
// СОБЫТИЯ
// ============================================================================
void ModuleBase::postEvent(int32_t eventId, ShEventData* data) {
    ShEventData local;
    if (data == nullptr) {
        local.clear();
        data = &local;
    }
    // Модуль не обязан заполнять отправителя — подставляем сами.
    data->sourceModule = getModuleId();
    data->timestampMs  = millis();
    EventBus::getInstance().post(eventId, data);
}

void ModuleBase::publishError(const char* errorCode) {
    ShEventData d;
    d.clear();
    safeStrCopy(d.payload, sizeof(d.payload), errorCode);
    // Модульная ошибка — событие уровня "модуль сменил состояние" с кодом 4
    d.code = 4;   // 4 = error (см. SH_EVENT_MODULE_STATUS в Events.h)
    postEvent(SH_EVENT_MODULE_STATUS, &d);
    log(LogLevel::Error, "module error: %s", errorCode);
}

// ============================================================================
// МЬЮТЕКС
// ============================================================================
bool ModuleBase::ensureMutex() {
    if (_mutex == nullptr) {
        // Рекурсивный: модуль может брать мьютекс вложенно из своего же
        // контекста (например, log() внутри секции) — обычный мьютекс бы
        // завис. Таймаут всё равно контролирует takeMutex().
        _mutex = xSemaphoreCreateRecursiveMutex();
        if (_mutex == nullptr) {
            Serial.printf("[CORE] CRITICAL: mutex alloc failed for %s\n", getName());
            return false;
        }
    }
    return true;
}

bool ModuleBase::takeMutex(TickType_t timeoutMs) const {
    if (_mutex == nullptr) return false;
    // ESP-IDF 5.x (Arduino core 3.x): суффикс "...Mutex" у рекурсивных API
    // удалён — используем xSemaphoreTakeRecursive/GiveRecursive.
    return xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void ModuleBase::giveMutex() const {
    if (_mutex != nullptr) xSemaphoreGiveRecursive(_mutex);
}
