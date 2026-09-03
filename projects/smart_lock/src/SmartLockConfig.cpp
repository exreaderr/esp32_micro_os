// ============================================================================
// SmartLockConfig.cpp — поля lock.* (см. шапку .h)
// ============================================================================
#include "SmartLockConfig.h"
#include <services/ConfigService.h>

void registerSmartLockConfig() {
    ConfigService& cfg = ConfigService::getInstance();

    // === ГРУППА: Замок ================================================
    cfg.addFields("Замок", {
        { "lock.open_ms",     ConfigType::UINT,   "3000", 500, 30000, CFG_NONE,
          "Замок", "Длительность импульса открытия, мс" },          // NVS: l_time
        { "lock.fail_secure", ConfigType::BOOL,   "false", 0, 0, CFG_CRITICAL,
          "Замок", "Электромеханический замок (fail-secure)" },     // NVS: l_type
        { "lock.cycle_count", ConfigType::UINT,   "0", 0, 0, CFG_READONLY,
          "Замок", "Счётчик циклов (ресурс замка)" },               // NVS: cycle_count
    });

    // === ГРУППА: Дверь ================================================
    cfg.addFields("Дверь", {
        { "lock.door_alarm_min", ConfigType::UINT, "1", 1, 60, CFG_NONE,
          "Дверь", "Тревога «дверь открыта долго», мин" },          // NVS: dr_alarm_m
    });

    // === ГРУППА: Кнопка выхода ========================================
    cfg.addFields("Кнопка выхода", {
        { "lock.exit_restrict", ConfigType::BOOL, "false", 0, 0, CFG_NONE,
          "Кнопка выхода", "Ограничение кнопки включено" },         // NVS: exit_restrict
        { "lock.exit_restrict_active", ConfigType::BOOL, "false", 0, 0, CFG_NONE,
          "Кнопка выхода", "Расписание активно" },                  // NVS: ex_restr_act
        { "lock.exit_restrict_start", ConfigType::STRING, "22:00", 0, 0, CFG_NONE,
          "Кнопка выхода", "Запрет с (ЧЧ:ММ)" },                    // NVS: restr_st_h/m
        { "lock.exit_restrict_end", ConfigType::STRING, "06:00", 0, 0, CFG_NONE,
          "Кнопка выхода", "Запрет до (ЧЧ:ММ)" },                   // NVS: restr_en_h/m
    });
    // Правило Fail-Safe из монолита: при мёртвом RTC кнопка ВСЕГДА разрешена —
    // зашито в SmartLockApp::handleExitButton(), в конфиг не выносится.

    // === ГРУППА: Доступ по вебу =======================================
    cfg.addFields("Доступ по вебу", {
        { "lock.user_pin", ConfigType::STRING, "", 0, 0, CFG_SECRET,
          "Доступ по вебу", "Общий ПИН жильца (legacy; основной — per-user в базе)" },
        { "lock.user_pin_enabled", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Доступ по вебу", "Требовать ПИН для кнопки «Открыть»" },
        { "lock.user_session_min", ConfigType::UINT, "30", 5, 1440, CFG_NONE,
          "Доступ по вебу", "Сессия жильца по бездействию, мин" },
    });

    // === ГРУППА: Звук профиля ==========================================
    // Громкость и тихие часы — ядерные audio.* (НЕ дублируем, NVS a_vol
    // мигрирует туда). Здесь — только доменный тихий режим монолита.
    cfg.addFields("Звук профиля", {
        { "lock.quiet_mode", ConfigType::BOOL, "false", 0, 0, CFG_NONE,
          "Звук профиля", "Тихий режим (только критические тревоги)" }, // NVS: quiet
    });

    // === ГРУППА: Умный дом ==============================================
    cfg.addFields("Умный дом", {
        // Топик погоды: HA (или своя метеостанция) публикует JSON
        // {"temp":20.6,"feels_like":21.0,"state":"rainy"}. Пусто = выкл.
        // Подписка оформляется в start() — применение после перезагрузки.
        { "lock.weather_topic", ConfigType::STRING, "", 0, 0, CFG_NONE,
          "Умный дом", "MQTT-топик погоды (JSON temp/feels_like/state)" },
    });

    // === ГРУППА: Безопасность (5.5.14, поле добавлено В КОНЕЦ схемы) ===
    // Авто-чистка просроченных временных ключей (решение владельца
    // 03.09.2026): смена суток при достоверном времени → TEMPORARY с
    // истёкшим сроком удаляются из базы (журнал + событие cardRemoved).
    cfg.addFields("Безопасность", {
        { "lock.purge_expired", ConfigType::BOOL, "true", 0, 0, CFG_NONE,
          "Безопасность", "Авто-удаление просроченных временных ключей (ежесуточно)" },
    });
}
