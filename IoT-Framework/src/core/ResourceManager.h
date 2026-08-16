// ============================================================================
// ResourceManager.h — РЕЕСТР АППАРАТНЫХ И СИСТЕМНЫХ РЕСУРСОВ (предложение A2)
// ============================================================================
// Фаза 0. Отвечает на вопрос: "кто занял этот ресурс?" — и не даёт двум
// владельцам занять один ресурс дважды.
//
// Что регистрируем:
//   · GPIO         — пины (реле, геркон, Wiegand, PHY power...)
//   · I2C-адреса   — 0x68 DS3231 и любые другие на шине
//   · UART         — аппаратные Serial 0/1/2
//   · NVS-namespace — "lock_cfg", "db_backup"... (чтобы два сервиса не писали
//                     в один namespace Preferences)
//   · диапазоны событий — выдача базового смещения профилю
//
// Поток использования:
//   1. BaseProfile (ядро) регистрирует ресурсы платформы: GPIO16 (PHY power),
//      GPIO32/33 (I2C), адрес 0x68 (DS3231).
//   2. Kernel вызывает profile.describeHardware() -> манифест периферии.
//   3. Kernel валидирует манифест через claim*(): конфликт -> модуль НЕ
//      стартует, в лог — явная ошибка вида
//      "GPIO15 занят: 'lock.relay' vs 'light.pwm'".
//   4. report() -> карта занятых ресурсов (для /api/system и документации).
//
// Все таблицы статические — реестр работает до init кучи (ранний boot).
// ============================================================================
#pragma once

#include <Arduino.h>
#include "ShTypes.h"

// Бюджеты таблиц (WT32-ETH01: 40 GPIO; I2C-устройств мало; UART — 3)
constexpr uint8_t  RM_MAX_GPIO     = 40;   // GPIO 0..39
constexpr uint8_t  RM_MAX_I2C      = 8;    // занятых адресов на шине
constexpr uint8_t  RM_MAX_UART     = 3;    // UART0/1/2
constexpr uint8_t  RM_MAX_NVS      = 8;    // namespace'ов Preferences
constexpr uint8_t  RM_MAX_EVENTS   = 24;   // выданных диапазонов событий
constexpr uint8_t  RM_OWNER_LEN    = 24;   // длина имени владельца

class ResourceManager {
public:
    static ResourceManager& getInstance();

    // --- GPIO ------------------------------------------------------------
    /// Занять пин за владельцем (например "lock.relay").
    /// true — пин свободен и зарегистрирован; false — конфликт, в логе
    /// имя прежнего владельца. Повторная регистрация ТЕМ ЖЕ владельцем
    /// идемпотентна (возвращает true).
    bool claimGpio(uint8_t pin, const char* owner);

    /// Проверка без регистрации (для валидации манифеста "на сухую").
    bool isGpioFree(uint8_t pin) const;

    /// Имя владельца пина или nullptr, если свободен.
    const char* gpioOwner(uint8_t pin) const;

    // --- I2C -------------------------------------------------------------
    bool claimI2cAddress(uint8_t addr7bit, const char* owner);

    // --- UART ------------------------------------------------------------
    bool claimUart(uint8_t uartNum, const char* owner);

    // --- NVS namespace -----------------------------------------------------
    bool claimNvsNamespace(const char* nsName, const char* owner);

    // --- Диапазоны событий -------------------------------------------------
    /// Выдать базовое смещение событий в диапазоне приложений
    /// (SH_EVENT_APP_BASE..SH_EVENT_APP_END) шагом 0x40.
    /// Повторный запрос ТЕМ ЖЕ владельцем идемпотентен — возвращает его
    /// базу (как claimGpio). -1 только при исчерпании таблицы.
    int32_t claimEventRange(const char* owner);

    // --- Отчёт -------------------------------------------------------------
    /// Текстовая карта занятых ресурсов в буфер (для /api/system, лога).
    /// Возвращает длину записанного текста.
    size_t report(char* buf, size_t bufSize) const;

    /// Общее число зарегистрированных конфликтов с начала работы.
    uint32_t conflictCount() const { return _conflicts; }

private:
    ResourceManager() = default;

    /// Общий код обнаружения конфликта: лог + счётчик.
    bool conflict(const char* kind, uint32_t id, const char* newOwner,
                  const char* existingOwner);

    // --- Таблицы -----------------------------------------------------------
    // GPIO: владелец по индексу пина; nullptr = свободен.
    char _gpioOwner[RM_MAX_GPIO][RM_OWNER_LEN];
    bool _gpioUsed[RM_MAX_GPIO];

    char _i2cOwner[RM_MAX_I2C][RM_OWNER_LEN];
    uint8_t _i2cAddr[RM_MAX_I2C];
    uint8_t _i2cCount = 0;

    char _uartOwner[RM_MAX_UART][RM_OWNER_LEN];
    bool _uartUsed[RM_MAX_UART];

    char _nvsNs[RM_MAX_NVS][RM_OWNER_LEN];       // имя namespace
    char _nvsOwner[RM_MAX_NVS][RM_OWNER_LEN];    // владелец
    uint8_t _nvsCount = 0;

    int32_t _eventBase[RM_MAX_EVENTS];
    char _eventOwner[RM_MAX_EVENTS][RM_OWNER_LEN];
    uint8_t _eventCount = 0;

    uint32_t _conflicts = 0;
};
