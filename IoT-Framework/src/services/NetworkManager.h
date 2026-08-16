// ============================================================================
// NetworkManager.h — ЕДИНЫЙ ВЛАДЕЛЕЦ СОСТОЯНИЯ СЕТИ (Фаза 2, порция 2)
// ============================================================================
// Собирает сетевую логику, размазанную в монолите v4.2.2 по трём местам:
//   · DeviceManager  — поднятие Ethernet (ETH.begin, параметры WT32-ETH01);
//   · ConfigManager  — applyNetworkSettings (DHCP/статика из конфигурации);
//   · PazManager     — контроль шлюза пингом (живучесть маршрута).
// Теперь: ОДИН модуль владеет link/IP/шлюзом и ОДИН публикует NET_* события.
//
// Реализованные решения дорожной карты:
//   · A3 — уровни деградации: FULL -> LOCAL_NET -> AUTONOMOUS.
//     NetworkService владеет СЕТЕВОЙ осью деградации. Когда появится
//     вторая ось (напр. "RTC потерян", Фаза 3), агрегация max(оси) уйдёт
//     в Kernel — интерфейс SH_EVENT_DEGRADED_LEVEL от этого не изменится.
//   · E1 — идентичность устройства: hostname (config или auto из MAC),
//     deviceId (MAC ETH) — основа fleet-операций и MQTT-топиков (Фаза 3).
//
// Поведение в Safe Mode: модуль — ядерный, стартует ВСЕГДА (сеть нужна
// для recovery: веб-интерфейс восстановления и OTA).
// ============================================================================
#pragma once

#include "../core/ModuleBase.h"
#include "../platform/BaseProfile.h"
#include <IPAddress.h>

// ============================================================================
// УРОВНИ ДЕГРАДАЦИИ (A3)
// ============================================================================
// FULL        — link + IP + шлюз доступен: полный функционал (MQTT, NTP, УД).
// LOCAL_NET   — link + IP есть, шлюз НЕ отвечает: работает локальный веб-UI
//               и межустройственные события в сегменте, облачные функции
//               приостановлены (подписчик — MqttTransport, Фаза 3).
// AUTONOMOUS  — сети нет (link down / нет IP / net.enabled=false):
//               только локальная бизнес-логика. Для СКУД: Fail-Safe по
//               локальной базе карт — ровно поведение монолита при потере сети.
// ============================================================================
enum class DegradationLevel : uint8_t {
    Full       = 0,
    LocalNet   = 1,
    Autonomous = 2
};

// Бюджеты
constexpr uint32_t NET_PING_COUNT         = 3;     // пакетов в сессии проверки
constexpr uint32_t NET_PING_TIMEOUT_MS    = 800;   // таймаут одного пакета
constexpr uint8_t  NET_GW_FAIL_THRESHOLD  = 2;     // неудачных сессий до LOST
constexpr uint8_t  NET_HOSTNAME_LEN       = 24;    // "microos-a1b2c3" + запас
constexpr uint8_t  NET_DEVICE_ID_LEN      = 13;    // MAC HEX (12) + '\0'

// NB: класс назван NetworkService, а НЕ NetworkManager (как в дорожной
// карте): в Arduino-ядре 3.x уже есть глобальный класс NetworkManager
// (библиотека Network) — коллизия имён ловится на этапе сборки.
class NetworkService : public ModuleBase {
public:
    static NetworkService& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "NetworkService"; }
    const char* getVersion() const override { return "5.5.6"; }   // 5.5.6: fallback на hardcoded 192.168.1.50 (конфиг игнорится); 5.5.5: Safe Mode fallback; 5.5.2: probe по факту
    ModuleId getModuleId() const override { return 0x0100; }   // транспорт

    void registerExtensions() override;   // инжекция схемы net.* / sys.hostname
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    uint32_t getTickIntervalMs() const override { return 500; }
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }

    // --- СОСТОЯНИЕ (для сервисов и профилей) ------------------------------
    bool isConnected() const { return _linkUp && _hasIp; }
    bool isGatewayOk() const { return _gwOk; }
    DegradationLevel degradationLevel() const { return _level; }
    uint32_t gatewayRttMs() const { return _gwRttMs; }

    // --- ИДЕНТИЧНОСТЬ (E1) -------------------------------------------------
    const char* hostname() const { return _hostname; }
    /// MAC интерфейса ETH как HEX-строка ("a1b2c3d4e5f6") — стабильный
    /// идентификатор устройства в экосистеме (топики MQTT, реестр УД).
    const char* deviceId() const { return _deviceId; }
    /// Текущий IP строкой в буфер вызывающего ("0.0.0.0", если нет).
    void ipString(char* buf, size_t bufSize) const;

    // --- ТОЧКИ ВХОДА КОЛБЭКОВ ОС (НЕ для прикладного кода!) ----------------
    // Вызываются из задач сетевого стека и ping-треда через статические
    // трамплины .cpp (C-ABI требует свободных функций -> методы публичны).
    // Колбэки только выставляют флаги; события шины публикуются из tick().
    void onNetConnected();
    void onNetDisconnected();
    void onNetGotIp();
    void onNetLostIp();
    void onPingPacket(bool ok, uint32_t rttMs);
    void onPingSessionEnd();

private:
    NetworkService() = default;

    // --- ВНУТРЕННЯЯ КУХНЯ ---------------------------------------------------
    void buildIdentity();             // hostname + deviceId (E1)
    void applyNetConfig();            // DHCP или статика из ConfigService
    void applySafeStaticFallback();   // Safe Mode: статика при мёртвом DHCP (5.5.5)
    void evaluateDegradation();       // пересчёт уровня A3 + событие
    void startGatewayPing();          // сессия esp_ping (одна за раз)
    void finishGatewayPing();         // вердикт по завершённой сессии

    // --- СОСТОЯНИЕ -----------------------------------------------------------
    // volatile: пишутся из задач сетевого стека/ping, читаются из tick().
    volatile bool _linkUp = false;    // ETH link поднят
    volatile bool _hasIp  = false;    // IP получен/назначен
    bool _gwOk          = true;       // шлюз отвечает (оптимистичный старт)
    bool _netEnabled    = true;       // net.enabled из конфигурации
    bool _pingActive    = false;      // идёт ping-сессия
    uint8_t  _gwFailStreak = 0;       // подряд неудачных сессий ping
    uint32_t _gwRttMs      = 0;       // RTT последнего успешного пакета
    uint32_t _lastPingMs   = 0;       // когда стартовала последняя сессия

    // Safe Mode статический fallback (5.5.5, закрытие бэклога сетевой
    // политики владельца): ТОЛЬКО в Safe Mode при net.dhcp=true — если за
    // ~15 с после link-up нет DHCP-лиза, адрес назначается статикой из
    // net.ip/mask/gateway (заводское 192.168.1.50/24), чтобы recovery-веб
    // был достижен в сети вообще без DHCP-сервера. В нормальном режиме
    // поведение не меняется: ожидание DHCP + деградация.
    uint32_t _safeDhcpSinceMs  = 0;     // link поднят без IP: момент взвода (0 = не взведён)
    bool     _safeFallbackDone = false; // статика применена (one-shot за сессию)

    // Результат текущей сессии ping (заполняется колбэками ping-задачи)
    volatile bool     _pingEnded   = false; // сессия завершена -> разбор в tick
    volatile uint32_t _pingReplies = 0;     // получено ответов
    volatile uint32_t _pingLastRtt = 0;     // RTT последнего ответа, мс

    DegradationLevel _level = DegradationLevel::Autonomous;

    char _hostname[NET_HOSTNAME_LEN];   // sys.hostname или auto
    char _deviceId[NET_DEVICE_ID_LEN];  // MAC HEX (E1)
    IPAddress _ip;                      // текущий IP (из события GOT_IP)
};
