// ============================================================================
// EventBus.h — ЕДИНАЯ ШИНА СОБЫТИЙ МикроОС 5.0
// ============================================================================
// Фаза 0. Заменяет ДВОЙНУЮ доставку v4.2.2 (esp_event + очередь AppCore +
// подписки каждого модуля на ESP_EVENT_ANY_ID):
//
//   БЫЛО (v4.2.2): postEvent -> esp_event_post -> задача esp_event ->
//     AppCore::eventHandler -> xQueue -> eventProcessorTask -> onEvent,
//     И параллельно каждый модуль получал то же событие второй раз через
//     свою подписку ANY_ID. Две очереди, двойная фильтрация, ~300 байт RAM
//     на событие в полёте.
//
//   СТАЛО (5.0): одна FreeRTOS-очередь, одна задача-диспетчер, доставка
//     ТОЛЬКО подписчикам конкретного eventId. Модуль, не подписанный на
//     событие, на нём не просыпается вообще.
//
// Гарантии:
//   · post() неблокирующий и копирует данные — можно вызывать из любой задачи;
//   · postFromISR() — из прерываний (драйверы: Wiegand, кнопки);
//   · переполнение очереди: вытесняется самое старое событие + счётчик потерь
//     + троттлированное SYS-предупреждение (урок: тихая потеря логов v2.5.0);
//   · защита от рекурсивного зацикливания: глубина цепочки "событие породило
//     событие" ограничена SH_BUS_MAX_CHAIN.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ShTypes.h"

class IModule;   // forward — шина не включает интерфейс модуля,
                 // чтобы не было циклической зависимости заголовков

// ============================================================================
// НАСТРОЙКИ ШИНЫ (компиляционные бюджеты)
// ============================================================================
constexpr uint8_t  SH_BUS_QUEUE_LEN     = 32;   // глубина очереди событий
constexpr uint8_t  SH_BUS_MAX_SUBSCRIBERS = 16; // подписчиков на один eventId
constexpr uint8_t  SH_BUS_MAX_EVENT_IDS  = 96;  // различных eventId с подпиской
constexpr uint8_t  SH_BUS_MAX_CHAIN      = 4;   // макс. глубина цепочки событий
constexpr uint32_t SH_BUS_STACK_BYTES    = 4096; // стек задачи-диспетчера

class EventBus {
public:
    static EventBus& getInstance();

    // --- ПОДПИСКА -------------------------------------------------------
    /// Подписать модуль на конкретный eventId. Повторная подписка того же
    /// модуля на тот же ID игнорируется. Возвращает false при исчерпании
    /// таблиц (смотреть SH_BUS_MAX_*).
    bool subscribe(int32_t eventId, IModule* module);

    /// Снять ВСЕ подписки модуля (вызывается в stop()).
    void unsubscribeAll(IModule* module);

    // --- ПУБЛИКАЦИЯ ------------------------------------------------------
    /// Публикация из контекста задачи. Неблокирующая: при переполнении
    /// вытесняет старейшее событие. data может быть nullptr.
    void post(int32_t eventId, const ShEventData* data = nullptr);

    /// Публикация из ISR (IRAM-safe). Пробуждение задачи-диспетчера
    /// через taskWoken, если оно требуется.
    void postFromISR(int32_t eventId, const ShEventData* data,
                     BaseType_t* taskWoken);

    // --- ЖИЗНЕННЫЙ ЦИКЛ --------------------------------------------------
    /// Создать очередь и задачу-диспетчер. Вызывается Kernel до init модулей.
    bool begin();

    /// Остановить задачу и очередь (перед ребутом / в Safe Mode — нет).
    void end();

    // --- МЕТРИКИ (B1: системная телеметрия) -------------------------------
    uint32_t getDroppedCount()  const { return _dropped; }
    uint32_t getPublishedCount()const { return _published; }
    uint8_t  getHighWatermark() const { return _highWatermark; }
    /// Сброс счётчиков (после выгрузки метрик в TelemetryService).
    void resetMetrics();

private:
    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // --- Запись подписки: (eventId, модуль). Плоские массивы вместо
    //     std::map — экономия heap и предсказуемое время поиска. ---
    struct Subscription {
        int32_t  eventId;
        IModule* module;
    };

    // --- Элемент очереди: событие + глубина цепочки. Глубина передаётся
    //     вместе с событием, чтобы отловить зацикливание A->B->A->... ---
    struct QueuedEvent {
        int32_t     eventId;
        ShEventData data;
        bool        hasData;
        uint8_t     chainDepth;
    };

    /// Задача-диспетчер: разбирает очередь и вызывает onEvent подписчиков.
    static void dispatcherTask(void* arg);
    void dispatch(const QueuedEvent& ev);

    /// Общий код постановки в очередь (обычный и ISR-вариант).
    void enqueue(const QueuedEvent& ev, bool fromIsr, BaseType_t* taskWoken);

    // --- Данные ---------------------------------------------------------
    QueueHandle_t  _queue = nullptr;
    TaskHandle_t   _task  = nullptr;

    Subscription _subs[SH_BUS_MAX_EVENT_IDS][SH_BUS_MAX_SUBSCRIBERS];
    uint8_t      _subsCount[SH_BUS_MAX_EVENT_IDS];   // подписчиков в строке
    int32_t      _subIds[SH_BUS_MAX_EVENT_IDS];      // eventId строки
    uint8_t      _subRows = 0;                       // занятых строк

    // Текущая глубина цепочки (растёт, когда onEvent публикует новое событие
    // синхронно из dispatch). При превышении SH_BUS_MAX_CHAIN событие
    // отбрасывается с предупреждением — защита от бесконечного пинг-понга.
    uint8_t      _chainDepth = 0;

    // Метрики
    volatile uint32_t _dropped   = 0;   // потеряно из-за переполнения
    volatile uint32_t _published = 0;   // всего опубликовано
    uint8_t  _highWatermark = 0;        // максимум заполнения очереди
    uint32_t _lastOverflowWarnMs = 0;   // троттлинг предупреждений
};
