// ============================================================================
// IModule.cpp — МикроОС v5.0
// ============================================================================
#include "IModule.h"

// Статические callback'и (устанавливаются AppCore)
std::function<bool(int32_t, const ShEventData*, bool)> IModule::_eventPoster;
std::function<bool(int32_t, const ShEventData*, BaseType_t*)> IModule::_eventPosterISR;

// -----------------------------------------------------------------------------
// ОТПРАВКА СОБЫТИЙ
// -----------------------------------------------------------------------------
void IModule::postEvent(int32_t eventId, const ShEventData* data) const {
    if (isInISR()) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        postEventFromISR(eventId, data, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        return;
    }

    if (_eventPoster) {
        ShEventData eventData = *data;
        eventData.sourceModule = getModuleId();
        eventData.timestamp = millis();
        _eventPoster(eventId, &eventData, false);
    }
}

void IModule::postEventFromISR(int32_t eventId, const ShEventData* data,
                                BaseType_t* higherPriorityTaskWoken) const {
    if (_eventPosterISR) {
        ShEventData eventData = *data;
        eventData.sourceModule = getModuleId();
        eventData.timestamp = millis();
        _eventPosterISR(eventId, &eventData, higherPriorityTaskWoken);
    }
}

// -----------------------------------------------------------------------------
// ЛОГИРОВАНИЕ ЧЕРЕЗ СОБЫТИЙНУЮ ШИНУ
// -----------------------------------------------------------------------------
void IModule::logEvent(LogLevel level, const char* tag, const char* msg) const {
    ShEventData data;
    data.clear();
    data.targetModule = 0;  // Всем подписчикам (включая LogManager)
    data.command = static_cast<int32_t>(level);
    data.value = static_cast<int32_t>(level);

    // Формат: [TAG] message
    char fullMsg[PAYLOAD_MAX_LEN];
    snprintf(fullMsg, sizeof(fullMsg), "[%s] %s", tag, msg);
    safeStrCopy(data.payload, sizeof(data.payload), fullMsg);
    data.payloadLen = strlen(data.payload);

    postEvent(SH_EVENT_LOG_ENTRY, &data);
}

// -----------------------------------------------------------------------------
// БЕЗОПАСНОЕ КОПИРОВАНИЕ СТРОК
// -----------------------------------------------------------------------------
bool IModule::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || !src || destSize == 0) return false;
    size_t srcLen = strlen(src);
    size_t copyLen = (srcLen < destSize - 1) ? srcLen : destSize - 1;
    memcpy(dest, src, copyLen);
    dest[copyLen] = '\0';
    return (srcLen < destSize);
}

bool IModule::safeStrCat(char* dest, size_t destSize, const char* src) {
    if (!dest || !src || destSize == 0) return false;
    size_t destLen = strlen(dest);
    if (destLen >= destSize - 1) return false;
    size_t remaining = destSize - destLen - 1;
    size_t srcLen = strlen(src);
    size_t copyLen = (srcLen < remaining) ? srcLen : remaining;
    memcpy(dest + destLen, src, copyLen);
    dest[destLen + copyLen] = '\0';
    return (srcLen < remaining);
}

// -----------------------------------------------------------------------------
// CONSTANT-TIME СРАВНЕНИЕ (защита от timing attack)
// -----------------------------------------------------------------------------
bool IModule::secureCompare(const uint8_t* a, const uint8_t* b, size_t len) {
    if (!a || !b || len == 0) return false;
    volatile uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= (a[i] ^ b[i]);
    }
    return (result == 0);
}