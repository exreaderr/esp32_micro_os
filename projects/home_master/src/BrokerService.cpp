// ============================================================================
// BrokerService.cpp — встроенный MQTT-брокер мастера (M1, spike)
// ============================================================================
#include "BrokerService.h"
#include <services/ConfigService.h>
#include <third_party/smqtt/sMQTTBroker.h>

// ============================================================================
// АДАПТЕР ДВИЖКА: все колбэки sMQTT прилетают сюда и передаются сервису.
// onEvent вызывается из tick() (кооперативно, контекст loopTask) —
// тяжёлое и FS здесь ЗАПРЕЩЕНО, только счётчики и лог.
// ============================================================================
namespace {
class MicroOsBroker : public sMQTTBroker {
public:
    bool onEvent(sMQTTEvent* event) override {
        return BrokerService::getInstance().handleBrokerEvent(event);
    }
};
MicroOsBroker g_broker;   // статикой: никакой кучи под сам движок
}

BrokerService& BrokerService::getInstance() {
    static BrokerService instance;
    return instance;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
void BrokerService::init() {
    _enabled    = cfgGetBool("broker.enabled", false);
    _port       = (uint16_t)cfgGetUInt("broker.port", 1883);
    _maxClients = (uint16_t)cfgGetUInt("broker.max_clients", 12);
    cfgGetStr("broker.user", _user, sizeof(_user), "");
    cfgGetStr("broker.pass", _pass, sizeof(_pass), "");
    if (_maxClients == 0 || _maxClients > ACCEPT_TABLE_MAX) _maxClients = ACCEPT_TABLE_MAX;

    if (!_enabled) {
        // Штатный режим M1-spike до стенда: код слинкован, но сокет не
        // слушаем. Включение — ключом broker.enabled через админку + ребут.
        log(LogLevel::Info, "broker: disabled (broker.enabled=false, spike M1 standby)");
        _initialized = true;
        return;
    }

    // Замер spike'а: heap до/после создания слушающего сокета = цена
    // «нулевого» брокера без единого клиента.
    uint32_t heapBefore = ESP.getFreeHeap();
    bool ok = g_broker.init(_port, /*checkWifiConnection=*/false);
    logHeap(ok ? "listening socket up" : "LISTEN FAILED");
    if (!ok) {
        publishError("broker_listen");
        _initialized = true;   // модуль отработал: деградация, не паника
        return;
    }
    _running = true;
    log(LogLevel::Info,
        "broker: MQTT 3.1.1 (sMQTT, vendored) on :%u, max_clients=%u, auth=%s, socket cost %d B",
        _port, _maxClients, _user[0] ? "user/pass" : "anonymous",
        (int)(heapBefore - ESP.getFreeHeap()));
    _initialized = true;
}

void BrokerService::start() {
    _started = true;
    if (_running)
        log(LogLevel::Info, "broker: accepting clients (M1 принят: ёмкость контура 12, стена — 16 сокетов lwIP ядра)");
}

void BrokerService::stop() {
    // Движок не имеет shutdown: перестаём обслуживать сокеты; lwIP
    // рассчистит TCP при ребуте. Для spike'а достаточно.
    _running = false;
    _started = false;
}

void BrokerService::tick() {
    if (!_running) return;
    g_broker.update();

    // Soak-телеметрия: раз в минуту, только когда есть живые клиенты.
    if (_clients > 0 && millis() - _lastStatsMs > STATS_INTERVAL_MS) {
        _lastStatsMs = millis();
        logHeap("soak stats");
    }
}

// ============================================================================
// СОБЫТИЯ ДВИЖКА
// ============================================================================
bool BrokerService::handleBrokerEvent(sMQTTEvent* event) {
    switch (event->Type()) {
        case NewClient_sMQTTEventType: {
            auto* e = (sMQTTNewClientEvent*)event;

            // Аутентификация: задан broker.user — сверяем пару полностью.
            if (_user[0]) {
                bool oku = (e->Login() == _user);
                bool okp = (e->Password() == _pass);
                if (!oku || !okp) {
                    _rejected++;
                    log(LogLevel::Warning, "broker: REJECT auth (user='%s'), total rejected %lu",
                        e->Login().c_str(), (unsigned long)_rejected);
                    return false;   // CONNACK 0x04 + закрытие сокета движком
                }
            }

            // Потолок клиентов (в апстриме его нет — наш контурный предел).
            if (_clients >= _maxClients) {
                _rejected++;
                log(LogLevel::Warning, "broker: REJECT cap %u reached, total rejected %lu",
                    _maxClients, (unsigned long)_rejected);
                return false;
            }

            for (uint8_t i = 0; i < ACCEPT_TABLE_MAX; i++) {
                if (_accepted[i] == nullptr) { _accepted[i] = e->Client(); break; }
            }
            _clients++;

            // ГЛАВНЫЙ ЗАМЕР SPIKE'А: дельта heap между строками connect =
            // стоимость одного MQTT-подключения (сокет lwIP + sMQTTClient +
            // std::string'и). На стенде читается прямо из журнала.
            char what[48];
            snprintf(what, sizeof(what), "client #%u connected", _clients);
            logHeap(what);
            return true;
        }

        case RemoveClient_sMQTTEventType: {
            auto* e = (sMQTTRemoveClientEvent*)event;
            bool known = false;
            for (uint8_t i = 0; i < ACCEPT_TABLE_MAX; i++) {
                if (_accepted[i] == e->Client()) { _accepted[i] = nullptr; known = true; break; }
            }
            if (known && _clients > 0) {
                _clients--;
                char what[48];
                snprintf(what, sizeof(what), "client left, %u active", _clients);
                logHeap(what);
            }
            // Хуки (мост M2, журнал M3): ушёл ПРИНЯТЫЙ клиент — для синтеза
            // offline наверху (sMQTT LWT не исполняет). Клиент жив до конца
            // события — id валиден.
            if (known) {
                BrokerEventInfo info{};
                info.type = BrokerEventInfo::RemoveClient;
                strncpy(info.clientId, e->Client()->getClientId().c_str(),
                        sizeof(info.clientId) - 1);
                fireHooks(info);
            }
            // Отвергнутые/недожавшие CONNECT в таблице не числятся — молчим.
            return true;
        }

        case Public_sMQTTEventType: {
            _rxTotal++;
            // Хуки (мост M2, журнал M3): тема+тело+retain (патч движка #3).
            // Тело длиннее бюджета события помечается truncated — мост такое
            // отбросит, журнал усечёт с cut-флагом; наверх усечённое НИКОГДА.
            {
                auto* e = (sMQTTPublicClientEvent*)event;
                BrokerEventInfo info{};
                info.type = BrokerEventInfo::Publish;
                std::string t = e->Topic();
                std::string p = e->Payload();
                strncpy(info.topic, t.c_str(), sizeof(info.topic) - 1);
                info.truncated = (p.size() >= sizeof(info.payload));
                strncpy(info.payload, p.c_str(), sizeof(info.payload) - 1);
                info.retain = e->Retain();
                // 5.5.3: идентичность автора — мост учит по ней availability-
                // топик клиента (client_id=hostname ≠ топиковый MAC-id,
                // стенд 08.08: synth ушёл в чужой топик). Клиент жив до
                // конца события — id валиден (как и в RemoveClient).
                strncpy(info.clientId, e->Client()->getClientId().c_str(),
                        sizeof(info.clientId) - 1);
                fireHooks(info);
            }
            return true;
        }

        case Subscribe_sMQTTEventType:
        case UnSubscribe_sMQTTEventType:
        case LostConnect_sMQTTEventType:
        default:
            return true;
    }
}

// ============================================================================
// СОСТОЯНИЕ / ПУБЛИКАЦИЯ ИЗ ЯДРА
// ============================================================================
uint32_t BrokerService::retained() const {
    // g_broker — глобальный объект анонимного namespace, константность
    // *this на него не распространяется.
    return _running ? (uint32_t)g_broker.getRetainedTopicCount() : 0;
}

bool BrokerService::hasClient(const char* clientId) const {
    if (!_running || clientId == nullptr || clientId[0] == '\0') return false;
    // Своя таблица принятых сессий — vendored-движок не трогаем (уходящая
    // сессия уже удалена из таблицы до вызова хуков — см. RemoveClient).
    for (uint8_t i = 0; i < ACCEPT_TABLE_MAX; ++i) {
        auto* c = (sMQTTClient*)_accepted[i];
        if (c != nullptr && c->isConnected() &&
            strcmp(c->getClientId().c_str(), clientId) == 0) {
            return true;
        }
    }
    return false;
}

bool BrokerService::publishLocal(const char* topic, const char* payload, bool retain) {
    if (!_running || !topic || !payload) return false;
    g_broker.publish(std::string(topic), std::string(payload), 0, retain);
    return true;
}

bool BrokerService::addEventHook(BrokerEventHook h) {
    if (h == nullptr) return false;
    for (uint8_t i = 0; i < BROKER_HOOK_MAX; ++i)
        if (_hooks[i] == h) return true;            // уже есть — идемпотентно
    for (uint8_t i = 0; i < BROKER_HOOK_MAX; ++i) {
        if (_hooks[i] == nullptr) { _hooks[i] = h; return true; }
    }
    log(LogLevel::Error, "broker: hook slots full (%u)", BROKER_HOOK_MAX);
    return false;
}

void BrokerService::removeEventHook(BrokerEventHook h) {
    for (uint8_t i = 0; i < BROKER_HOOK_MAX; ++i)
        if (_hooks[i] == h) _hooks[i] = nullptr;
}

void BrokerService::logHeap(const char* what) const {
    log(LogLevel::Info,
        "broker: %s | clients=%u retained=%lu rx=%lu | heap=%lu B, watermark=%lu B",
        what, _clients, (unsigned long)retained(), (unsigned long)_rxTotal,
        (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
}
