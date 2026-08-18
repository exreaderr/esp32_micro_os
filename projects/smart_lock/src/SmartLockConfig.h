// ============================================================================
// SmartLockConfig.h — СХЕМА КОНФИГУРАЦИИ ПРОФИЛЯ (поля lock.*)
// ============================================================================
// Все поля перенесены из NVS-настроек монолита smart_lock v2.5.0
// (namespace "lock_cfg", часть 2). Колонка "NVS" в .cpp — старый ключ
// монолита, нужен для миграции настроек при переходе с v2.5.0 на 5.0.
// Веб-UI строится автоматически по группам/меткам этой таблицы.
//
// Чего здесь НЕТ (владеет ядро, профиль не дублирует):
//   · громкость/тихие часы  -> audio.*  (AudioService);
//   · сеть/MQTT/время/OTA   -> net.*, mqtt.*, sys.*, ota.*;
//   · ПИН администратора    -> AuthService (не конфиг вовсе).
// ============================================================================
#pragma once

/// Регистрация всех полей lock.* в ConfigService.
/// Вызывается из SmartLockApp::registerExtensions().
void registerSmartLockConfig();

// ============================================================================
// ТАБЛИЦА МИГРАЦИИ NVS v2.5.0 → МикроОС 5.0 (namespace "lock_cfg")
// ============================================================================
//   l_time        → lock.open_ms            quiet        → lock.quiet_mode
//   l_type        → lock.fail_secure        a_vol        → audio.volume (!)
//   dr_alarm_m    → lock.door_alarm_min     exit_restrict→ lock.exit_restrict
//   ex_restr_act  → lock.exit_restrict_active
//   restr_st_h/m  → lock.exit_restrict_start ("HH:MM")
//   restr_en_h/m  → lock.exit_restrict_end   ("HH:MM")
//   cycle_count   → lock.cycle_count
//   web_p_hash    → AuthService (админ) + lock.user_pin (жилец)
//   hostname      → sys.hostname            net_dhcp     → net.dhcp
//   cfg_ip/mask/  → net.ip / net.mask /     mq_ip/u/p    → mqtt.host/user/pass
//   gateway/dns     net.gateway / net.dns   use_mqtt     → mqtt.enabled
