// ============================================================================
// DriverRegistry.h — РЕЕСТР ДРАЙВЕРОВ ПЕРИФЕРИИ
// ============================================================================
// Фаза 1. Наследник половины DeviceManager v4.2.2: регистрация, поиск по
// имени, тик всех драйверов одной точкой. Сам — модуль ядра (ModuleBase),
// чтобы получить фазы init/tick и логирование бесплатно.
//
// Поток (из базовой архитектуры, раздел 3.1–3.2):
//   1. profile.registerDrivers() -> dr.add<...>() при загрузке (шаг 6);
//   2. фаза init ядра -> registry.init() -> driver->init() каждого драйвера
//      (порядок = порядок регистрации; шины уже живы — BusManager идёт
//      раньше по приоритету);
//   3. tick() реестра -> poll() каждого драйвера по его getPollIntervalMs();
//   4. isHealthy() агрегируется для HealthMonitor.
// ============================================================================
#pragma once

#include "ModuleBase.h"
#include "IDeviceDriver.h"

constexpr uint8_t DRIVER_REGISTRY_MAX = 16;   // драйверов на устройство

class DriverRegistry : public ModuleBase {
public:
    static DriverRegistry& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "DriverRegistry"; }
    const char* getVersion() const override { return "5.0.0"; }
    ModuleId getModuleId() const override { return 0x0004; }

    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 50; }  // часто: poll
                                                                // фильтруется
                                                                // по драйверам
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- РЕЕСТР --------------------------------------------------------------
    /// Добавить драйвер (вызывается из profile.registerDrivers).
    /// false — таблица полна или драйвер с таким именем уже есть.
    bool add(IDeviceDriver* driver);

    /// Поиск по имени: "esp_temp". nullptr — нет такого.
    IDeviceDriver* find(const char* name) const;

    /// Типобезопасный поиск: dr.findAs<EspTempDriver>("esp_temp").
    template <typename T>
    T* findAs(const char* name) const {
        return static_cast<T*>(find(name));
    }

    // --- ЗДОРОВЬЕ --------------------------------------------------------------
    /// true, если ВСЕ драйверы живы (для сводного статуса HealthMonitor).
    bool allHealthy() const;

    /// Число драйверов в состоянии failed (init вернул false).
    uint8_t failedCount() const { return _failedCount; }

private:
    DriverRegistry() = default;

    struct DriverSlot {
        IDeviceDriver* driver;
        uint32_t       lastPollMs;
        bool           failed;     // init() вернул false
    };

    DriverSlot _slots[DRIVER_REGISTRY_MAX];
    uint8_t    _count = 0;
    uint8_t    _failedCount = 0;
};
