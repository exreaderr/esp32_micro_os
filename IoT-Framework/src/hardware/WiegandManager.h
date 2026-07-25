// ============================================================================
// WiegandManager.h - ULTIMATE MICRO-OS V5.0 (ADAPTED)
// ============================================================================
// Описание: Менеджер Wiegand-считывателя карт.
//           Адаптирован для новой событийной архитектуры МикроОС v5.0.
//
// ИЗМЕНЕНИЯ v5.0:
// - Сохранена 100% функциональность v4.2.2
// - Добавлена поддержка EVENT_CARD_READ
// - Добавлен метод handleCardData() для публикации события
// - Расширена диагностика
// - Улучшена потокобезопасность
// ============================================================================
#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

#include "core/IModule.h"
#include "core/ShEventData.h"

// ============================================================================
// 1. КОНСТАНТЫ
// ============================================================================
#define WIEGAND_SUPPORT_34BIT
#define WIEGAND_SUPPORT_37BIT

// ============================================================================
// 2. СОБЫТИЯ WIEGAND MANAGER (РАСШИРЕНЫ)
// ============================================================================
enum WiegandEvents : int32_t {
    SH_EVENT_CARD_READ = SH_EVENT_USER_BASE + 0x0500,
    SH_EVENT_CARD_VALID = SH_EVENT_USER_BASE + 0x0501,
    SH_EVENT_CARD_INVALID = SH_EVENT_USER_BASE + 0x0502,
    SH_EVENT_CARD_PARITY_ERROR = SH_EVENT_USER_BASE + 0x0503,
    SH_EVENT_WIEGAND_ERROR = SH_EVENT_USER_BASE + 0x0504,
    SH_EVENT_WIEGAND_STATS = SH_EVENT_USER_BASE + 0x0505
};

// ============================================================================
// 3. СТРУКТУРЫ ДАННЫХ (ВАШИ, СОХРАНЕНЫ)
// ============================================================================
struct CardReadEvent {
    uint32_t cardNumber;
    uint32_t facilityCode;
    uint64_t rawData;
    uint8_t bitCount;
    uint32_t timestamp;
    uint8_t deviceType;
    uint8_t parityType;
    bool isValid;
    bool parityOk;
};

enum class WiegandDeviceType : uint8_t {
    UNKNOWN = 0,
    READER_26BIT = 1,
    READER_34BIT = 2,
    READER_37BIT = 3,
    KEYBOARD = 4,
    CUSTOM = 6
};

enum class WiegandParity : uint8_t {
    NONE = 0,
    PARITY_26BIT = 1,
    PARITY_34BIT = 2,
    PARITY_37BIT = 3
};

struct WiegandData {
    uint32_t cardNumber = 0;
    uint32_t facilityCode = 0;
    uint64_t rawData = 0;
    uint8_t bitCount = 0;
    uint32_t timestamp = 0;
    WiegandDeviceType deviceType = WiegandDeviceType::UNKNOWN;
    WiegandParity parityType = WiegandParity::NONE;
    bool isValid = false;
    bool parityOk = false;
};

struct WiegandStats {
    uint32_t totalReads = 0;
    uint32_t validReads = 0;
    uint32_t invalidReads = 0;
    uint32_t parityErrors = 0;
    uint32_t timeoutErrors = 0;
    uint32_t lastReadTime = 0;
    uint32_t minBitCount = 255;
    uint32_t maxBitCount = 0;
    uint32_t bufferOverflows = 0;
    uint32_t isrCalls = 0;
    uint32_t debounceRejects = 0;
};

// ============================================================================
// 4. ОСНОВНОЙ КЛАСС (РАСШИРЕН)
// ============================================================================
class WiegandManager : public IModule {
public:
    // === КОЛБЭКИ (ВАШИ) ===
    typedef std::function<void(const WiegandData& data)> OnCardReadCallback;
    typedef std::function<void(uint32_t cardNumber)> OnCardReadSimple;
    typedef std::function<void(const WiegandStats& stats)> OnStatsUpdateCallback;
    typedef std::function<void(const char* errorCode)> OnErrorCallback;
    typedef std::function<void(uint32_t cardNumber)> OnWebCardReadCallback;

    // === СИНГЛТОН (ВАШ) ===
    static WiegandManager& getInstance();

    // === КОНСТРУКТОР / ДЕСТРУКТОР (ВАШ) ===
    WiegandManager();
    ~WiegandManager();

    WiegandManager(const WiegandManager&) = delete;
    WiegandManager& operator=(const WiegandManager&) = delete;

    // === IModule (ВАШ, ДОПОЛНЕН) ===
    const char* getName() const override { return "WiegandManager"; }
    const char* getVersion() const override { return "5.0.0"; }
    uint32_t getModuleId() const override { return MODULE_ID_WIEGAND; }
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;
    bool isReady() const override { return _enabled && _initialized; }
    const char* getStatus() const override;
    void getDiagnostics(ShEventData* data) const override;

    // === ИНИЦИАЛИЗАЦИЯ (ВАША) ===
    void begin(uint8_t d0Pin, uint8_t d1Pin,
              uint32_t timeoutMs = 100,
              WiegandDeviceType expectedType = WiegandDeviceType::READER_26BIT);
    void end();
    void reset();

    // === КОЛБЭКИ (ВАШИ) ===
    void setOnCardRead(OnCardReadCallback cb) { _onCardRead = cb; }
    void setOnCardReadSimple(OnCardReadSimple cb) { _onCardReadSimple = cb; }
    void setOnStatsUpdate(OnStatsUpdateCallback cb) { _onStatsUpdate = cb; }
    void setOnError(OnErrorCallback cb) { _onError = cb; }
    void setOnWebCardRead(OnWebCardReadCallback cb) { _onWebCardRead = cb; }

    // === УПРАВЛЕНИЕ (ВАШЕ) ===
    void enable();
    void disable();
    bool isEnabled() const { return _enabled; }
    void resetBuffer();
    void resetStats();

    // === ЧТЕНИЕ (ВАШЕ) ===
    bool hasCard() const { return _cardAvailable; }
    WiegandData getCardData();
    uint32_t getCardNumber();

    // === КОНФИГУРАЦИЯ (ВАША) ===
    void setTimeout(uint32_t ms) { _timeoutMs = constrain(ms, 50U, 1000U); }
    void setExpectedType(WiegandDeviceType type) { _expectedType = type; }
    void setIgnoreParity(bool ignore) { _ignoreParity = ignore; }
    void setMinBits(uint8_t minBits) { _minBits = minBits; }
    void setMaxBits(uint8_t maxBits) { _maxBits = maxBits; }
    void setDebounceUs(uint32_t us) { _debounceUs = constrain(us, 1U, 1000U); }

    // === WEB-РЕЖИМ (ВАШ) ===
    void startWebReadMode(uint32_t timeoutMs = 10000);
    void stopWebReadMode();
    bool isWebReadMode() const { return _webReadMode; }

    // === ДИАГНОСТИКА (ВАША, РАСШИРЕНА) ===
    void streamDiagnosticInfo(Stream& stream) const;
    void printStats() const;

    // === ISR-ОБРАБОТЧИКИ (ВАШИ) ===
    static void IRAM_ATTR handleD0_ISR();
    static void IRAM_ATTR handleD1_ISR();

private:
    // === ВНУТРЕННИЕ МЕТОДЫ (ВАШИ) ===
    void processCard(uint64_t rawData, uint8_t bitCount);
    void updateStats(bool valid, bool parityOk, uint8_t bitCount);
    bool checkParity26(uint64_t rawData);
    bool checkParity34(uint64_t rawData);
    #ifdef WIEGAND_SUPPORT_37BIT
    bool checkParity37(uint64_t rawData);
    #endif
    void decode26Bit(uint64_t rawData);
    void decode34Bit(uint64_t rawData);
    #ifdef WIEGAND_SUPPORT_37BIT
    void decode37Bit(uint64_t rawData);
    #endif
    void logMessage(const char* msg);
    void logMessage(const char* format, ...);
    void safeStrCopy(char* dest, size_t destSize, const char* src);
    bool isInitializedAndEnabled() const;

    // === ОТПРАВКА СОБЫТИЙ (ВАША, РАСШИРЕНА) ===
    void publishCardEvent(const WiegandData& data);
    void publishStatsEvent();
    void publishErrorEvent(const char* errorCode);
    void handleCardData(const WiegandData& data); // НОВОЕ

    // === ОБРАБОТЧИКИ СОБЫТИЙ (ВАШИ) ===
    static void eventHandler(void* handlerArgs, esp_event_base_t base,
                            int32_t id, void* eventData);
    void handleCommand(const ShEventData* data);

    // === ISR-БУФЕР (ВАШ) ===
    struct ISR_Buffer {
        volatile uint64_t rawData;
        volatile uint8_t bitCount;
        volatile uint32_t lastBitTime;
        volatile bool overflow;
        volatile uint32_t debounceCounter;
    };

    // === ДАННЫЕ (ВАШИ, РАСШИРЕНЫ) ===
    uint8_t _d0Pin = 255;
    uint8_t _d1Pin = 255;
    uint8_t _d0InterruptNum = 0;
    uint8_t _d1InterruptNum = 0;
    uint32_t _timeoutMs = 100;
    uint32_t _debounceUs = 10;
    uint8_t _minBits = 24;
    uint8_t _maxBits = 64;
    bool _ignoreParity = false;
    WiegandDeviceType _expectedType = WiegandDeviceType::READER_26BIT;
    bool _initialized = false;
    bool _enabled = false;
    bool _cardAvailable = false;
    bool _webReadMode = false;
    uint32_t _webReadTimeoutMs = 10000;
    uint32_t _webReadStartMs = 0;
    WiegandData _lastCardData;
    WiegandStats _stats;
    uint32_t _statsUpdateCounter = 0;
    uint32_t _lastTickMs = 0;
    uint32_t _moduleId = MODULE_ID_WIEGAND;
    bool _beginInProgress = false;

    // НОВОЕ: счетчик опубликованных событий
    uint32_t _totalEventsPublished = 0;

    SemaphoreHandle_t _wiegandMutex = nullptr;

    static volatile ISR_Buffer _isrBuf;
    static portMUX_TYPE _isrMux;
    static WiegandManager* _instance;

    // === КОЛБЭКИ (ВАШИ) ===
    OnCardReadCallback _onCardRead = nullptr;
    OnCardReadSimple _onCardReadSimple = nullptr;
    OnStatsUpdateCallback _onStatsUpdate = nullptr;
    OnErrorCallback _onError = nullptr;
    OnWebCardReadCallback _onWebCardRead = nullptr;

    // === КОНСТАНТЫ (ВАШИ) ===
    static constexpr uint32_t STATS_UPDATE_INTERVAL = 100;
    static constexpr uint32_t MAX_BIT_COUNT = 64;
    static constexpr uint32_t ISR_DEBOUNCE_US = 5;
    static constexpr uint32_t MUTEX_TIMEOUT_MS = 500;
};