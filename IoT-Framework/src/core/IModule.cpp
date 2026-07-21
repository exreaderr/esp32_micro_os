// ============================================================================
// IModule.cpp - ULTIMATE MICRO-OS V4.2.1 (EVENT-DRIVEN) - PRODUCTION
// ============================================================================
// Описание: Реализация методов базового интерфейса модулей.
// Вынесена в .cpp для ускорения компиляции и экономии Flash-памяти.
//
// ИЗМЕНЕНИЯ v4.2.1:
// - Убрана зависимость от глобальной переменной Core
// - Используется синглтон AppCore::getInstance()
// - Добавлена защита от отправки событий до инициализации ядра
//
// ИЗМЕНЕНИЯ v4.2.2 (АУДИТ):
// - Добавлена защита от отправки из ISR
// - Исправлена безопасная работа с payload (strnlen вместо strlen)
// - SH_EVENT_MODULE_TICK заменен на SH_EVENT_MODULE_STATUS для sendStatus
// - Добавлена проверка на валидность данных перед отправкой
// ============================================================================
#include "IModule.h"
#include "core/AppCore.h"

// ============================================================================
// 1. ВСПОМОГАТЕЛЬНАЯ ФУНКЦИЯ БЕЗОПАСНОГО КОПИРОВАНИЯ
// ============================================================================
/**
 * @brief Безопасное копирование строки в буфер с ограничением
 * @param dest Буфер назначения
 * @param destSize Размер буфера
 * @param src Исходная строка (может быть nullptr)
 * @param outLen Указатель для сохранения длины (может быть nullptr)
 * @return true если копирование выполнено успешно
 */
static inline bool safeCopyPayload(char* dest, size_t destSize, const char* src, size_t* outLen = nullptr) {
    if (!dest || destSize == 0) return false;

    if (src == nullptr) {
        dest[0] = '\0';
        if (outLen) *outLen = 0;
        return true;
    }

    // Используем strnlen для безопасного определения длины
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
    if (outLen) *outLen = len;
    return true;
}

// ============================================================================
// 2. ОТПРАВКА СОБЫТИЙ (С ЗАЩИТОЙ ОТ ISR)
// ============================================================================
void IModule::postEvent(int32_t eventId, const ShEventData* data) const {
    // Защита: проверяем, не в ISR ли мы
    if (xPortInIsrContext()) {
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        AppCore::getInstance().postEventFromISR(eventId, data, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
        return;
    }

    // Защита: если ядро не готово, не отправляем
    if (!AppCore::getInstance().isReady()) {
        return;
    }

    // Валидация данных (если data не nullptr)
    if (data != nullptr) {
        // Проверяем, что payloadLen не превышает размер буфера
        if (data->payloadLen >= sizeof(data->payload)) {
            // Логируем ошибку через ядро
            AppCore::getInstance().logError("IModule", "Payload too large: %zu bytes", data->payloadLen);
            return;
        }
    }

    AppCore::getInstance().postEvent(eventId, data);
}

// ============================================================================
// 3. ОТПРАВКА КОМАНД (С ЗАЩИТОЙ ОТ nullptr И ISR)
// ============================================================================
void IModule::sendCommand(uint32_t targetModuleId, uint32_t command,
                         int32_t value, const char* payload) const {
    // Защита: если ядро не готово, не отправляем
    if (!AppCore::getInstance().isReady()) {
        return;
    }

    // Защита от ISR
    if (xPortInIsrContext()) {
        // Создаем структуру в стеке ISR (можно, но с осторожностью)
        ShEventData cmd;
        memset(&cmd, 0, sizeof(ShEventData));
        cmd.sourceModule = _moduleId;
        cmd.targetModule = targetModuleId;
        cmd.command = command;
        cmd.value = value;

        // Безопасное копирование
        safeCopyPayload(cmd.payload, sizeof(cmd.payload), payload, &cmd.payloadLen);

        // Отправляем через ISR-безопасный метод
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        AppCore::getInstance().postEventFromISR(SH_EVENT_CMD_EXECUTE, &cmd, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
        return;
    }

    // Полная очистка структуры
    ShEventData cmd;
    memset(&cmd, 0, sizeof(ShEventData));
    cmd.sourceModule = _moduleId;
    cmd.targetModule = targetModuleId;
    cmd.command = command;
    cmd.value = value;

    // БЕЗОПАСНОЕ КОПИРОВАНИЕ СТРОКИ
    safeCopyPayload(cmd.payload, sizeof(cmd.payload), payload, &cmd.payloadLen);

    // Отправка через ядро
    AppCore::getInstance().postEvent(SH_EVENT_CMD_EXECUTE, &cmd);
}

// ============================================================================
// 4. ОТПРАВКА ОТВЕТА
// ============================================================================
void IModule::sendResponse(uint32_t targetModuleId, uint32_t command,
                          int32_t value, const char* payload) const {
    // Автоматически добавляем бит RESPONSE (0x8000)
    // Это позволяет модулям отличать команды от ответов
    sendCommand(targetModuleId, command | 0x8000, value, payload);
}

// ============================================================================
// 5. ОТПРАВКА СТАТУСА
// ============================================================================
void IModule::sendStatus(const char* status) const {
    // Защита: если ядро не готово, не отправляем
    if (!AppCore::getInstance().isReady()) {
        return;
    }

    // Защита от ISR
    if (xPortInIsrContext()) {
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = _moduleId;
        data.targetModule = 0; // Всем модулям системы
        data.command = CMD_GET_STATUS;
        data.value = isReady() ? 1 : 0;

        // БЕЗОПАСНАЯ ПРОВЕРКА НА NULLPTR!
        if (status != nullptr) {
            safeCopyPayload(data.payload, sizeof(data.payload), status, &data.payloadLen);
        } else {
            // Если статус не передан, отправляем "OK"
            strncpy(data.payload, "OK", sizeof(data.payload) - 1);
            data.payload[sizeof(data.payload) - 1] = '\0';
            data.payloadLen = 2;
        }

        BaseType_t higherPriorityTaskWoken = pdFALSE;
        AppCore::getInstance().postEventFromISR(SH_EVENT_MODULE_STATUS, &data, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
        return;
    }

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0; // Всем модулям системы
    data.command = CMD_GET_STATUS;
    data.value = isReady() ? 1 : 0;

    // БЕЗОПАСНАЯ ПРОВЕРКА НА NULLPTR!
    if (status != nullptr) {
        safeCopyPayload(data.payload, sizeof(data.payload), status, &data.payloadLen);
    } else {
        // Если статус не передан, отправляем "OK"
        strncpy(data.payload, "OK", sizeof(data.payload) - 1);
        data.payload[sizeof(data.payload) - 1] = '\0';
        data.payloadLen = 2;
    }

    // Используем СПЕЦИАЛЬНОЕ СОБЫТИЕ для статуса (не SH_EVENT_MODULE_TICK!)
    AppCore::getInstance().postEvent(SH_EVENT_MODULE_STATUS, &data);
}

// ============================================================================
// 6. ОТПРАВКА ОШИБКИ (НОВЫЙ МЕТОД)
// ============================================================================
void IModule::sendError(uint32_t errorCode, const char* errorMessage) const {
    if (!AppCore::getInstance().isReady()) return;

    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = errorCode;
    data.value = errorCode;

    safeCopyPayload(data.payload, sizeof(data.payload), errorMessage, &data.payloadLen);

    AppCore::getInstance().postEvent(SH_EVENT_MODULE_ERROR, &data);
}