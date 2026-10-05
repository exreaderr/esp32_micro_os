// ============================================================================
// HomeMasterApp.h — ПОЛИТИКА МАСТЕРА УМНОГО ДОМА (M1)
// ============================================================================
// «Голова» профиля: инжектирует расширения в ядро (конфиг-схему, веб-лицо,
// ПАЗ), владеет режимом мастера (auto/solo/bridge — оживут с мостом M2),
// объявляет устройство в Home Assistant (discovery к вышестоящему брокеру)
// и публикует профильный снимок состояния (SD/брокер) в MQTT.
// ============================================================================
#pragma once

// 5.8.8: ЕДИНСТВЕННЫЙ источник версии профиля. Макрос объявлен ДО включения
// ядра — Version.h зашивает полную метку «MICROOS|ядро|профиль|END» в .bin,
// getVersion() отдаёт ту же версию строкой. Расходиться не могут по
// построению.
#define MICROOS_PROFILE_VER 0.9.2

#include <core/Version.h>
#include <core/ModuleBase.h>

/// Режим работы мастера (концепция §3). Значения конфига master.mode.
enum class MasterMode : uint8_t {
    Auto   = 0,   // детект вышестоящего брокера (M2), сейчас = Solo
    Solo   = 1,   // всегда свой брокер (брокер — M1)
    Bridge = 2,   // всегда транслятор на upstream (мост — M2)
};

class HomeMasterApp : public ModuleBase {
public:
    static HomeMasterApp& getInstance();

    // --- IModule ---------------------------------------------------------
    const char* getName() const override { return "HomeMasterApp"; }
    const char* getVersion() const override { return MICROOS_STR(MICROOS_PROFILE_VER); }   // 0.9.2: W8 — фикс разбора записей bfresp (двойная скобка [[…]]: цикл хватал внешнюю скобку массива → 0 записей из любого непустого кадра, три полевые сессии ушли в «необслужимые»; поймано сверкой таймингов bfreq/bfresp с провода, CRC кадров верны) + триггер «поток weather возобновился после >75 мин» (тест с обрывом линка: брокер без keepalive не видит offline, state-триггер слеп); 0.9.1: хотфикс index.html — гремлин при правке вкладки «Архив» продублировал 251 строку JS (сырой код текстом внизу страницы); восстановлено из 0.8.9 +2 строки W8; 0.9.0: W8 backfill — скан дыр w7a за 7 суток (старт+5 мин / ежесуточно arch.bf_hour UTC / шлюз online после offline>75 мин), bfreq по суткам UTC, приём bfresp хуком (RAM), мердж только отсутствующих ts (tmp+rename), необслужимые сутки — повтор раз/сутки, взаимное исключение с W7; инцидент: гремлин съел чтение arch.pace_ms в 0.8.9 — восстановлено; 0.8.9: W7 — пейсинг из конфига arch.pace_ms (дефолт 500 мс; дренаж шлюза ~2 кадра/с, 02.10), сторож сессии 240 с; 0.8.8: W7 — пейсинг выдачи по wall-clock 200 мс/кадр (замер ветки: реальный тик ~20 мс, а не 150; 210 кадров ≈ 42 с); 0.8.7: W7 — темп выдачи ровно 1 кадр/тик (пачки 0.8.6 затирались однослотовым mailbox ext-подписки ядра на шлюзе, 8/210; 210 кадров ≈ 32 с); 0.8.6: W7 — переспрос не ломает бегущую сессию; 0.8.5: версия профиля в OTA-зеркале; 0.8.4: вкладка «Архив» в веб-морде (статус архивариуса, файлы /archive/, группа «Архив W7» переехала туда из «Прочих»; API hm/archive/status); 0.8.3: диагностика архивариуса (warn при req с чужим id — ловит рассинхрон arch.gw_id; инфо о первом принятом weather-кадре); 0.8.2: кадр архива = 4 ч / 4 записи, ≤216 Б худший (входящий кап MQTT_BODY_LEN=256, доп. ветки 20.09 — 6 записей давало 290 Б); 0.8.1: W7 архивариус (HmArchiveModule: часовые ведра wx_* на SD /archive/, выдача восстановления по MQTT запрос-ответу, кадр=полдня, CRC32/zlib); 0.8.0: единая инфо-карточка флота (uiDisplayName «Домашний мастер», extras режим/клиенты; ядро 5.9.0); 0.7.0: M5 — статус-LED WS2812 ×4 + OLED по требованию (ядро 5.8.9); 0.6.10: двухполевая версия OTA (ядро 5.8.8); 0.6.9: ConfirmState → ядро;         // 0.6.9: ядро 5.8.7 — ConfirmState демонтирован, подтверждения в HealthMonitor; 0.6.8: hm.fleet + гистерезис (вариант Б); 0.6.7: ручная загрузка троек + otam.src; 0.6.6: broker.pass → NVS, hostname; 0.6.5: OTA-раздача кусками + TWDT-feed; 0.6.4: bk само-проверка в цикле; 0.6.3: правка bk.self; 0.6.2: bk.self + оверлей; 0.6.1: OTA-зеркало
    ModuleId getModuleId() const override { return 0x1102; }   // 0x1101=SdService, 0x1103=BrokerService, 0x1104=BridgeService
    void registerExtensions() override;
    void init() override;
    void start() override;
    void stop() override { _started = false; }
    void tick() override;
    void onEvent(int32_t eventId, const ShEventData* data) override;
    bool canHandleEvent(int32_t eventId) const override;

    /// Режим из конфига (строка master.mode -> enum; неизвестное = Auto).
    MasterMode mode() const { return _mode; }
    const char* modeStr() const;

private:
    // --- HA discovery + профильное состояние (M1) -------------------------
    /// Объявление сущностей в HA (retained-конфиги, паттерн smart_lock).
    void publishHaDiscovery();
    /// Снимок профиля в <prefix>/<id>/hm/state (retained JSON для HA и
    /// будущего флот-дашборда). Офлайн — копится/дедупится outbox'ом ядра.
    void publishHmState();
    /// Команды из брокера, неизвестные ядру (cmd/sd_remount).
    static bool onMqttCmd(const char* verb, const char* body);

    static constexpr uint32_t HM_STATE_PERIOD_MS = 30000;  // период снимка

    MasterMode _mode = MasterMode::Auto;
    uint32_t   _lastStateMs = 0;
};
