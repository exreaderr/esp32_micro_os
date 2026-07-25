// ============================================================================
// WiegandManager.cpp - ULTIMATE MICRO-OS V5.0 (FULLY INTEGRATED)
// ============================================================================
// Описание: Полноценный менеджер Wiegand-считывателя с поддержкой:
// - 26-bit, 34-bit, 37-bit форматов
// - Проверка четности
// - ISR-обработка с критическими секциями
// - Web-режим для чтения карт
// - Статистика чтений
// - Защита от дребезга и переполнения буфера
// - Полная потокобезопасность
// - Публикация событий через новую шину (v5.0)
//
// ИЗМЕНЕНИЯ v5.0:
// - Сохранена 100% функциональность v4.2.2
// - Добавлен метод handleCardData() для публикации события EVENT_CARD_READ
// - Добавлен счетчик _totalEventsPublished
// - Расширена диагностика
// ============================================================================
#include "WiegandManager.h"
#include "core/AppCore.h"  // НОВОЕ: для публикации событий
#include <esp_task_wdt.h>
#include "core/IModule.h"
#include <cstdarg>
#include <cstring>

// ============================================================================
// СТАТИЧЕСКИЙ ЭКЗЕМПЛЯР (СИНГЛТОН)
// ============================================================================
static WiegandManager _wiegandManagerInstance;

// ============================================================================
// СТАТИЧЕСКИЕ ПЕРЕМЕННЫЕ
// ============================================================================
portMUX_TYPE WiegandManager::_isrMux = portMUX_INITIALIZER_UNLOCKED;
volatile WiegandManager::ISR_Buffer WiegandManager::_isrBuf = {0, 0, 0, false, 0};
WiegandManager* WiegandManager::_instance = nullptr;

// ============================================================================
// 1. КОНСТРУКТОР / ДЕСТРУКТОР
// ============================================================================
WiegandManager::WiegandManager() {
    _moduleId = MODULE_ID_WIEGAND;

    _wiegandMutex = xSemaphoreCreateRecursiveMutex();

    _minBits = 24;
    _maxBits = 64;
    _stats.minBitCount = 255;
    _statsUpdateCounter = 0;
    _enabled = false;
    _initialized = false;
    _cardAvailable = false;
    _ignoreParity = false;
    _timeoutMs = 100;
    _debounceUs = 10;
    _expectedType = WiegandDeviceType::READER_26BIT;
    _lastTickMs = 0;
    _webReadMode = false;
    _webReadTimeoutMs = 10000;
    _webReadStartMs = 0;
    _beginInProgress = false;
    _totalEventsPublished = 0; // НОВОЕ

    _onCardRead = nullptr;
    _onCardReadSimple = nullptr;
    _onStatsUpdate = nullptr;
    _onError = nullptr;
    _onWebCardRead = nullptr;

    _instance = this;

    if (_wiegandMutex == nullptr) {
        Serial.println("[WIEGAND] CRITICAL: Failed to create mutex!");
    }

    portENTER_CRITICAL(&_isrMux);
    _isrBuf.rawData = 0;
    _isrBuf.bitCount = 0;
    _isrBuf.lastBitTime = 0;
    _isrBuf.overflow = false;
    _isrBuf.debounceCounter = 0;
    portEXIT_CRITICAL(&_isrMux);

    Serial.println("[WIEGAND] Instance created (v5.0)");
}

WiegandManager::~WiegandManager() {
    stop();
    if (_wiegandMutex != nullptr) {
        vSemaphoreDelete(_wiegandMutex);
        _wiegandMutex = nullptr;
    }
    _instance = nullptr;
}

// ============================================================================
// 2. СИНГЛТОН
// ============================================================================
WiegandManager& WiegandManager::getInstance() {
    return _wiegandManagerInstance;
}

// ============================================================================
// 3. ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================
void WiegandManager::safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

void WiegandManager::logMessage(const char* msg) {
    if (msg == nullptr) return;
    Serial.printf("[WIEGAND] %s\n", msg);

    if (_initialized) {
        ShEventData data;
        memset(&data, 0, sizeof(ShEventData));
        data.sourceModule = _moduleId;
        data.targetModule = MODULE_ID_LOG;
        data.command = SH_EVENT_LOG_ENTRY;
        data.value = 0;
        safeStrCopy(data.payload, sizeof(data.payload), msg);
        data.payloadLen = strlen(data.payload);
        postEvent(SH_EVENT_CMD_EXECUTE, &data);
    }
}

void WiegandManager::logMessage(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(buffer);
}

bool WiegandManager::isInitializedAndEnabled() const {
    return _initialized && _enabled;
}

// ============================================================================
// 4. ЖИЗНЕННЫЙ ЦИКЛ (IModule)
// ============================================================================
void WiegandManager::init() {
    logMessage("Init pending - call begin() with parameters");
}

void WiegandManager::start() {
    if (_initialized && _enabled) {
        logMessage("Started");
    }
}

void WiegandManager::stop() {
    if (_d0Pin != 255) {
        detachInterrupt(digitalPinToInterrupt(_d0Pin));
    }
    if (_d1Pin != 255) {
        detachInterrupt(digitalPinToInterrupt(_d1Pin));
    }

    portENTER_CRITICAL(&_isrMux);
    _isrBuf.rawData = 0;
    _isrBuf.bitCount = 0;
    _isrBuf.lastBitTime = 0;
    _isrBuf.overflow = false;
    _isrBuf.debounceCounter = 0;
    portEXIT_CRITICAL(&_isrMux);

    _enabled = false;
    _initialized = false;
    _cardAvailable = false;
    _beginInProgress = false;
    logMessage("Stopped");
}

void WiegandManager::tick() {
    if (!isInitializedAndEnabled()) return;

    esp_task_wdt_reset();

    uint32_t now = millis();
    _lastTickMs = now;

    if (_webReadMode && (now - _webReadStartMs > _webReadTimeoutMs)) {
        _webReadMode = false;
        logMessage("Web read mode timeout");
        publishErrorEvent("WEB_READ_TIMEOUT");
    }

    uint32_t lastBitTime;
    uint8_t bitCount;
    bool overflow;

    portENTER_CRITICAL(&_isrMux);
    lastBitTime = _isrBuf.lastBitTime;
    bitCount = _isrBuf.bitCount;
    overflow = _isrBuf.overflow;
    portEXIT_CRITICAL(&_isrMux);

    if (overflow) {
        logMessage("ISR buffer overflow! Resetting...");
        portENTER_CRITICAL(&_isrMux);
        _isrBuf.rawData = 0;
        _isrBuf.bitCount = 0;
        _isrBuf.lastBitTime = 0;
        _isrBuf.overflow = false;
        _isrBuf.debounceCounter = 0;
        portEXIT_CRITICAL(&_isrMux);
        _stats.bufferOverflows++;
        publishErrorEvent("BUFFER_OVERFLOW");
        return;
    }

    if (bitCount > 0 && (now - lastBitTime > _timeoutMs)) {
        uint64_t rawData;
        portENTER_CRITICAL(&_isrMux);
        rawData = _isrBuf.rawData;
        _isrBuf.rawData = 0;
        _isrBuf.bitCount = 0;
        _isrBuf.lastBitTime = 0;
        _isrBuf.debounceCounter = 0;
        portEXIT_CRITICAL(&_isrMux);

        processCard(rawData, bitCount);
    }

    if (++_statsUpdateCounter >= STATS_UPDATE_INTERVAL) {
        _statsUpdateCounter = 0;
        if (_onStatsUpdate) _onStatsUpdate(_stats);
        publishStatsEvent();
    }
}

// ============================================================================
// 5. ОБРАБОТКА СОБЫТИЙ
// ============================================================================
void WiegandManager::eventHandler(void* handlerArgs, esp_event_base_t base,
                                  int32_t id, void* eventData) {
    WiegandManager* instance = static_cast<WiegandManager*>(handlerArgs);
    if (!instance) return;

    if (base == SH_SYS_EVENTS) {
        switch (id) {
            case SH_EVENT_SYS_RESTART:
            case SH_EVENT_SYS_SHUTDOWN:
                instance->stop();
                break;
            default:
                break;
        }
        return;
    }

    if (base == SH_APP_EVENTS) {
        instance->onEvent(id, static_cast<ShEventData*>(eventData));
    }
}

void WiegandManager::onEvent(int32_t eventId, const ShEventData* data) {
    if (!data) return;

    switch (eventId) {
        case SH_EVENT_CMD_EXECUTE:
            if (data->targetModule == _moduleId || data->targetModule == 0) {
                handleCommand(data);
            }
            break;
        case SH_EVENT_SYS_RESTART:
        case SH_EVENT_SYS_SHUTDOWN:
            stop();
            break;
        default:
            break;
    }
}

bool WiegandManager::canHandleEvent(int32_t eventId) const {
    return (eventId == SH_EVENT_CMD_EXECUTE ||
            eventId == SH_EVENT_SYS_RESTART ||
            eventId == SH_EVENT_SYS_SHUTDOWN);
}

// ============================================================================
// 6. СТАТУС
// ============================================================================
const char* WiegandManager::getStatus() const {
    static char statusBuffer[128];
    const char* state = _enabled ? "ACTIVE" : "DISABLED";
    snprintf(statusBuffer, sizeof(statusBuffer),
            "State: %s, Reads: %lu, Valid: %lu, Web: %s, Overflow: %lu",
            state,
            _stats.totalReads,
            _stats.validReads,
            _webReadMode ? "ON" : "OFF",
            _stats.bufferOverflows);
    return statusBuffer;
}

// ============================================================================
// 7. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void WiegandManager::getDiagnostics(ShEventData* data) const {
    if (!data) return;

    data->sourceModule = _moduleId;
    data->value = _stats.totalReads;

    snprintf(data->payload, sizeof(data->payload),
            "total:%lu,valid:%lu,invalid:%lu,parity:%lu,min:%lu,max:%lu,ovf:%lu,evt:%lu",
            _stats.totalReads,
            _stats.validReads,
            _stats.invalidReads,
            _stats.parityErrors,
            _stats.minBitCount,
            _stats.maxBitCount,
            _stats.bufferOverflows,
            _totalEventsPublished);
    data->payloadLen = strlen(data->payload);
}

// ============================================================================
// 8. ОБРАБОТКА КОМАНД
// ============================================================================
void WiegandManager::handleCommand(const ShEventData* data) {
    switch (data->command) {
        case CMD_GET_STATUS: {
            ShEventData response;
            memset(&response, 0, sizeof(ShEventData));
            response.sourceModule = _moduleId;
            response.targetModule = data->sourceModule;
            response.command = CMD_RESPONSE_DATA;
            response.value = _stats.totalReads;
            const char* status = getStatus();
            safeStrCopy(response.payload, sizeof(response.payload), status);
            response.payloadLen = strlen(response.payload);
            postEvent(SH_EVENT_CMD_RESPONSE, &response);
            break;
        }

        case 0x0500: { // GET_CARD
            if (hasCard()) {
                WiegandData card = getCardData();
                ShEventData response;
                memset(&response, 0, sizeof(ShEventData));
                response.sourceModule = _moduleId;
                response.targetModule = data->sourceModule;
                response.command = 0x0501;
                response.value = card.cardNumber;
                snprintf(response.payload, sizeof(response.payload),
                        "card:%lu,fc:%lu,bits:%d,valid:%d",
                        card.cardNumber,
                        card.facilityCode,
                        card.bitCount,
                        card.isValid ? 1 : 0);
                response.payloadLen = strlen(response.payload);
                postEvent(SH_EVENT_CMD_RESPONSE, &response);
            }
            break;
        }

        case 0x0502: // ENABLE
            enable();
            break;

        case 0x0503: // DISABLE
            disable();
            break;

        case 0x0504: // WEB_READ_START
            startWebReadMode(data->value > 0 ? data->value : 10000);
            break;

        case 0x0505: // WEB_READ_STOP
            stopWebReadMode();
            break;

        case 0x0506: // RESET_STATS
            resetStats();
            break;

        default:
            logMessage("Unknown command: 0x%X", data->command);
            break;
    }
}

// ============================================================================
// 9. ОТПРАВКА СОБЫТИЙ
// ============================================================================
void WiegandManager::publishCardEvent(const WiegandData& data) {
    CardReadEvent event;
    event.cardNumber = data.cardNumber;
    event.facilityCode = data.facilityCode;
    event.rawData = data.rawData;
    event.bitCount = data.bitCount;
    event.timestamp = data.timestamp;
    event.deviceType = (uint8_t)data.deviceType;
    event.parityType = (uint8_t)data.parityType;
    event.isValid = data.isValid;
    event.parityOk = data.parityOk;

    int32_t eventId = data.isValid ? SH_EVENT_CARD_VALID : SH_EVENT_CARD_INVALID;
    if (!data.parityOk && data.isValid) {
        eventId = SH_EVENT_CARD_PARITY_ERROR;
    }

    ShEventData shData;
    memset(&shData, 0, sizeof(ShEventData));
    shData.sourceModule = _moduleId;
    shData.targetModule = 0;
    shData.command = eventId;
    shData.value = data.cardNumber;
    memcpy(shData.payload, &event, min(sizeof(CardReadEvent), sizeof(shData.payload)));
    shData.payloadLen = sizeof(CardReadEvent);
    postEvent(SH_EVENT_MODULE_TICK, &shData);
}

// ============================================================================
// НОВЫЙ МЕТОД: ПУБЛИКАЦИЯ СОБЫТИЯ ЧЕРЕЗ НОВУЮ ШИНУ (v5.0)
// ============================================================================
void WiegandManager::handleCardData(const WiegandData& data) {
    // Формируем событие для новой событийной шины
    ShEventData event;
    memset(&event, 0, sizeof(ShEventData));
    event.type = EVENT_CARD_READ; // Константа из ShEventData.h
    event.senderId = _moduleId;
    event.targetModuleId = 0xFF; // Всем

    // Заполняем payload
    event.payload.intData.val1 = data.cardNumber;
    event.payload.intData.val2 = data.facilityCode;

    // Формируем строку с дополнительной информацией
    snprintf(event.payload.statusStr, sizeof(event.payload.statusStr),
            "bits:%d,valid:%d,parity:%d,type:%d",
            data.bitCount,
            data.isValid ? 1 : 0,
            data.parityOk ? 1 : 0,
            (int)data.deviceType);

    // Отправляем в шину через AppCore
    AppCore::getInstance().publishEvent(event);
    _totalEventsPublished++;
}

void WiegandManager::publishStatsEvent() {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_WIEGAND_STATS;
    data.value = _stats.totalReads;
    snprintf(data.payload, sizeof(data.payload),
            "total:%lu,valid:%lu,invalid:%lu,parity:%lu,ovf:%lu",
            _stats.totalReads,
            _stats.validReads,
            _stats.invalidReads,
            _stats.parityErrors,
            _stats.bufferOverflows);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

void WiegandManager::publishErrorEvent(const char* errorCode) {
    ShEventData data;
    memset(&data, 0, sizeof(ShEventData));
    data.sourceModule = _moduleId;
    data.targetModule = 0;
    data.command = SH_EVENT_WIEGAND_ERROR;
    data.value = _stats.parityErrors;
    safeStrCopy(data.payload, sizeof(data.payload), errorCode);
    data.payloadLen = strlen(data.payload);
    postEvent(SH_EVENT_MODULE_TICK, &data);
}

// ============================================================================
// 10. ИНИЦИАЛИЗАЦИЯ
// ============================================================================
void WiegandManager::begin(uint8_t d0Pin, uint8_t d1Pin,
                           uint32_t timeoutMs,
                           WiegandDeviceType expectedType) {
    if (_beginInProgress) {
        logMessage("Begin already in progress, skipping...");
        return;
    }
    _beginInProgress = true;

    if (_initialized) {
        logMessage("Already initialized, stopping first...");
        stop();
    }

    if (_wiegandMutex == nullptr) {
        logMessage("Mutex is null!");
        _beginInProgress = false;
        return;
    }

    if (xSemaphoreTakeRecursive(_wiegandMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        logMessage("Mutex timeout in begin!");
        _beginInProgress = false;
        return;
    }

    _d0Pin = d0Pin;
    _d1Pin = d1Pin;
    _timeoutMs = timeoutMs;
    _expectedType = expectedType;

    pinMode(_d0Pin, INPUT_PULLUP);
    pinMode(_d1Pin, INPUT_PULLUP);

    portENTER_CRITICAL(&_isrMux);
    _isrBuf.rawData = 0;
    _isrBuf.bitCount = 0;
    _isrBuf.lastBitTime = 0;
    _isrBuf.overflow = false;
    _isrBuf.debounceCounter = 0;
    portEXIT_CRITICAL(&_isrMux);

    _d0InterruptNum = digitalPinToInterrupt(_d0Pin);
    _d1InterruptNum = digitalPinToInterrupt(_d1Pin);

    attachInterrupt(_d0InterruptNum, handleD0_ISR, FALLING);
    attachInterrupt(_d1InterruptNum, handleD1_ISR, FALLING);

    _cardAvailable = false;
    _enabled = true;
    _initialized = true;
    resetStats();

    xSemaphoreGiveRecursive(_wiegandMutex);

    esp_event_handler_instance_register(
        SH_SYS_EVENTS,
        ESP_EVENT_ANY_ID,
        &WiegandManager::eventHandler,
        this,
        NULL
    );
    esp_event_handler_instance_register(
        SH_APP_EVENTS,
        ESP_EVENT_ANY_ID,
        &WiegandManager::eventHandler,
        this,
        NULL
    );

    logMessage("Initialized: D0=%d, D1=%d, timeout=%lu ms",
              _d0Pin, _d1Pin, _timeoutMs);

    _beginInProgress = false;
}

void WiegandManager::end() {
    stop();
}

void WiegandManager::reset() {
    if (_wiegandMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_wiegandMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        _cardAvailable = false;
        memset(&_lastCardData, 0, sizeof(_lastCardData));

        portENTER_CRITICAL(&_isrMux);
        _isrBuf.rawData = 0;
        _isrBuf.bitCount = 0;
        _isrBuf.lastBitTime = 0;
        _isrBuf.overflow = false;
        _isrBuf.debounceCounter = 0;
        portEXIT_CRITICAL(&_isrMux);

        xSemaphoreGiveRecursive(_wiegandMutex);
        logMessage("Reset complete");
    }
}

// ============================================================================
// 11. УПРАВЛЕНИЕ
// ============================================================================
void WiegandManager::enable() {
    if (!_initialized) return;
    _enabled = true;
    logMessage("Enabled");
}

void WiegandManager::disable() {
    _enabled = false;
    logMessage("Disabled");
}

void WiegandManager::resetBuffer() {
    portENTER_CRITICAL(&_isrMux);
    _isrBuf.rawData = 0;
    _isrBuf.bitCount = 0;
    _isrBuf.lastBitTime = 0;
    _isrBuf.overflow = false;
    _isrBuf.debounceCounter = 0;
    portEXIT_CRITICAL(&_isrMux);
    _cardAvailable = false;
    logMessage("Buffer reset");
}

void WiegandManager::resetStats() {
    if (_wiegandMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_wiegandMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(&_stats, 0, sizeof(_stats));
        _stats.minBitCount = 255;
        _statsUpdateCounter = 0;
        xSemaphoreGiveRecursive(_wiegandMutex);
        logMessage("Stats reset");
    }
}

// ============================================================================
// 12. ЧТЕНИЕ
// ============================================================================
WiegandData WiegandManager::getCardData() {
    WiegandData data;
    if (_wiegandMutex == nullptr) return data;

    if (xSemaphoreTakeRecursive(_wiegandMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        data = _lastCardData;
        _cardAvailable = false;
        xSemaphoreGiveRecursive(_wiegandMutex);
    }
    return data;
}

uint32_t WiegandManager::getCardNumber() {
    if (_wiegandMutex == nullptr) return 0;

    uint32_t number = 0;
    if (xSemaphoreTakeRecursive(_wiegandMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        number = _lastCardData.cardNumber;
        _cardAvailable = false;
        xSemaphoreGiveRecursive(_wiegandMutex);
    }
    return number;
}

// ============================================================================
// 13. WEB-РЕЖИМ
// ============================================================================
void WiegandManager::startWebReadMode(uint32_t timeoutMs) {
    if (_wiegandMutex == nullptr) return;

    if (xSemaphoreTakeRecursive(_wiegandMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _webReadMode = true;
        _webReadTimeoutMs = timeoutMs;
        _webReadStartMs = millis();
        xSemaphoreGiveRecursive(_wiegandMutex);
        logMessage("Web read mode started (timeout=%lu ms)", timeoutMs);
    }
}

void WiegandManager::stopWebReadMode() {
    _webReadMode = false;
    logMessage("Web read mode stopped");
}

// ============================================================================
// 14. ОБРАБОТКА КАРТЫ (ИЗМЕНЕНО: добавлен вызов handleCardData)
// ============================================================================
void WiegandManager::processCard(uint64_t rawData, uint8_t bitCount) {
    if (_wiegandMutex == nullptr) return;

    if (bitCount < _minBits || bitCount > _maxBits) {
        logMessage("Invalid bit count: %d (min=%d, max=%d)",
                  bitCount, _minBits, _maxBits);
        updateStats(false, false, bitCount);
        if (_onError) _onError("INVALID_BIT_COUNT");
        publishErrorEvent("INVALID_BIT_COUNT");
        return;
    }

    _lastCardData.rawData = rawData;
    _lastCardData.bitCount = bitCount;
    _lastCardData.timestamp = millis();
    _lastCardData.isValid = false;
    _lastCardData.parityOk = false;
    _lastCardData.facilityCode = 0;
    _lastCardData.cardNumber = 0;

    bool parityOk = false;
    bool decoded = false;

    if (bitCount == 26 && (_expectedType == WiegandDeviceType::READER_26BIT ||
                          _expectedType == WiegandDeviceType::UNKNOWN)) {
        _lastCardData.deviceType = WiegandDeviceType::READER_26BIT;
        _lastCardData.parityType = WiegandParity::PARITY_26BIT;
        parityOk = checkParity26(rawData);
        if (parityOk || _ignoreParity) {
            decode26Bit(rawData);
            _lastCardData.isValid = true;
            _lastCardData.parityOk = parityOk;
            decoded = true;
            logMessage("26-bit: ID=%lu, FC=%lu",
                      _lastCardData.cardNumber, _lastCardData.facilityCode);
        }
    }

    #ifdef WIEGAND_SUPPORT_34BIT
    else if (bitCount == 34 && (_expectedType == WiegandDeviceType::READER_34BIT ||
                               _expectedType == WiegandDeviceType::UNKNOWN)) {
        _lastCardData.deviceType = WiegandDeviceType::READER_34BIT;
        _lastCardData.parityType = WiegandParity::PARITY_34BIT;
        parityOk = checkParity34(rawData);
        if (parityOk || _ignoreParity) {
            decode34Bit(rawData);
            _lastCardData.isValid = true;
            _lastCardData.parityOk = parityOk;
            decoded = true;
            logMessage("34-bit: ID=%lu, FC=%lu",
                      _lastCardData.cardNumber, _lastCardData.facilityCode);
        }
    }
    #endif

    #ifdef WIEGAND_SUPPORT_37BIT
    else if (bitCount == 37 && (_expectedType == WiegandDeviceType::READER_37BIT ||
                               _expectedType == WiegandDeviceType::UNKNOWN)) {
        _lastCardData.deviceType = WiegandDeviceType::READER_37BIT;
        _lastCardData.parityType = WiegandParity::PARITY_37BIT;
        parityOk = checkParity37(rawData);
        if (parityOk || _ignoreParity) {
            decode37Bit(rawData);
            _lastCardData.isValid = true;
            _lastCardData.parityOk = parityOk;
            decoded = true;
            logMessage("37-bit: ID=%lu", _lastCardData.cardNumber);
        }
    }
    #endif

    else {
        logMessage("Unsupported bit count: %d", bitCount);
        updateStats(false, false, bitCount);
        if (_onError) _onError("UNSUPPORTED_FORMAT");
        publishErrorEvent("UNSUPPORTED_FORMAT");
        return;
    }

    if (decoded) {
        _cardAvailable = true;
        updateStats(true, parityOk, bitCount);

        // НОВОЕ: публикация через новую шину
        handleCardData(_lastCardData);

        if (_onCardRead) _onCardRead(_lastCardData);
        if (_onCardReadSimple) _onCardReadSimple(_lastCardData.cardNumber);

        if (_webReadMode) {
            if (_onWebCardRead) _onWebCardRead(_lastCardData.cardNumber);
            stopWebReadMode();
            logMessage("Web read: card %lu", _lastCardData.cardNumber);
        }

        publishCardEvent(_lastCardData);
    } else {
        updateStats(false, parityOk, bitCount);
        if (!parityOk && !_ignoreParity) {
            logMessage("Parity error on %d-bit card", bitCount);
            if (_onError) _onError("PARITY_ERROR");
            publishErrorEvent("PARITY_ERROR");
        }
    }
}

// ============================================================================
// 15. ДЕКОДИРОВАНИЕ (ВАШЕ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void WiegandManager::decode26Bit(uint64_t rawData) {
    _lastCardData.facilityCode = (uint32_t)((rawData >> 17) & 0xFF);
    _lastCardData.cardNumber = (uint32_t)((rawData >> 1) & 0xFFFF);
}

#ifdef WIEGAND_SUPPORT_34BIT
void WiegandManager::decode34Bit(uint64_t rawData) {
    _lastCardData.facilityCode = (uint32_t)((rawData >> 17) & 0xFFFF);
    _lastCardData.cardNumber = (uint32_t)((rawData >> 1) & 0xFFFF);
}
#endif

#ifdef WIEGAND_SUPPORT_37BIT
void WiegandManager::decode37Bit(uint64_t rawData) {
    _lastCardData.cardNumber = (uint32_t)((rawData >> 1) & 0xFFFFFFFF);
    _lastCardData.facilityCode = 0;
}
#endif

// ============================================================================
// 16. ПРОВЕРКА ЧЕТНОСТИ (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
bool WiegandManager::checkParity26(uint64_t rawData) {
    bool evenParity = false;
    bool oddParity = false;

    for (int i = 1; i <= 13; i++) {
        if (rawData & (1ULL << (25 - i))) evenParity = !evenParity;
    }
    for (int i = 14; i <= 25; i++) {
        if (rawData & (1ULL << (25 - i))) oddParity = !oddParity;
    }

    bool evenCheck = ((rawData >> 25) & 1) == (evenParity ? 0 : 1);
    bool oddCheck = ((rawData >> 0) & 1) == (oddParity ? 1 : 0);
    return evenCheck && oddCheck;
}

#ifdef WIEGAND_SUPPORT_34BIT
bool WiegandManager::checkParity34(uint64_t rawData) {
    bool evenParity = false;
    bool oddParity = false;

    for (int i = 1; i <= 17; i++) {
        if (rawData & (1ULL << (33 - i))) evenParity = !evenParity;
    }
    for (int i = 18; i <= 33; i++) {
        if (rawData & (1ULL << (33 - i))) oddParity = !oddParity;
    }

    bool evenCheck = ((rawData >> 33) & 1) == (evenParity ? 0 : 1);
    bool oddCheck = ((rawData >> 0) & 1) == (oddParity ? 1 : 0);
    return evenCheck && oddCheck;
}
#endif

#ifdef WIEGAND_SUPPORT_37BIT
bool WiegandManager::checkParity37(uint64_t rawData) {
    bool parity = false;
    uint32_t dataBits = (uint32_t)((rawData >> 1) & 0xFFFFFFFF);
    for (int i = 0; i < 36; i++) {
        if (dataBits & (1 << i)) parity = !parity;
    }
    return ((rawData >> 0) & 1) == (parity ? 1 : 0);
}
#endif

// ============================================================================
// 17. СТАТИСТИКА (ВАША, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void WiegandManager::updateStats(bool valid, bool parityOk, uint8_t bitCount) {
    _stats.totalReads++;
    _stats.isrCalls++;

    if (valid) {
        _stats.validReads++;
        if (bitCount < _stats.minBitCount) _stats.minBitCount = bitCount;
        if (bitCount > _stats.maxBitCount) _stats.maxBitCount = bitCount;
        _stats.lastReadTime = millis();
    } else {
        _stats.invalidReads++;
    }

    if (!parityOk) _stats.parityErrors++;
}

// ============================================================================
// 18. ISR-ОБРАБОТЧИКИ (ВАШИ, БЕЗ ИЗМЕНЕНИЙ)
// ============================================================================
void IRAM_ATTR WiegandManager::handleD0_ISR() {
    if (_instance == nullptr) return;
    if (!_instance->_enabled) return;

    uint32_t now = micros();
    uint32_t lastBitTime;

    portENTER_CRITICAL_ISR(&_isrMux);
    lastBitTime = _isrBuf.lastBitTime;

    if (now - lastBitTime < ISR_DEBOUNCE_US) {
        _isrBuf.debounceCounter++;
        portEXIT_CRITICAL_ISR(&_isrMux);
        return;
    }

    if (_isrBuf.bitCount >= MAX_BIT_COUNT) {
        _isrBuf.overflow = true;
        portEXIT_CRITICAL_ISR(&_isrMux);
        return;
    }

    _isrBuf.rawData = (_isrBuf.rawData << 1) | 0;
    _isrBuf.bitCount++;
    _isrBuf.lastBitTime = now;

    portEXIT_CRITICAL_ISR(&_isrMux);
}

void IRAM_ATTR WiegandManager::handleD1_ISR() {
    if (_instance == nullptr) return;
    if (!_instance->_enabled) return;

    uint32_t now = micros();
    uint32_t lastBitTime;

    portENTER_CRITICAL_ISR(&_isrMux);
    lastBitTime = _isrBuf.lastBitTime;

    if (now - lastBitTime < ISR_DEBOUNCE_US) {
        _isrBuf.debounceCounter++;
        portEXIT_CRITICAL_ISR(&_isrMux);
        return;
    }

    if (_isrBuf.bitCount >= MAX_BIT_COUNT) {
        _isrBuf.overflow = true;
        portEXIT_CRITICAL_ISR(&_isrMux);
        return;
    }

    _isrBuf.rawData = (_isrBuf.rawData << 1) | 1;
    _isrBuf.bitCount++;
    _isrBuf.lastBitTime = now;

    portEXIT_CRITICAL_ISR(&_isrMux);
}

// ============================================================================
// 19. ДИАГНОСТИКА (ИЗМЕНЕНО: добавлен _totalEventsPublished)
// ============================================================================
void WiegandManager::streamDiagnosticInfo(Stream& stream) const {
    stream.println("==============================");
    stream.println(" WIEGAND MANAGER DIAGNOSTIC");
    stream.println("==============================");
    stream.printf(" Version: %s\n", getVersion());
    stream.printf(" Initialized: %s\n", _initialized ? "YES" : "NO");
    stream.printf(" Enabled: %s\n", _enabled ? "YES" : "NO");
    stream.printf(" D0 Pin: %d (INT: %d)\n", _d0Pin, _d0InterruptNum);
    stream.printf(" D1 Pin: %d (INT: %d)\n", _d1Pin, _d1InterruptNum);
    stream.printf(" Timeout: %lu ms\n", _timeoutMs);
    stream.printf(" Debounce: %lu us\n", _debounceUs);
    stream.printf(" Expected Type: %d\n", (int)_expectedType);
    stream.printf(" Ignore Parity: %s\n", _ignoreParity ? "YES" : "NO");
    stream.printf(" Min Bits: %d\n", _minBits);
    stream.printf(" Max Bits: %d\n", _maxBits);
    stream.printf(" Web Mode: %s\n", _webReadMode ? "ON" : "OFF");
    stream.println("--- Stats ---");
    stream.printf(" Total Reads: %lu\n", _stats.totalReads);
    stream.printf(" Valid Reads: %lu\n", _stats.validReads);
    stream.printf(" Invalid Reads: %lu\n", _stats.invalidReads);
    stream.printf(" Parity Errors: %lu\n", _stats.parityErrors);
    stream.printf(" Buffer Overflows: %lu\n", _stats.bufferOverflows);
    stream.printf(" Debounce Rejects: %lu\n", _stats.debounceRejects);
    stream.printf(" ISR Calls: %lu\n", _stats.isrCalls);
    stream.printf(" Min Bit Count: %lu\n", _stats.minBitCount);
    stream.printf(" Max Bit Count: %lu\n", _stats.maxBitCount);
    stream.printf(" Last Read: %lu\n", _stats.lastReadTime);
    stream.printf(" Events Published: %lu\n", _totalEventsPublished); // НОВОЕ
    stream.println("==============================");
}

void WiegandManager::printStats() const {
    streamDiagnosticInfo(Serial);
}