// ============================================================================
// Events.h — ЕДИНЫЙ РЕЕСТР СОБЫТИЙ МикроОС 5.0
// ============================================================================
// Фаза 0. Единственный файл, где объявляются СИСТЕМНЫЕ идентификаторы
// событий. Профильные события объявляет профиль, получив диапазон через
// ResourceManager::claimEventRange() — коллизии исключены по построению.
//
// Устранённые проблемы v4.2.2 (зафиксированы в базовой архитектуре, П5):
//   · SH_EVENT_TEMP_WARNING (0x0600) совпадал с SH_EVENT_MQTT_CONNECTED;
//   · SH_EVENT_MODULE_STATUS (0x1000) совпадал с USER_BASE и
//     CONFIG_EVENT_CHANGED;
//   · дубли по смыслу: TIME_SYNC vs RTC_SYNCED, TEMPERATURE_UPDATE vs
//     TEMP_UPDATE — здесь у каждого факта ОДНО событие-владелец.
//
// Карта диапазонов:
//   0x0000–0x00FF  Ядро и системные
//   0x0100–0x01FF  Системные сервисы (по 0x10 на сервис)
//   0x0200–0x02FF  Транспорт
//   0x0300–0x03FF  Драйверы
//   0x0400–0x04FF  Доменные сервисы
//   0x0900–0x09FF  Команды (адресное взаимодействие модулей)
//   0x1000–0x1FFF  Приложения профилей
// ============================================================================
#pragma once

#include <cstdint>

// ============================================================================
// ЯДРО (0x0000–0x00FF) — владелец: Kernel
// ============================================================================
enum ShSysEvents : int32_t {
    SH_EVENT_BOOT               = 0x0000,  // система загрузилась (payload: причина)
    SH_EVENT_READY              = 0x0001,  // все модули start() завершили
    SH_EVENT_SHUTDOWN           = 0x0002,  // штатное завершение перед ребутом
    SH_EVENT_MODULE_STATUS      = 0x0003,  // модуль сменил состояние
                                           // (code: 1=init,2=start,3=stop,4=error)
    SH_EVENT_SAFE_MODE_ENTERED  = 0x0004,  // вход в Kernel Safe Mode
                                           // (code: причина, см. Kernel::SafeModeReason)
    SH_EVENT_BOOTLOOP_DETECTED  = 0x0005,  // bootloop-счётчик достиг порога
    SH_EVENT_EVENT_OVERFLOW     = 0x0006,  // очередь шины переполнена
                                           // (code: число потерянных событий)
    SH_EVENT_TICK_OVERRUN       = 0x0007,  // модуль превысил бюджет tick
                                           // (code: длительность, мс)
    SH_EVENT_DEGRADED_LEVEL     = 0x0008,  // смена уровня деградации
                                           // (code: 0=FULL,1=LOCAL_NET,2=AUTONOMOUS)
};

// ============================================================================
// СИСТЕМНЫЕ СЕРВИСЫ (0x0100–0x01FF) — по 0x10 на сервис
// ============================================================================
enum ShServiceEvents : int32_t {
    // ConfigService (0x0100–0x010F)
    CFG_EVENT_CHANGED           = 0x0100,  // параметр изменён (payload: ключ)
    CFG_EVENT_SAVE_FAILED       = 0x0101,  // ошибка записи конфигурации
    CFG_EVENT_MIGRATED          = 0x0102,  // выполнена миграция NVS v2.5.0→5.0

    // StorageService (0x0110–0x011F)
    STORAGE_EVENT_LOW_SPACE     = 0x0110,  // свободно < порога (code: % заполнения)
    STORAGE_EVENT_CORRUPTED     = 0x0111,  // файл повреждён (payload: путь)

    // TimeService (0x0120–0x012F)
    TIME_EVENT_SYNCED           = 0x0120,  // время синхронизировано (RTC или NTP)
    TIME_EVENT_RTC_LOST         = 0x0121,  // DS3231 перестал отвечать
    TIME_EVENT_RTC_RESTORED     = 0x0122,  // DS3231 восстановлен после recovery

    // AuthService (0x0130–0x013F)
    AUTH_EVENT_LOGIN            = 0x0130,  // успешная авторизация (code: роль)
    AUTH_EVENT_LOGIN_FAILED     = 0x0131,  // неудачная попытка
                                           // (code: осталось попыток)
    AUTH_EVENT_LOCKED_OUT       = 0x0132,  // блокировка после N попыток
                                           // (code: длительность, сек)
    AUTH_EVENT_SETUP_REQUIRED   = 0x0133,  // C1: первый старт, ПИН не задан —
                                           // требуется provisioning

    // HealthMonitor (0x0140–0x014F)
    HEALTH_EVENT_WARNING        = 0x0140,  // проверка дала WARNING
    HEALTH_EVENT_CRITICAL       = 0x0141,  // проверка дала CRITICAL
    HEALTH_EVENT_RECOVERED      = 0x0142,  // проверка вернулась в норму
    HEALTH_EVENT_WDT_REBOOT     = 0x0143,  // планируется ребут по WDT/ПАЗ

    // UpdateService (0x0150–0x015F)
    OTA_EVENT_STARTED           = 0x0150,  // началось обновление
    OTA_EVENT_PROGRESS          = 0x0151,  // прогресс (code: %)
    OTA_EVENT_SUCCESS           = 0x0152,  // прошивка записана, ребут
    OTA_EVENT_FAILED            = 0x0153,  // ошибка (code: причина)
    OTA_EVENT_ROLLBACK          = 0x0154,  // авто-откат на предыдущий раздел

    // TelemetryService (0x0160–0x016F)
    TEL_EVENT_SNAPSHOT          = 0x0160,  // снимок метрик собран
                                           // (code: heap КБ; payload: кратко)

    // AudioService (0x0170–0x017F)
    SND_EVENT_STARTED           = 0x0170,  // фраза началась (payload: имя)
    SND_EVENT_FINISHED          = 0x0171,  // фраза доиграла (payload: имя)
    SND_EVENT_DROPPED           = 0x0172,  // фраза отброшена (очередь полна /
                                           // тихие часы; payload: имя)
    SND_EVENT_ALARM_ON          = 0x0173,  // зацикленная сирена включена
    SND_EVENT_ALARM_OFF         = 0x0174,  // сирена снята
    SND_EVENT_RESUMED           = 0x0175,  // software-resume после ADVERT

    // Scheduler (0x0180–0x018F) — доменный сервис временных правил.
    // ВНИМАНИЕ: первоначальный резерв 0x0170–0x017F (до реализации
    // ScheduleService) ДУБЛИРОВАЛ диапазон AudioService выше — повторный
    // «урок v4.2.2», пойманный до того, как ID ушёл в код. Глобальный
    // реестр существует именно для этого: любой новый блок сверяется
    // с этим файлом, а не с памятью автора.
    SCHED_EVENT_PERIOD_CHANGED  = 0x0180,  // вступил/вышел период
                                           // (code: 0=вышел, N=код периода
                                           //  при входе; payload: имя правила)
    SCHED_EVENT_RULE_FIRED      = 0x0181,  // точечное правило сработало
                                           // (будильник/cron; code: код,
                                           //  payload: имя правила)

    // CounterService (0x0190–0x019F) — персистентные счётчики + PCNT
    CNT_EVENT_ROLLOVER          = 0x0190,  // счётчик обернулся через 2^32
                                           // (payload: имя счётчика)
};

// ============================================================================
// ТРАНСПОРТ (0x0200–0x02FF)
// ============================================================================
enum ShNetEvents : int32_t {
    // NetworkManager (0x0200–0x020F) — ЕДИНСТВЕННЫЙ владелец состояния сети
    NET_EVENT_CONNECTED         = 0x0200,  // link поднят, IP получен
    NET_EVENT_DISCONNECTED      = 0x0201,  // link потерян
    NET_EVENT_IP_CHANGED        = 0x0202,  // сменился IP (payload: "x.x.x.x")
    NET_EVENT_GATEWAY_LOST      = 0x0203,  // шлюз не пингуется
    NET_EVENT_GATEWAY_RESTORED  = 0x0204,  // шлюз снова доступен
    NET_EVENT_DISABLED          = 0x0205,  // сеть отключена (джампер/локальный режим)

    // MqttTransport (0x0210–0x021F)
    SH_EVENT_MQTT_CONNECTED        = 0x0210,  // подключились к брокеру
    SH_EVENT_MQTT_DISCONNECTED     = 0x0211,  // потеряли брокера
    SH_EVENT_MQTT_MESSAGE          = 0x0212,  // входящее сообщение (payload: топик)

    // HttpServer (0x0220–0x022F)
    HTTP_EVENT_REQUEST          = 0x0220,  // входящий запрос (code: число открытых)
};

// ============================================================================
// ДРАЙВЕРЫ (0x0300–0x03FF)
// ============================================================================
enum ShDriverEvents : int32_t {
    // EspTempDriver (0x0300–0x030F) — ЕДИНСТВЕННЫЙ источник температуры кристалла
    DRV_EVENT_TEMP_UPDATE       = 0x0300,  // code: температура x10 (°C)
    DRV_EVENT_TEMP_WARNING      = 0x0301,  // выше порога предупреждения
    DRV_EVENT_TEMP_CRITICAL     = 0x0302,  // выше критического порога
    DRV_EVENT_TEMP_PANIC        = 0x0303,  // ТЕРМИЧЕСКАЯ ПАНИКА: последний
                                           // рубеж (залежь №2). crit — «плохо»,
                                           // panic — «кристалл варится»: ПАЗ
                                           // обязан зафиксировать в журнале.

    // WiegandDriver (0x0310–0x031F) — драйвер КАТАЛОГА профильной периферии
    // (profiles/drivers/wiegand), НЕ ядра. Блок ID остаётся в общем реестре
    // во избежание коллизий (урок v4.2.2): ID глобальны, код — профильный.
    DRV_EVENT_WIEGAND_CARD      = 0x0310,  // карта считана (payload: HEX 8 симв.)
    DRV_EVENT_WIEGAND_NOISE     = 0x0311,  // битовый мусор отклонён (code: биты)

    // DfPlayerDriver (0x0320–0x032F)
    DRV_EVENT_AUDIO_STARTED     = 0x0320,  // начало трека (code: номер)
    DRV_EVENT_AUDIO_FINISHED    = 0x0321,  // конец трека (BUSY -> HIGH)
    DRV_EVENT_AUDIO_OFFLINE     = 0x0322,  // плеер не отвечает
    DRV_EVENT_AUDIO_RESTORED    = 0x0323,  // плеер вернулся

    // BusManager (0x0330–0x033F)
    DRV_EVENT_BUS_RECOVERED     = 0x0330,  // шина восстановлена (code: номер)
    DRV_EVENT_BUS_DEAD          = 0x0331,  // recovery не удался
};

// ============================================================================
// ДОМЕННЫЕ СЕРВИСЫ (0x0400–0x04FF)
// ============================================================================
// 0x0400–0x040F — AccessControl (базовые; профильные расширения — в диапазоне
// профиля). Полный набор СКУД-событий профиля — см. SmartLockApp.h.
enum ShDomainEvents : int32_t {
    ACCESS_EVENT_GRANTED        = 0x0400,  // доступ разрешён (payload: карта)
    ACCESS_EVENT_DENIED         = 0x0401,  // отказ (code: причина)
    ACCESS_EVENT_LOCKED         = 0x0402,  // замок закрыт
    ACCESS_EVENT_UNLOCKED       = 0x0403,  // замок открыт (code: источник)
};

// ============================================================================
// КОМАНДЫ (0x0900–0x09FF) — адресное взаимодействие модулей
// ============================================================================
// Команда — это событие с получателем: в payload команды целевой модуль
// передаётся в ShEventData::sourceModule? НЕТ — для команд используется
// отдельная структура адресации (Phase 1). Здесь резервируем только ID-ы.
enum ShCmdEvents : int32_t {
    SH_EVENT_CMD_EXECUTE        = 0x0900,  // выполнить команду
    SH_EVENT_CMD_RESPONSE       = 0x0901,  // ответ на команду
};

// ============================================================================
// ПРИЛОЖЕНИЯ ПРОФИЛЕЙ (0x1000–0x1FFF)
// ============================================================================
// Базовые смещения выдаёт ResourceManager::claimEventRange().
// Пример (SmartLock): SL_EVENT_* = 0x1000+0x00 — см. profiles/smart_lock.
constexpr int32_t SH_EVENT_APP_BASE = 0x1000;
constexpr int32_t SH_EVENT_APP_END  = 0x1FFF;
