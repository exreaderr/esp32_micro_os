// ============================================================================
// MqttOutbox.h — OUTBOX MQTT: ЧИСТОЕ RAM-КОЛЬЦО (D2: host-тесты)
// ============================================================================
// Ядерный бэклог 5.1.0. Внутренний outbox esp-mqtt работает ТОЛЬКО пока
// клиент подключён к брокеру: сообщения, опубликованные в разрыве,
// терялись молча — а в разрыве случается самое важное (доступ, тревоги,
// OTA). Здесь — кольцо «принято к доставке» на время офлайна с replay
// при reconnect (дренирует MqttTransport::onMqttConnected).
//
// Политики (все — осознанные, задокументированные):
//   · RAM, не flash: персистентный outbox = запись в NVS на каждое
//     сообщение — тот же износ, что лечил CounterService. Цена офлайн-
//     ребута — потеря очереди; на штатный ребут сообщения не планируются
//     (SH_EVENT_SHUTDOWN сливает счётчики, но не MQTT — см. 5.1.0 docs);
//   · переполнение -> вытеснение САМОГО СТАРОГО + счётчик dropped
//     (урок v2.5.0: тихая потеря недопустима — потеря всегда посчитана);
//   · retained-дедупликация: retained-сообщение по топику, уже лежащему
//     в кольце, ЗАМЕЩАЕТ тело (last-wins): устаревшее retained-состояние
//     после реконнекта опаснее отсутствующего;
//   · телеметрия сюда НЕ кладётся — ретро-снимки бессмысленны, свежая
//     публикуется сразу после дренажа.
//
// Чистая структура: никаких Arduino/heap — POD и адресная арифметика.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// Бюджеты слота = бюджетам MqttTransport (статик-ассерт на паритет — там,
// рядом с постмортемом MQTT_BODY_LEN).
constexpr uint8_t  MQTT_OB_MAX       = 8;    // слотов в кольце
constexpr uint8_t  MQTT_OB_TOPIC_LEN = 96;
constexpr uint16_t MQTT_OB_BODY_LEN  = 256;

struct MqttObSlot {
    char    topic[MQTT_OB_TOPIC_LEN];
    char    body[MQTT_OB_BODY_LEN];
    uint8_t qos;            // 0/1 (2 не используем — как и весь мост)
    bool    retained;
    bool    used;
};

namespace mqtt_ob {

struct Outbox {
    MqttObSlot slots[MQTT_OB_MAX];
    uint8_t  head    = 0;   // самое старое сообщение
    uint8_t  count   = 0;
    uint32_t dropped = 0;   // вытеснено при переполнении (за всё время)

    void reset() {
        head = 0; count = 0; dropped = 0;
        for (uint8_t i = 0; i < MQTT_OB_MAX; ++i) slots[i].used = false;
    }

    uint8_t size() const { return count; }

    /// Принять сообщение к доставке. false — топик пуст или длиннее слота
    /// (топик обрезать нельзя: уйдёт не туда — это хуже отказа). Тело
    /// обрезается по размеру слота (поведение snprintf в путях публикации).
    bool push(const char* topic, const char* body, uint8_t qos, bool retained) {
        if (topic == nullptr || topic[0] == '\0' ||
            strlen(topic) >= MQTT_OB_TOPIC_LEN) {
            return false;
        }
        if (body == nullptr) body = "";

        // Retained-дедупликация: то же состояние — свежее значение взамен.
        if (retained) {
            for (uint8_t k = 0; k < count; ++k) {
                MqttObSlot& s = slots[(head + k) % MQTT_OB_MAX];
                if (s.used && s.retained && strcmp(s.topic, topic) == 0) {
                    copyBody(s, body);
                    s.qos = qos;
                    return true;
                }
            }
        }

        // Переполнение: вытесняем старейшее, потеря считается.
        if (count == MQTT_OB_MAX) {
            slots[head].used = false;
            head = (uint8_t)((head + 1) % MQTT_OB_MAX);
            --count;
            ++dropped;
        }

        MqttObSlot& s = slots[(head + count) % MQTT_OB_MAX];
        strncpy(s.topic, topic, sizeof(s.topic) - 1);
        s.topic[sizeof(s.topic) - 1] = '\0';
        copyBody(s, body);
        s.qos = qos;
        s.retained = retained;
        s.used = true;
        ++count;
        return true;
    }

    /// Самое старое сообщение (для дренажа: peek -> publish -> pop).
    const MqttObSlot* peek() const {
        return count ? &slots[head] : nullptr;
    }

    void pop() {
        if (!count) return;
        slots[head].used = false;
        head = (uint8_t)((head + 1) % MQTT_OB_MAX);
        --count;
    }

private:
    static void copyBody(MqttObSlot& s, const char* body) {
        strncpy(s.body, body, sizeof(s.body) - 1);
        s.body[sizeof(s.body) - 1] = '\0';
    }
};

} // namespace mqtt_ob
