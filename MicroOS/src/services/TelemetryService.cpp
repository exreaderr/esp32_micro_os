// ============================================================================
// TelemetryService.cpp — реализация сбора системных метрик
// ============================================================================
#include "TelemetryService.h"
#include "ConfigService.h"
#include "NetworkManager.h"
#include "AuditService.h"
#include "../core/Events.h"
#include "../core/Kernel.h"
#include "../core/DriverRegistry.h"
#include "../drivers/EspTempDriver.h"

TelemetryService& TelemetryService::getInstance() {
    static TelemetryService instance;
    return instance;
}

// ============================================================================
// КОНФИГ-СХЕМА
// ============================================================================
void TelemetryService::registerExtensions() {
    ConfigService::getInstance().addFields("Телеметрия", {
        { "tel.period_s", ConfigType::UINT, "15", 5, 300, CFG_NONE,
          "Телеметрия", "Период сбора метрик, с" },
    });
    // Пороги ПАЗ температуры кристалла (Phase 2 из шапки EspTempDriver.h).
    // Умолчания 75/90/105 (решение владельца 14.08.2026, +15 к монолитным
    // 60/75/90): датчик меряет КРИСТАЛЛ, а не корпус; WT32-ETH01 в закрытом
    // корпусе с активным PHY законно живёт на ~79 °C — постоянный «критикал»
    // обесценивал событие. Поднимать дальше — ОСОЗНАННО, это ручка ПАЗ,
    // а не «убрать красное».
    ConfigService::getInstance().addFields("Система", {
        { "sys.temp_warn_c", ConfigType::INT, "75", 30, 100, CFG_NONE,
          "Система", "CPU: предупреждение по температуре, °C" },
        { "sys.temp_crit_c", ConfigType::INT, "90", 40, 125, CFG_NONE,
          "Система", "CPU: критическая температура, °C" },
        { "sys.temp_panic_c", ConfigType::INT, "105", 50, 125, CFG_NONE,
          "Система", "CPU: ТЕРМИЧЕСКАЯ ПАНИКА (журнал ПАЗ), °C" },
        // 5.8.1 (урок №22): ширина перевзвода warn/crit — тоже ручка.
        { "sys.temp_hyst_c", ConfigType::INT, "2", 0, 10, CFG_NONE,
          "Система", "CPU: гистерезис температурных порогов, °C" },
    });
}

void TelemetryService::init() {
    applyTempThresholds();
    _initialized = true;
    log(LogLevel::Info, "init: metrics collector ready");
}

void TelemetryService::applyTempThresholds() {
    // Пороги — драйверу (санитизация внутри setThresholds/setPanic)
    EspTempDriver& t = EspTempDriver::getInstance();
    t.setThresholds((float)cfgGetInt("sys.temp_warn_c", 75),
                    (float)cfgGetInt("sys.temp_crit_c", 90),
                    (float)cfgGetInt("sys.temp_hyst_c", 2));
    t.setPanic((float)cfgGetInt("sys.temp_panic_c", 105));
}

void TelemetryService::onEvent(int32_t eventId, const ShEventData* data) {
    // Живое применение порогов ПАЗ: поля sys.temp_* — CFG_NONE, поэтому
    // без подписки правка из панели ждала бы рестарта (ловушка UX 5.0.4).
    if (eventId != CFG_EVENT_CHANGED || data == nullptr) return;
    if (strcmp(data->payload, "sys.temp_warn_c") == 0 ||
        strcmp(data->payload, "sys.temp_crit_c") == 0 ||
        strcmp(data->payload, "sys.temp_panic_c") == 0 ||
        strcmp(data->payload, "sys.temp_hyst_c") == 0) {
        applyTempThresholds();
    }
}

void TelemetryService::start() {
    _started = true;
    collect();   // первый снимок сразу — подписчики не ждут периода
}

void TelemetryService::stop() { _started = false; }

// ============================================================================
// TICK: периодический сбор
// ============================================================================
void TelemetryService::tick() {
    uint32_t periodMs = cfgGetUInt("tel.period_s", 15) * 1000UL;
    if (millis() - _lastCollectMs >= periodMs) {
        collect();
    }
}

// ============================================================================
// СБОР СНИМКА У ВЛАДЕЛЬЦЕВ МЕТРИК
// ============================================================================
void TelemetryService::collect() {
    _lastCollectMs = millis();

    _snap.uptimeSec  = millis() / 1000;
    _snap.heapFree   = ESP.getFreeHeap();
    _snap.heapMin    = ESP.getMinFreeHeap();

    EventBus& bus = EventBus::getInstance();
    _snap.busDropped   = bus.getDroppedCount();
    _snap.busHighWater = (uint16_t)bus.getHighWatermark();

    // Температура — от единственного источника (драйвер платформы).
    // cpuSeq — пульс ЧТЕНИЙ драйвера: дежурный HealthMonitor следит за
    // ним, а не за значением (урок 15.08.2026: в терморавновесии значение
    // неподвижно часами — ложные STUCK; остановившиеся чтения — настоящие).
    auto* temp = DriverRegistry::getInstance().findAs<EspTempDriver>("esp_temp");
    _snap.cpuTenths = temp ? (int16_t)(temp->getTemperature() * 10.0f) : 0;
    _snap.cpuSeq    = temp ? (int16_t)temp->getReadSeq() : 0;

    // Сеть — уровень деградации и качество канала до шлюза
    NetworkService& net = NetworkService::getInstance();
    _snap.degradation  = (uint8_t)net.degradationLevel();
    _snap.gatewayRttMs = net.gatewayRttMs();

    // Живучесть — bootloop-счётчик ядра (RTC RAM)
    _snap.bootloopCount = Kernel::rtc().bootloopCount;

    // Аудит — потери очереди (должны быть 0)
    _snap.auditOverflows = AuditService::getInstance().queueOverflows();

    // Снимок — в шину: MqttTransport (порция 2) опубликует в УД.
    // code: heapFree в КБ (самая оперативная метрика); payload: краткая форма.
    ShEventData d; d.clear();
    d.code = (int32_t)(_snap.heapFree / 1024);
    snprintf(d.payload, sizeof(d.payload), "t=%d h=%lu d=%u",
             (int)_snap.cpuTenths,
             (unsigned long)(_snap.heapFree / 1024),
             (unsigned)_snap.degradation);
    postEvent(TEL_EVENT_SNAPSHOT, &d);
}

// ============================================================================
// JSON (для /api/system и MQTT)
// ============================================================================
size_t TelemetryService::toJson(char* buf, size_t bufSize) const {
    if (bufSize == 0) return 0;
    int n = snprintf(buf, bufSize,
        "{\"uptime\":%lu,\"heap\":%lu,\"heap_min\":%lu,"
        "\"bus_dropped\":%lu,\"bus_hwm\":%u,"
        "\"cpu_t\":%d.%d,\"degradation\":%u,\"gw_rtt\":%lu,"
        "\"bootloop\":%u,\"audit_lost\":%lu}",
        (unsigned long)_snap.uptimeSec,
        (unsigned long)_snap.heapFree,
        (unsigned long)_snap.heapMin,
        (unsigned long)_snap.busDropped,
        (unsigned)_snap.busHighWater,
        (int)(_snap.cpuTenths / 10), (int)(abs(_snap.cpuTenths % 10)),
        (unsigned)_snap.degradation,
        (unsigned long)_snap.gatewayRttMs,
        (unsigned)_snap.bootloopCount,
        (unsigned long)_snap.auditOverflows);
    return n > 0 ? (size_t)n : 0;
}
