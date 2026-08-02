// ============================================================================
// EventBus.cpp — реализация единой шины событий
// ============================================================================
#include "EventBus.h"
#include "IModule.h"
#include "Events.h"

EventBus& EventBus::getInstance() {
    static EventBus instance;
    return instance;
}

// ============================================================================
// ЖИЗНЕННЫЙ ЦИКЛ
// ============================================================================
bool EventBus::begin() {
    // Очередь создаётся один раз; повторный begin безопасен.
    if (_queue == nullptr) {
        _queue = xQueueCreate(SH_BUS_QUEUE_LEN, sizeof(QueuedEvent));
        if (_queue == nullptr) {
            Serial.println(F("[BUS] CRITICAL: queue creation failed"));
            return false;
        }
    }
    if (_task == nullptr) {
        // Задача-диспетчер на ядре 0: ядро 1 оставляем сетевому стеку и
        // пользовательскому loop() Arduino.
        BaseType_t rc = xTaskCreatePinnedToCore(
            dispatcherTask, "eventBus", SH_BUS_STACK_BYTES / sizeof(StackType_t),
            this, /*priority*/ 5, &_task, /*core*/ 0);
        if (rc != pdPASS) {
            Serial.println(F("[BUS] CRITICAL: dispatcher task creation failed"));
            return false;
        }
    }
    Serial.println(F("[BUS] Event bus started"));
    return true;
}

void EventBus::end() {
    if (_task != nullptr) {
        vTaskDelete(_task);
        _task = nullptr;
    }
    if (_queue != nullptr) {
        vQueueDelete(_queue);
        _queue = nullptr;
    }
}

// ============================================================================
// ПОДПИСКА
// ============================================================================
bool EventBus::subscribe(int32_t eventId, IModule* module) {
    if (module == nullptr) return false;

    // Ищем существующую строку для этого eventId
    for (uint8_t r = 0; r < _subRows; ++r) {
        if (_subIds[r] != eventId) continue;
        // Дубликат подписки — молча игнорируем (идемпотентность)
        for (uint8_t i = 0; i < _subsCount[r]; ++i) {
            if (_subs[r][i].module == module) return true;
        }
        if (_subsCount[r] >= SH_BUS_MAX_SUBSCRIBERS) {
            Serial.printf("[BUS] ERROR: subscribers limit for event 0x%04lX\n",
                          (unsigned long)eventId);
            return false;
        }
        _subs[r][_subsCount[r]].eventId = eventId;
        _subs[r][_subsCount[r]].module  = module;
        _subsCount[r]++;
        return true;
    }

    // Новая строка под новый eventId
    if (_subRows >= SH_BUS_MAX_EVENT_IDS) {
        Serial.printf("[BUS] ERROR: eventId table full, cannot subscribe 0x%04lX\n",
                      (unsigned long)eventId);
        return false;
    }
    _subIds[_subRows]       = eventId;
    _subs[_subRows][0]      = { eventId, module };
    _subsCount[_subRows]    = 1;
    _subRows++;
    return true;
}

void EventBus::unsubscribeAll(IModule* module) {
    for (uint8_t r = 0; r < _subRows; ++r) {
        for (uint8_t i = 0; i < _subsCount[r]; ++i) {
            if (_subs[r][i].module == module) {
                // Удаление сдвигом хвоста (порядок подписчиков не важен)
                for (uint8_t j = i; j + 1 < _subsCount[r]; ++j) {
                    _subs[r][j] = _subs[r][j + 1];
                }
                _subsCount[r]--;
                i--;  // перепроверить сдвинутый элемент
            }
        }
    }
}

// ============================================================================
// ПУБЛИКАЦИЯ
// ============================================================================
void EventBus::post(int32_t eventId, const ShEventData* data) {
    QueuedEvent ev;
    ev.eventId    = eventId;
    ev.hasData    = (data != nullptr);
    // ПОСТМОРТЕМ 5.0.x: _chainDepth имеет смысл ТОЛЬКО внутри задачи-
    // диспетчера — именно там крутится dispatch() и растит глубину.
    // Шина многозадачная (тики в loopTask, mqtt-задача, HTTP): чужой пост,
    // пришедшийся на чужой dispatch, наследовал его глубину и событие
    // случайно улетало под страж («chain too deep» без всякой цепочки —
    // маскировалось под шторм, гасились SND_EVENT_FINISHED и др.).
    // Пост из другой задачи — всегда НАЧАЛО новой цепочки (depth 0).
    ev.chainDepth = (xTaskGetCurrentTaskHandle() == _task) ? _chainDepth : 0;
    if (data) ev.data = *data; else ev.data.clear();

    // Защита от рекурсивного зацикливания событий
    if (ev.chainDepth >= SH_BUS_MAX_CHAIN) {
        Serial.printf("[BUS] WARN: event chain too deep, dropping 0x%04lX\n",
                      (unsigned long)eventId);
        return;
    }
    enqueue(ev, /*fromIsr*/ false, nullptr);
    _published++;
}

void EventBus::postFromISR(int32_t eventId, const ShEventData* data,
                           BaseType_t* taskWoken) {
    QueuedEvent ev;
    ev.eventId    = eventId;
    ev.hasData    = (data != nullptr);
    ev.chainDepth = 0;   // из ISR цепочек не бывает
    if (data) ev.data = *data; else ev.data.clear();
    enqueue(ev, /*fromIsr*/ true, taskWoken);
    _published++;
}

// ============================================================================
// ПОСТАНОВКА В ОЧЕРЕДЬ (общая для task/ISR)
// ============================================================================
void EventBus::enqueue(const QueuedEvent& ev, bool fromIsr, BaseType_t* taskWoken) {
    if (_queue == nullptr) return;

    BaseType_t rc;
    if (fromIsr) {
        rc = xQueueSendToBackFromISR(_queue, &ev, taskWoken);
    } else {
        rc = xQueueSendToBack(_queue, &ev, 0);   // неблокирующе!
    }

    if (rc == pdTRUE) {
        // High-water mark для телеметрии
        if (!fromIsr) {
            UBaseType_t used = SH_BUS_QUEUE_LEN - uxQueueSpacesAvailable(_queue);
            if (used > _highWatermark) _highWatermark = (uint8_t)used;
        }
        return;
    }

    // --- ПЕРЕПОЛНЕНИЕ: вытесняем старейшее событие -------------------------
    // Урок v2.5.0: тихая потеря событий недопустима — считаем и сообщаем.
    QueuedEvent dummy;
    if (fromIsr) {
        xQueueReceiveFromISR(_queue, &dummy, taskWoken);
        xQueueSendToBackFromISR(_queue, &ev, taskWoken);
    } else {
        xQueueReceive(_queue, &dummy, 0);
        xQueueSendToBack(_queue, &ev, 0);
    }
    _dropped++;

    // Троттлинг предупреждения: не чаще раза в 5 секунд, чтобы само
    // предупреждение не стало источником шторма.
    uint32_t now = millis();
    if (!fromIsr && now - _lastOverflowWarnMs > 5000) {
        _lastOverflowWarnMs = now;
        Serial.printf("[BUS] WARN: queue overflow, dropped=%lu\n",
                      (unsigned long)_dropped);
        // Отдельное системное событие — НЕ через post(), чтобы не
        // усугубить переполнение: кладём напрямую только если есть место.
        // (HealthMonitor также видит _dropped через метрики.)
    }
}

// ============================================================================
// ДИСПЕТЧЕР
// ============================================================================
void EventBus::dispatcherTask(void* arg) {
    EventBus* self = static_cast<EventBus*>(arg);
    QueuedEvent ev;
    for (;;) {
        // Ждём событие бесконечно — задача спит, когда шина пуста
        if (xQueueReceive(self->_queue, &ev, portMAX_DELAY) == pdTRUE) {
            self->dispatch(ev);
        }
    }
}

void EventBus::dispatch(const QueuedEvent& ev) {
    // Находим строку подписчиков
    for (uint8_t r = 0; r < _subRows; ++r) {
        if (_subIds[r] != ev.eventId) continue;

        for (uint8_t i = 0; i < _subsCount[r]; ++i) {
            IModule* m = _subs[r][i].module;
            if (m == nullptr) continue;
            // Модуль получает только то, на что подписан; фильтрация
            // canHandleEvent остаётся как дополнительный предохранитель.
            if (!m->canHandleEvent(ev.eventId)) continue;

            _chainDepth = ev.chainDepth + 1;  // для событий, порождённых
                                              // из этого onEvent
            m->onEvent(ev.eventId, ev.hasData ? &ev.data : nullptr);
            _chainDepth = ev.chainDepth;
        }
        return;
    }
    // Событие без подписчиков — норма (например, метрики для будущих
    // подписчиков). Не шумим в лог: это уровень Debug.
}

// ============================================================================
// МЕТРИКИ
// ============================================================================
void EventBus::resetMetrics() {
    _dropped      = 0;
    _published    = 0;
    _highWatermark = 0;
}
