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
        { "broker.pass", ConfigType::SECRET, "", 0, 0, CFG_SECRET | CFG_CRITICAL,
          "MQTT-брокер", "Пароль (только NVS; сверяется, если задан логин)" },
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

    // === ГРУППА: Погодное зеркало W4 ==================================
    // Зеркало показаний weather_gate на мастере (глубокая история на SD).
    // Пустой топик = выключено. Два канала приёма, оба без новых сокетов:
    // подписка на вышестоящем транспорте (норма) + хук локального брокера
    // (автономия). ПОЛЯ — ТОЛЬКО В КОНЕЦ СХЕМЫ (правило совместимости).
    cfg.addFields("Погодное зеркало", {
        { "wx.mirror_topic", ConfigType::STRING, "", 0, 0, CFG_CRITICAL,
          "Погодное зеркало", "Топик погоды шлюза (напр. microos/weather_gate/weather; пусто = выкл)" },
        { "wx.mirror_local", ConfigType::BOOL, "true", 0, 0, CFG_CRITICAL,
          "Погодное зеркало", "Слушать и локальный брокер (in-proc, автономный режим)" },
        { "wx.mirror_dlog", ConfigType::BOOL, "true", 0, 0, CFG_CRITICAL,
          "Погодное зеркало", "Писать каналы wx_* в DataLog (глубокая история на SD)" },
    });

    // === ГРУППА: Бэкапы (M3.3 BackupAggregator, 04.09.2026) ============
    // Мастер собирает ПОЛНЫЕ снимки конфигов устройств парка (включая
    // секреты — решение владельца 04.09: контур проводной/доверенный,
    // флэш и так хранит их открыто) на SD: /backup/<ip>/config-*.json.
    // Поля живые (CFG_NONE): смена расписания/списка — без ребута.
    // ПОЛЯ — ТОЛЬКО В КОНЕЦ СХЕМЫ (правило совместимости).
    cfg.addFields("Бэкапы", {
        { "bk.enabled", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Бэкапы", "Собирать снимки конфигов парка на SD" },
        { "bk.hosts", ConfigType::STRING, "10.146.75.53,10.146.75.55", 0, 0, CFG_NONE,
          "Бэкапы", "IP устройств парка через запятую (без мастера)" },
        { "bk.admin_pin", ConfigType::SECRET, "", 0, 0, CFG_SECRET | CFG_NONE,
          "Бэкапы", "Пароль администратора устройств парка (единый, только NVS)" },
        { "bk.period_days", ConfigType::UINT, "7", 1, 90, CFG_NONE,
          "Бэкапы", "Период планового съёма, дней" },
        { "bk.keep", ConfigType::UINT, "10", 2, 60, CFG_NONE,
          "Бэкапы", "Сколько снимков хранить на устройство (ротация)" },
        { "bk.on_change", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Бэкапы", "Внеплановый снимок через ~5 мин после смены конфига устройства" },
    });

    // === ГРУППА: OTA-зеркало (0.6.1, 05.09.2026) ======================
    // Мастер зеркалит прошивки парка с HA (свой mqtt.host:8123) на SD
    // (/ota/<hostname>/) и раздаёт их устройствам вторым WebServer на
    // :8123 — устройства и так спрашивают мастера (их mqtt.host = мастер,
    // порт зашит в ядре). Цепочка замыкается: и прошивка, и бэкап — на
    // мастере, адрес HA устройствам не нужен. Поля живые (CFG_NONE).
    // ПОЛЯ — ТОЛЬКО В КОНЕЦ СХЕМЫ (правило совместимости).
    cfg.addFields("OTA-зеркало", {
        { "otam.enabled", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "OTA-зеркало", "Зеркалировать прошивки парка с HA на SD и раздавать по LAN" },
        { "otam.hosts", ConfigType::STRING, "smart_lock,weather_gate", 0, 0, CFG_NONE,
          "OTA-зеркало", "Hostname-ы устройств парка через запятую (sys.hostname)" },
        { "otam.period_h", ConfigType::UINT, "6", 1, 168, CFG_NONE,
          "OTA-зеркало", "Период опроса HA на новые версии, часов" },
        { "otam.src", ConfigType::STRING, "", 0, 0, CFG_NONE,
          "OTA-зеркало", "Источник-переопределение (пусто = HA; напр. http://192.168.1.10:8123)" },
    });

    // === ГРУППА: Статус-LED (M5, 08.09.2026) ==========================
    // 4 пикселя WS2812 = лицо ПАЗ на железе. По умолчанию ВЫКЛЮЧЕНО:
    // нет ленты — модуль молчит. Распиновка утверждена владельцем
    // 08.09.2026 (GPIO21, питание 3.3V, ~470 Ом на data).
    cfg.addFields("Статус-LED", {
        { "led.enabled", ConfigType::BOOL, "false", 0, 0, CFG_CRITICAL,
          "Статус-LED", "Лента WS2812 на мастере (ребут)" },
        { "led.pin", ConfigType::UINT, "21", 0, 48, CFG_CRITICAL,
          "Статус-LED", "GPIO data (21 — свободен, см. дорожную карту M5)" },
        { "led.count", ConfigType::UINT, "4", 1, 8, CFG_CRITICAL,
          "Статус-LED", "Пикселей в ленте" },
        { "led.bright", ConfigType::UINT, "15", 1, 100, CFG_CRITICAL,
          "Статус-LED", "Яркость, % (статус, не гирлянда)" },
        { "led.map", ConfigType::STRING, "hm.sd,hm.bk,hm.ota,hm.fleet", 0, 0, CFG_CRITICAL,
          "Статус-LED", "Проверка i-го пикселя через запятую (пусто = погашен)" },
    });

    // === ГРУППА: Экран (M5, 08.09.2026) ===============================
    // OLED SSD1306 на I2C-шине платы (16/17, та же шина, что DS3231) +
    // 3 кнопки. Экран всегда погашен, будит любая кнопка. По умолчанию
    // ВЫКЛЮЧЕНО: нет дисплея — модуль молчит.
    cfg.addFields("Экран", {
        { "oled.enabled", ConfigType::BOOL, "false", 0, 0, CFG_CRITICAL,
          "Экран", "OLED SSD1306 128x64 на мастере (ребут)" },
        { "oled.addr", ConfigType::UINT, "60", 3, 119, CFG_CRITICAL,
          "Экран", "I2C-адрес (60 = 0x3C)" },
        { "oled.timeout", ConfigType::UINT, "30", 5, 300, CFG_CRITICAL,
          "Экран", "Гашение после последнего нажатия, с" },
        { "btn.plus", ConfigType::UINT, "15", 0, 48, CFG_CRITICAL,
          "Экран", "GPIO кнопки «+»" },
        { "btn.minus", ConfigType::UINT, "18", 0, 48, CFG_CRITICAL,
          "Экран", "GPIO кнопки «−»" },
        { "btn.ok", ConfigType::UINT, "2", 0, 48, CFG_CRITICAL,
          "Экран", "GPIO кнопки «ОК»" },
    });
}
