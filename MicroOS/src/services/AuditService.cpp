// ============================================================================
// AuditService.cpp — реализация аудит-журнала
// ============================================================================
#include "AuditService.h"
#include "ConfigService.h"
#include "StorageService.h"
#include "TimeService.h"
#include "../core/Events.h"

// Спинлок очереди (onEvent — контекст диспетчера шины, ждать нельзя)
static portMUX_TYPE s_auditMux = portMUX_INITIALIZER_UNLOCKED;

AuditService& AuditService::getInstance() {
    static AuditService instance;
    return instance;
}

// ============================================================================
// КУРАТОРСКИЙ СПИСОК: какие события юридически значимы
// ============================================================================
const char* AuditService::eventName(int32_t eventId) {
    switch (eventId) {
        // Аутентификация (C1/C3) — кто и когда вошёл, перебор, блокировки
        case AUTH_EVENT_LOGIN:           return "AUTH_LOGIN";
        case AUTH_EVENT_LOGIN_FAILED:    return "AUTH_FAILED";
        case AUTH_EVENT_LOCKED_OUT:      return "AUTH_LOCKOUT";
        case AUTH_EVENT_SETUP_REQUIRED:  return "AUTH_SETUP";
        // Конфигурация — любая смена параметра (payload: ключ)
        case CFG_EVENT_CHANGED:          return "CFG_CHANGED";
        case CFG_EVENT_MIGRATED:         return "CFG_MIGRATED";
        // Доступ (СКУД) — кого пустили/не пустили, состояние замка
        case ACCESS_EVENT_GRANTED:       return "ACCESS_GRANTED";
        case ACCESS_EVENT_DENIED:        return "ACCESS_DENIED";
        case ACCESS_EVENT_LOCKED:        return "LOCK_LOCKED";
        case ACCESS_EVENT_UNLOCKED:      return "LOCK_UNLOCKED";
        // Живучесть системы — аварийные режимы и обновления
        case SH_EVENT_SAFE_MODE_ENTERED: return "SAFE_MODE";
        case SH_EVENT_BOOTLOOP_DETECTED: return "BOOTLOOP";
        case SH_EVENT_DEGRADED_LEVEL:    return "DEGRADED";
        case OTA_EVENT_STARTED:          return "OTA_START";
        case OTA_EVENT_SUCCESS:          return "OTA_SUCCESS";
        case OTA_EVENT_FAILED:           return "OTA_FAILED";
        case OTA_EVENT_ROLLBACK:         return "OTA_ROLLBACK";
        case HEALTH_EVENT_WDT_REBOOT:    return "WDT_REBOOT";
        // Сеть и хранилище — внешние условия инцидентов
        case NET_EVENT_CONNECTED:        return "NET_UP";
        case NET_EVENT_DISCONNECTED:     return "NET_DOWN";
        case NET_EVENT_DISABLED:         return "NET_DISABLED";
        case STORAGE_EVENT_LOW_SPACE:    return "STORAGE_LOW";
        case STORAGE_EVENT_CORRUPTED:    return "STORAGE_CORRUPT";
        default:                         return nullptr;
    }
}

bool AuditService::canHandleEvent(int32_t eventId) const {
    return eventName(eventId) != nullptr;
}

// ============================================================================
// КОНФИГ-СХЕМА
// ============================================================================
void AuditService::registerExtensions() {
    ConfigService::getInstance().addFields("Аудит", {
        { "audit.enabled", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Аудит", "Вести аудит-журнал /audit.log" },
    });
}

void AuditService::init() {
    _initialized = true;
    repairFileTail();
    log(LogLevel::Info, "init: audit queue ready");
}

// ============================================================================
// РЕМОНТ ХВОСТА ПОСЛЕ РЕБУТА ПОСРЕДИ APPEND (5.8.4)
// Битый хвост — не переполнение буфера (строка ~70-90 Б при лимите 160),
// а обрыв appendFile ребутом/питанием: LittleFS сохраняет ФС, но не
// атомарность строки. Парсеры JSON-lines спотыкаются о такой хвост.
// ============================================================================
void AuditService::repairFileTail() {
    StorageService& st = StorageService::getInstance();
    size_t sz = st.fileSize(AUDIT_FILE_PATH);
    if (sz == 0) return;

    // Хвост: строка длиннее AUDIT_LINE_LEN быть не может — окна хватит
    size_t win = (sz < AUDIT_LINE_LEN) ? sz : (size_t)AUDIT_LINE_LEN;
    size_t off = sz - win;
    char buf[AUDIT_LINE_LEN];
    File f = st.openRead(AUDIT_FILE_PATH);
    if (!f) return;
    f.seek(off);
    size_t got = f.read((uint8_t*)buf, win);
    f.close();
    if (got == 0 || buf[got - 1] == '\n') return;   // хвост цел

    int lastNl = -1;
    for (int i = (int)got - 1; i >= 0; --i) {
        if (buf[i] == '\n') { lastNl = i; break; }
    }
    // Нет '\n' во всём окне — строка битая целиком, отрезаем всё окно
    size_t keepBytes = (lastNl >= 0) ? off + (size_t)lastNl + 1 : off;

    File in  = st.openRead(AUDIT_FILE_PATH);
    File out = st.openTemp(AUDIT_FILE_PATH);
    if (!in || !out) {
        if (in) in.close();
        if (out) out.close();
        log(LogLevel::Error, "audit tail repair: не открыть файлы");
        return;
    }
    size_t left = keepBytes;
    char chunk[128];
    while (left > 0) {
        size_t want = (left < sizeof(chunk)) ? left : sizeof(chunk);
        size_t n = in.read((uint8_t*)chunk, want);
        if (n == 0) break;
        out.write((const uint8_t*)chunk, n);
        left -= n;
    }
    in.close();
    out.close();
    st.commitTemp(AUDIT_FILE_PATH);
    log(LogLevel::Info, "audit tail repaired: срезано %u байт битого хвоста",
        (unsigned)(sz - keepBytes));
}

void AuditService::start() {
    // Подписка строго по кураторскому списку — никаких "всех событий"
    EventBus& bus = EventBus::getInstance();
    static const int32_t EVENTS[] = {
        AUTH_EVENT_LOGIN, AUTH_EVENT_LOGIN_FAILED, AUTH_EVENT_LOCKED_OUT,
        AUTH_EVENT_SETUP_REQUIRED,
        CFG_EVENT_CHANGED, CFG_EVENT_MIGRATED,
        ACCESS_EVENT_GRANTED, ACCESS_EVENT_DENIED,
        ACCESS_EVENT_LOCKED, ACCESS_EVENT_UNLOCKED,
        SH_EVENT_SAFE_MODE_ENTERED, SH_EVENT_BOOTLOOP_DETECTED,
        SH_EVENT_DEGRADED_LEVEL,
        OTA_EVENT_STARTED, OTA_EVENT_SUCCESS, OTA_EVENT_FAILED,
        OTA_EVENT_ROLLBACK, HEALTH_EVENT_WDT_REBOOT,
        NET_EVENT_CONNECTED, NET_EVENT_DISCONNECTED, NET_EVENT_DISABLED,
        STORAGE_EVENT_LOW_SPACE, STORAGE_EVENT_CORRUPTED,
    };
    for (size_t i = 0; i < sizeof(EVENTS) / sizeof(EVENTS[0]); ++i) {
        bus.subscribe(EVENTS[i], this);
    }
    _started = true;
    log(LogLevel::Info, "started: %u curated events",
        (unsigned)(sizeof(EVENTS) / sizeof(EVENTS[0])));
}

void AuditService::stop() {
    EventBus::getInstance().unsubscribeAll(this);
    _started = false;
}

// ============================================================================
// ГОРЯЧИЙ КОНТУР: событие -> JSON-строка в очередь (диспетчер шины!)
// ============================================================================
void AuditService::onEvent(int32_t eventId, const ShEventData* data) {
    if (!cfgGetBool("audit.enabled", true)) return;
    const char* name = eventName(eventId);
    if (name == nullptr) return;
    // 5.8.1: та же правка направления, что в MQTT-зеркале — возврат
    // уровня сети в FULL пишется RECOVERED, а не DEGRADED.
    if (eventId == SH_EVENT_DEGRADED_LEVEL && data != nullptr &&
        strcmp(data->payload, "FULL") == 0) {
        name = "RECOVERED";
    }

    // Время: unix от TimeService, если достоверно; иначе 0 (потребитель
    // смотрит на "up" — uptime всегда есть). Формат JSON-lines:
    // {"ts":1738...,"up":123456,"ev":"AUTH_LOGIN","code":1,"info":"web"}
    time_t unix = TimeService::getInstance().getUnixTime();

    // Санитизация payload: кавычки ломают JSON — заменяем на апостроф
    char info[SH_EVENT_PAYLOAD_SIZE];
    const char* src = (data && data->payload[0]) ? data->payload : "";
    size_t i = 0;
    for (; src[i] && i < sizeof(info) - 1; ++i) {
        info[i] = (src[i] == '"') ? '\'' : src[i];
    }
    info[i] = '\0';

    char line[AUDIT_LINE_LEN];
    snprintf(line, sizeof(line),
             "{\"ts\":%lu,\"up\":%lu,\"ev\":\"%s\",\"code\":%ld,\"info\":\"%s\"}\n",
             (unsigned long)unix,
             (unsigned long)(data ? data->timestampMs : millis()),
             name,
             (long)(data ? data->code : 0),
             info);
    enqueue(line);
}

void AuditService::enqueue(const char* line) {
    portENTER_CRITICAL(&s_auditMux);
    uint8_t next = (uint8_t)((_qHead + 1) % AUDIT_QUEUE_SIZE);
    if (next == _qTail) {
        // Очередь полна: теряем НОВУЮ запись (старые важнее — они первыми
        // уйдут в файл, хронология не перевернётся) + счётчик для B1.
        _overflows++;
        portEXIT_CRITICAL(&s_auditMux);
        return;
    }
    safeStrCopy(_queue[_qHead], AUDIT_LINE_LEN, line);
    _qHead = next;
    portEXIT_CRITICAL(&s_auditMux);
}

// ============================================================================
// ХОЛОДНЫЙ КОНТУР: очередь -> файл (свой tick)
// ============================================================================
void AuditService::tick() {
    for (uint8_t n = 0; n < AUDIT_FLUSH_PER_TICK; ++n) {
        char line[AUDIT_LINE_LEN];
        portENTER_CRITICAL(&s_auditMux);
        if (_qTail == _qHead) {
            portEXIT_CRITICAL(&s_auditMux);
            break;
        }
        safeStrCopy(line, sizeof(line), _queue[_qTail]);
        _qTail = (uint8_t)((_qTail + 1) % AUDIT_QUEUE_SIZE);
        portEXIT_CRITICAL(&s_auditMux);

        // Ротация перед записью, если файл перерос
        if (StorageService::getInstance().fileSize(AUDIT_FILE_PATH)
                >= AUDIT_FILE_MAX) {
            StorageService::getInstance().rotate(AUDIT_FILE_PATH,
                                                 AUDIT_FILE_KEEP);
            log(LogLevel::Info, "audit file rotated");
        }

        if (!StorageService::getInstance().appendFile(AUDIT_FILE_PATH, line)) {
            publishError("AUDIT_WRITE");
            break;   // остаток — в следующий tick
        }
        _total++;
    }
}
