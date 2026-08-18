// ============================================================================
// HomeMasterConfig.cpp — поля master.* / sd.* (см. шапку .h)
// ============================================================================
#include "HomeMasterConfig.h"
#include <services/ConfigService.h>

void registerHomeMasterConfig() {
    ConfigService& cfg = ConfigService::getInstance();

    // === ГРУППА: Мастер ===============================================
    // Режимы (концепция, §3): auto — детект вышестоящего брокера (M2),
    // solo — всегда свой брокер, bridge — всегда транслятор. На M0 поле
    // фиксируется в конфиге и журналируется, переключение — с M2.
    cfg.addFields("Мастер", {
        { "master.mode", ConfigType::STRING, "auto", 0, 0, CFG_CRITICAL,
          "Мастер", "Режим: auto / solo / bridge (bridge — с M2)" },
        { "master.upstream_host", ConfigType::STRING, "", 0, 0, CFG_CRITICAL,
          "Мастер", "Вышестоящий брокер (host/IP, пусто = детект)" },
        { "master.upstream_port", ConfigType::UINT, "1883", 1, 65535, CFG_CRITICAL,
          "Мастер", "Порт вышестоящего брокера" },
    });

    // === ГРУППА: MQTT-брокер (M1, spike) ===============================
    // Резервный брокер параноидального контура. По умолчанию ВЫКЛЮЧЕН:
    // код слинкован, сокет не слушается. Включение — только осознанно,
    // на стенде, с замером heap (протокол spike'а — в дорожной карте).
    cfg.addFields("MQTT-брокер", {
        { "broker.enabled", ConfigType::BOOL, "false", 0, 0, CFG_CRITICAL,
          "MQTT-брокер", "Встроенный брокер (M1 spike, требует ребут)" },
        { "broker.port", ConfigType::UINT, "1883", 1, 65535, CFG_CRITICAL,
          "MQTT-брокер", "Порт прослушивания" },
        { "broker.max_clients", ConfigType::UINT, "12", 1, 64, CFG_CRITICAL,
          "MQTT-брокер", "Потолок клиентов (стенд 07.08: 16 сокетов lwIP = 12 + 4 служебных)" },
        { "broker.user", ConfigType::STRING, "", 0, 0, CFG_CRITICAL,
          "MQTT-брокер", "Логин (пусто = анонимный доступ)" },
        { "broker.pass", ConfigType::STRING, "", 0, 0, CFG_CRITICAL,
          "MQTT-брокер", "Пароль (сверяется, только если задан логин)" },
    });

    // === ГРУППА: Мост M2 ==============================================
    // Транслятор «локальный брокер ↔ вышестоящий» (дизайн — концепция,
    // блок M2). Активен, когда брокер слушает и режим не solo. In-proc,
    // сокетов не ест. Офлайн-политика и фильтры — в шапке BridgeService.h.
    cfg.addFields("Мост M2", {
        { "bridge.enabled", ConfigType::BOOL, "true", 0, 0, CFG_CRITICAL,
          "Мост M2", "Транслятор локальный↔вышестоящий (ребут)" },
        { "bridge.synth_offline", ConfigType::BOOL, "true", 0, 0, CFG_CRITICAL,
          "Мост M2", "Синтез offline при отвале устройства (LWT моста)" },
        { "bridge.down_extra", ConfigType::STRING, "", 0, 0, CFG_CRITICAL,
          "Мост M2", "Whitelist топиков-вниз через запятую (напр. smart_lock/weather)" },
    });

    // === ГРУППА: SD-карта =============================================
    cfg.addFields("SD-карта", {
        { "sd.enabled", ConfigType::BOOL, "true", 0, 0, CFG_CRITICAL,
          "SD-карта", "Использовать microSD (журнал/бэкапы/OTA-репо)" },
        { "sd.freq_mhz", ConfigType::UINT, "20", 4, 40, CFG_CRITICAL,
          "SD-карта", "Частота SPI карты, МГц (снизить при нестабильности)" },
    });

    // === ГРУППА: Журнал M3 ============================================
    // Долгий журнал парка на SD (концепция §4.2 EventJournal). Фильтр —
    // MQTT-маски через запятую (журнал — читатель, маски разрешены; в
    // отличие от bridge.down_extra это не граница безопасности). Дефолт:
    // события парка + живость устройств, телеметрия НЕ журналируется.
    cfg.addFields("Журнал M3", {
        { "journal.enabled", ConfigType::BOOL, "true", 0, 0, CFG_CRITICAL,
          "Журнал M3", "Вести журнал событий парка на SD" },
        { "journal.topics", ConfigType::STRING, "microos/+/events/#,microos/+/state", 0, 0, CFG_CRITICAL,
          "Журнал M3", "Фильтр топиков (маски MQTT, через запятую; пусто = молчит)" },
        { "journal.flush_s", ConfigType::UINT, "5", 1, 60, CFG_CRITICAL,
          "Журнал M3", "Период сброса кольца на SD, с (батч + fsync)" },
        { "journal.max_mb", ConfigType::UINT, "100", 1, 500, CFG_CRITICAL,
          "Журнал M3", "Ротация сегмента по размеру, МБ" },
        { "journal.max_days", ConfigType::UINT, "90", 1, 3650, CFG_CRITICAL,
          "Журнал M3", "Глубина архива, дней (старое удаляется)" },
    });
}
