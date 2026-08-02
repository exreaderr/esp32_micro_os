// ============================================================================
// NetworkManager.cpp — реализация класса NetworkService. единого владельца сети
// ============================================================================
#include "NetworkManager.h"
#include "ConfigService.h"
#include "../core/Events.h"

#include <ETH.h>                  // Ethernet LAN8720 (библиотека ядра)
#include <WiFi.h>                 // WiFi.onEvent — общая точка сетевых событий
#include <esp_mac.h>              // esp_read_mac (идентичность, E1)
#include <lwip/ip_addr.h>         // ipaddr_aton
#include <ping/ping_sock.h>       // esp_ping_* (контроль шлюза, ex-ПАЗ)

// ============================================================================
// ТРАМПЛИНЫ КОЛБЭКОВ (C-ABI -> экземпляр)
// ============================================================================
// Сетевой стек и ping-тред требуют свободных функций. Указатель на
// экземпляр — файловая статика (урок IRAM: никаких getInstance() в колбэках
// чужих задач — статический локал с guard-переменной недопустим).
// ============================================================================
static NetworkService* s_nm = nullptr;

// Сетевые события: вызываются из задачи Arduino event loop (не ISR!).
// Только флаги — никаких событий шины и логов с аллокациями отсюда.
static void nmNetEventTrampoline(arduino_event_id_t event,
                                 arduino_event_info_t /*info*/) {
    if (s_nm == nullptr) return;
    switch (event) {
        case ARDUINO_EVENT_ETH_CONNECTED:    s_nm->onNetConnected();    break;
        case ARDUINO_EVENT_ETH_DISCONNECTED: s_nm->onNetDisconnected(); break;
        case ARDUINO_EVENT_ETH_GOT_IP:       s_nm->onNetGotIp();        break;
        case ARDUINO_EVENT_ETH_LOST_IP:      s_nm->onNetLostIp();       break;
        default: break;
    }
}

// Ping-колбэки: вызываются из внутреннего ping-треда lwIP (не ISR!).
static void nmPingSuccessTrampoline(esp_ping_handle_t hdl, void* args) {
    auto* self = static_cast<NetworkService*>(args);
    uint32_t rtt = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
    if (self) self->onPingPacket(true, rtt);
}
static void nmPingTimeoutTrampoline(esp_ping_handle_t /*hdl*/, void* args) {
    auto* self = static_cast<NetworkService*>(args);
    if (self) self->onPingPacket(false, 0);
}
static void nmPingEndTrampoline(esp_ping_handle_t hdl, void* args) {
    // Сессия отработала — удаляем её прямо здесь (handle локальный,
    // в NetworkService не хранится), вердикт tick() вынесет по флагу.
    esp_ping_delete_session(hdl);
    auto* self = static_cast<NetworkService*>(args);
    if (self) self->onPingSessionEnd();
}

NetworkService& NetworkService::getInstance() {
    static NetworkService instance;
    return instance;
}

// ============================================================================
// КОЛБЭК-ВХОДЫ (из задач сетевого стека / ping-треда)
// ============================================================================
void NetworkService::onNetConnected()    { _linkUp = true; }
void NetworkService::onNetDisconnected() { _linkUp = false; _hasIp = false; }
void NetworkService::onNetGotIp()        { _hasIp = true; }
void NetworkService::onNetLostIp()       { _hasIp = false; }

void NetworkService::onPingPacket(bool ok, uint32_t rttMs) {
    if (ok) {
        _pingReplies++;
        _pingLastRtt = rttMs;
    }
}

void NetworkService::onPingSessionEnd() { _pingEnded = true; }

// ============================================================================
// РЕГИСТРАЦИЯ КОНФИГ-СХЕМЫ (инжекция в ConfigService, точка расширения)
// ============================================================================
// Все параметры сети — декларативные поля: веб-UI (Фаза 3) построится из
// этой схемы автоматически, ядро о полях не знает.
// CFG_CRITICAL — изменение требует переприменения сети (перезагрузка или
// команда reapply через веб; Phase 3).
// ============================================================================
void NetworkService::registerExtensions() {
    ConfigService::getInstance().addFields("Сеть", {
        { "net.enabled",   ConfigType::BOOL, "true", 0, 0, CFG_CRITICAL,
          "Сеть", "Сеть включена (иначе — локальный режим)" },
        { "net.dhcp",      ConfigType::BOOL, "true", 0, 0, CFG_CRITICAL,
          "Сеть", "DHCP (иначе — статический адрес)" },
        { "net.ip",        ConfigType::IP, "192.168.1.50", 0, 0, CFG_CRITICAL,
          "Сеть", "Статический IP" },
        { "net.mask",      ConfigType::IP, "255.255.255.0", 0, 0, CFG_CRITICAL,
          "Сеть", "Маска подсети" },
        { "net.gateway",   ConfigType::IP, "192.168.1.1", 0, 0, CFG_CRITICAL,
          "Сеть", "Шлюз" },
        { "net.dns",       ConfigType::IP, "8.8.8.8", 0, 0, CFG_CRITICAL,
          "Сеть", "DNS-сервер" },
        { "net.ping_gw",   ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Сеть", "Контроль шлюза (деградация LOCAL_NET)" },
        { "net.gw_period", ConfigType::UINT, "30", 10, 300, CFG_NONE,
          "Сеть", "Период проверки шлюза, с" },
        // hostname — в группе «Сеть» (урок 5.0.x: в «Системе» его видела
        // только ядерная /admin, а это по смыслу сетевая настройка —
        // пользователь искал её рядом с DNS в панели профиля).
        { "sys.hostname",  ConfigType::STRING, "", 0, 0, CFG_CRITICAL,
          "Сеть", "Имя устройства (пусто = auto из MAC)" },
    });
}

// ============================================================================
// INIT: идентичность + поднятие Ethernet
// ============================================================================
void NetworkService::init() {
    s_nm = this;
    buildIdentity();

    _netEnabled = cfgGetBool("net.enabled", true);
    if (!_netEnabled) {
        // Локальный режим (из монолита: устройство без сети — честный
        // AUTONOMOUS без аварийных событий). Пин PHY не трогаем вовсе.
        _initialized = true;
        log(LogLevel::Info, "init: network DISABLED (local mode), host=%s",
            _hostname);
        return;
    }

    // Обработчики сетевых событий — ДО ETH.begin, чтобы не пропустить
    // быстрый CONNECTED на коротком кабеле.
    WiFi.onEvent(nmNetEventTrampoline, ARDUINO_EVENT_ETH_CONNECTED);
    WiFi.onEvent(nmNetEventTrampoline, ARDUINO_EVENT_ETH_DISCONNECTED);
    WiFi.onEvent(nmNetEventTrampoline, ARDUINO_EVENT_ETH_GOT_IP);
    WiFi.onEvent(nmNetEventTrampoline, ARDUINO_EVENT_ETH_LOST_IP);

    applyNetConfig();   // статика при net.dhcp=false — ДО begin

    // WT32-ETH01, каноническая инициализация (проверена монолитом v2.5.0):
    //   PHY LAN8720, адрес 1, MDC=23, MDIO=18, power=GPIO16,
    //   тактирование 50 МГц — внешний генератор платы, вход GPIO0.
    // NB: если на иной ревизии платы link не поднимается — кандидат на
    // замену ETH_CLOCK_GPIO17_OUT (генерация из ESP32); см. BaseProfile.h.
    bool ok = ETH.begin(ETH_PHY_LAN8720, /*phy_addr*/ 1,
                        /*mdc*/ 23, /*mdio*/ 18,
                        /*power*/ platform::ETH_PHY_POWER_PIN,
                        ETH_CLOCK_GPIO0_IN);

    // Hostname — СТРОГО ПОСЛЕ begin (урок 5.0.x, найдено в исходниках ядра
    // Arduino ESP32 3.3.11): NetworkInterface::setHostname тихо возвращает
    // false при _esp_netif == NULL, т.е. ДО begin — вызов был no-op, и
    // DHCP option 12 уходил дефолтным «espressif» (шлюз видел контроллер
    // именно так). Сейчас netif уже создан, а DHCP-discover уйдёт только
    // по link-up через секунды — имя успеет встать.
    if (ok && !ETH.setHostname(_hostname)) {
        log(LogLevel::Warning, "init: setHostname(%s) failed", _hostname);
    }

    _initialized = true;
    log(LogLevel::Info, "init: ETH begin %s, host=%s, id=%s, dhcp=%d",
        ok ? "OK" : "FAILED", _hostname, _deviceId,
        (int)cfgGetBool("net.dhcp", true));
    if (!ok) publishError("ETH_BEGIN");
}

// ============================================================================
// START: первичная публикация состояния
// ============================================================================
void NetworkService::start() {
    _started = true;
    if (!_netEnabled) {
        ShEventData d; d.clear();
        postEvent(NET_EVENT_DISABLED, &d);
    }
    // Начальный уровень деградации фиксируем событием — подписчики
    // (MqttTransport Фазы 3, профиль) стартуют с известным состоянием.
    evaluateDegradation();
}

void NetworkService::stop() {
    _started = false;
    // ETH.end() не вызываем: остановка сети на живой системе опаснее,
    // чем её отсутствие; локальный режим задаётся конфигурацией + ребут.
}

// ============================================================================
// TICK: переходы состояния -> события, контроль шлюза, деградация
// ============================================================================
void NetworkService::tick() {
    if (!_netEnabled) return;

    // --- Переходы link/IP -> NET_* события --------------------------------
    // Флаги выставляются колбэками сетевого стека; события публикуем из
    // своего потока — порядок гарантирован, рекурсии шины исключены.
    static bool prevLink = false, prevIp = false;
    bool link = _linkUp, ip = _hasIp;

    if (link != prevLink) {
        prevLink = link;
        ShEventData d; d.clear();
        if (link) {
            postEvent(NET_EVENT_CONNECTED, &d);
        } else {
            d.code = 1;   // 1 = link down
            postEvent(NET_EVENT_DISCONNECTED, &d);
        }
    }
    if (ip != prevIp) {
        prevIp = ip;
        if (ip) {
            _ip = ETH.localIP();
            ShEventData d; d.clear();
            ipString(d.payload, sizeof(d.payload));
            postEvent(NET_EVENT_IP_CHANGED, &d);
            log(LogLevel::Info, "IP: %s", d.payload);
        } else if (link) {
            // IP потерян при живом link — для потребителей это "сети нет"
            ShEventData d; d.clear();
            d.code = 2;   // 2 = ip lost
            postEvent(NET_EVENT_DISCONNECTED, &d);
        }
    }

    // --- Контроль шлюза (ex-ПАЗ): периодическая ping-сессия ---------------
    if (ip && cfgGetBool("net.ping_gw", true)) {
        uint32_t periodMs = cfgGetUInt("net.gw_period", 30) * 1000UL;
        if (!_pingActive && millis() - _lastPingMs >= periodMs) {
            startGatewayPing();
        }
    }
    if (_pingEnded) {
        _pingEnded = false;
        finishGatewayPing();
    }

    // --- A3: пересчёт уровня деградации ------------------------------------
    evaluateDegradation();
}

// ============================================================================
// ИДЕНТИЧНОСТЬ (E1)
// ============================================================================
void NetworkService::buildIdentity() {
    // deviceId = MAC интерфейса ETH — уникален и стабилен для экземпляра
    // железа; используется в MQTT-топиках и реестре УД (Фаза 3).
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(_deviceId, sizeof(_deviceId), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // hostname: из конфигурации; пусто -> "microos-<последние 3 байта MAC>"
    cfgGetStr("sys.hostname", _hostname, sizeof(_hostname), "");
    if (_hostname[0] == '\0') {
        snprintf(_hostname, sizeof(_hostname), "microos-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }
}

void NetworkService::ipString(char* buf, size_t bufSize) const {
    if (bufSize == 0) return;
    if (_hasIp) {
        snprintf(buf, bufSize, "%u.%u.%u.%u", _ip[0], _ip[1], _ip[2], _ip[3]);
    } else {
        safeStrCopy(buf, bufSize, "0.0.0.0");
    }
}

// ============================================================================
// КОНФИГУРАЦИЯ СЕТИ (ex-ConfigManager::applyNetworkSettings)
// ============================================================================
void NetworkService::applyNetConfig() {
    if (cfgGetBool("net.dhcp", true)) return;   // DHCP — по умолчанию

    char ip[CFG_VALUE_LEN], mask[CFG_VALUE_LEN];
    char gw[CFG_VALUE_LEN], dns[CFG_VALUE_LEN];
    cfgGetStr("net.ip", ip, sizeof(ip), "");
    cfgGetStr("net.mask", mask, sizeof(mask), "");
    cfgGetStr("net.gateway", gw, sizeof(gw), "");
    cfgGetStr("net.dns", dns, sizeof(dns), "");

    IPAddress lip, lmask, lgw, ldns;
    if (!lip.fromString(ip) || !lmask.fromString(mask)) {
        log(LogLevel::Error, "static config invalid, fallback to DHCP");
        return;
    }
    lgw.fromString(gw);
    ldns.fromString(dns);
    // ETH.config ДО ETH.begin — адрес применится при поднятии интерфейса
    ETH.config(lip, lgw, lmask, ldns);
    log(LogLevel::Info, "static IP configured: %s", ip);
}

// ============================================================================
// КОНТРОЛЬ ШЛЮЗА (ex-ПАЗ): esp_ping, одна сессия за раз
// ============================================================================
void NetworkService::startGatewayPing() {
    char gw[CFG_VALUE_LEN];
    cfgGetStr("net.gateway", gw, sizeof(gw), "");
    if (gw[0] == '\0') return;

    ip_addr_t target;
    if (ipaddr_aton(gw, &target) == 0) {
        log(LogLevel::Warning, "gateway '%s' unparsable, ping skipped", gw);
        return;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count       = NET_PING_COUNT;
    cfg.interval_ms = 200;
    cfg.timeout_ms  = NET_PING_TIMEOUT_MS;
    cfg.data_size   = 32;
    cfg.target_addr = target;

    esp_ping_callbacks_t cbs = {};
    cbs.cb_args         = this;
    cbs.on_ping_success = nmPingSuccessTrampoline;
    cbs.on_ping_timeout = nmPingTimeoutTrampoline;
    cbs.on_ping_end     = nmPingEndTrampoline;

    _pingReplies = 0;
    _pingEnded   = false;
    esp_ping_handle_t hdl = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
        log(LogLevel::Warning, "ping session create failed");
        return;
    }
    if (esp_ping_start(hdl) != ESP_OK) {
        // Старт не удался — сессию удаляем сами, иначе _pingActive
        // зависнет навсегда и контроль шлюза молча умрёт.
        esp_ping_delete_session(hdl);
        log(LogLevel::Warning, "ping start failed");
        return;
    }
    _pingActive = true;
    _lastPingMs = millis();   // завершение придёт в on_ping_end -> _pingEnded
}

void NetworkService::finishGatewayPing() {
    _pingActive = false;
    // Сессия, завершившаяся при уже упавшем link/IP, — не вердикт о шлюзе:
    // деградацией заведует AUTONOMOUS, события GATEWAY_* здесь — шум.
    if (!isConnected()) {
        _gwFailStreak = 0;
        return;
    }
    bool ok = (_pingReplies > 0);
    if (ok) {
        _gwRttMs = _pingLastRtt;
        _gwFailStreak = 0;
        if (!_gwOk) {
            _gwOk = true;
            ShEventData d; d.clear();
            d.code = (int32_t)_gwRttMs;
            postEvent(NET_EVENT_GATEWAY_RESTORED, &d);
            log(LogLevel::Info, "gateway restored, rtt=%lu ms",
                (unsigned long)_gwRttMs);
        }
    } else {
        if (_gwFailStreak < 255) _gwFailStreak++;
        // Гистерезис: LOST только после NET_GW_FAIL_THRESHOLD сессий
        // подряд — одиночный потерянный пинг не шатает систему.
        if (_gwOk && _gwFailStreak >= NET_GW_FAIL_THRESHOLD) {
            _gwOk = false;
            ShEventData d; d.clear();
            d.code = _gwFailStreak;
            postEvent(NET_EVENT_GATEWAY_LOST, &d);
            log(LogLevel::Warning, "gateway LOST after %u failed sessions",
                _gwFailStreak);
        }
    }
}

// ============================================================================
// A3: УРОВНИ ДЕГРАДАЦИИ
// ============================================================================
void NetworkService::evaluateDegradation() {
    DegradationLevel want;
    if (!_netEnabled || !_linkUp || !_hasIp) {
        want = DegradationLevel::Autonomous;
    } else if (!_gwOk) {
        want = DegradationLevel::LocalNet;
    } else {
        want = DegradationLevel::Full;
    }

    if (want != _level) {
        _level = want;
        ShEventData d; d.clear();
        d.code = (int32_t)want;
        safeStrCopy(d.payload, sizeof(d.payload),
                    want == DegradationLevel::Full       ? "FULL" :
                    want == DegradationLevel::LocalNet   ? "LOCAL_NET" :
                                                           "AUTONOMOUS");
        postEvent(SH_EVENT_DEGRADED_LEVEL, &d);
        log(LogLevel::Info, "degradation level -> %s", d.payload);
    }
}
