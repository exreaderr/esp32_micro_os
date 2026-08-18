// ============================================================================
// IDeviceProfile.h — КОНТРАКТ ПРОФИЛЯ УСТРОЙСТВА + МАНИФЕСТ ПЕРИФЕРИИ
// ============================================================================
// Фаза 0. Лежит в core/, потому что Kernel::boot() принимает IDeviceProfile —
// это часть контракта ядра. Реализации профилей живут в profiles/<device>/.
//
// Профиль — единственное место, где знают про периферию устройства:
//   describeHardware()   -> пины/шины (валидируются ResourceManager'ом)
//   registerDrivers()    -> драйверы периферии в реестр (Phase 1)
//   registerModules()    -> доменные сервисы + приложение устройства
//
// HardwareManifest — плоская структура фиксированного размера (boot-ранняя
// стадия: куча ещё не инициализирована, никаких String/vector).
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include "../platform/BaseProfile.h"   // BoardId: плата платформы (5.3.0)

class ResourceManager;
class Kernel;

// ============================================================================
// МАНИФЕСТ ПЕРИФЕРИИ
// ============================================================================
// Профиль заполняет только то, что у него есть; -1/0xFF = "нет такого".
// Валидация: каждый занятый пин регистрируется в ResourceManager — конфликт
// с базой или между двумя устройствами отлавливается при старте.
// ============================================================================

/// Произвольная GPIO-запись манифеста (для пинов вне типизированных полей)
struct ManifestGpio {
    uint8_t pin;                 // номер GPIO
    char    role[20];            // "lock.relay", "light.pwm" — владелец
};

struct HardwareManifest {
    // --- Аппаратная плата платформы (5.3.0: вторая плата, проверка A4) ---
    // Ядро вызывает platform::selectBoard() сразу после describeHardware —
    // до claim'а ресурсов, BusManager (I2C-пины) и NetworkService (ETH).
    platform::BoardId boardId = platform::BoardId::Wt32Eth01;   // дефолт

    // --- Системная кнопка Safe Mode (обрабатывает ядро, не профиль!) -----
    int8_t  safeModePin = -1;    // у smart_lock = GPIO14 (кнопка EXIT)

    // --- Типизированная периферия (только КОНСТАНТЫ платформы) -------------
    // Сюда попадает лишь периферия, общая для многих профилей (уровни 1–2
    // драйверной модели). Периферия каталога (уровень 3, напр. Wiegand)
    // объявляется через addGpio() — ядро о ней не знает.
    int8_t  dfPlayerRx = -1;     // DFPlayer Mini (UART)
    int8_t  dfPlayerTx = -1;
    int8_t  dfPlayerBusy = -1;
    uint8_t dfPlayerUart = 0xFF;

    // --- Произвольные GPIO профиля -------------------------------------------
    static constexpr uint8_t MAX_GPIO = 12;
    ManifestGpio gpio[MAX_GPIO];
    uint8_t gpioCount = 0;

    /// Добавить пин в манифест (профиль вызывает из describeHardware).
    /// false — таблица полна.
    bool addGpio(uint8_t pin, const char* role) {
        if (gpioCount >= MAX_GPIO) return false;
        gpio[gpioCount].pin = pin;
        size_t i = 0;
        for (; role && role[i] && i < sizeof(gpio[gpioCount].role) - 1; ++i)
            gpio[gpioCount].role[i] = role[i];
        gpio[gpioCount].role[i] = '\0';
        gpioCount++;
        return true;
    }

    /// Валидация всего манифеста через реестр ресурсов.
    /// Возвращает false при первом же конфликте (детали — в логе RM).
    bool validateResources(ResourceManager& rm) const;
};

// ============================================================================
// КОНТРАКТ ПРОФИЛЯ
// ============================================================================
class IDeviceProfile {
public:
    virtual ~IDeviceProfile() = default;

    /// Идентификатор профиля: "smart_lock", "smart_light".
    virtual const char* profileId() const = 0;

    /// Заполнить манифест периферии. Вызывается ядром ДО детекта Safe Mode
    /// (нужен safeModePin) — поэтому функция должна быть быстрой и без
    /// обращений к железу/heap: только запись констант в структуру.
    virtual void describeHardware(HardwareManifest& m) = 0;

    /// Зарегистрировать драйверы периферии (Phase 1: DriverRegistry).
    /// Вызывается только в штатном режиме (в Safe Mode — пропускается).
    virtual void registerDrivers(const HardwareManifest& m) = 0;

    /// Зарегистрировать модули профиля в Kernel.
    virtual void registerModules(Kernel& k) = 0;
};
