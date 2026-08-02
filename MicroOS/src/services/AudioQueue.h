// ============================================================================
// AudioQueue.h — ОЧЕРЕДЬ ВОСПРОИЗВЕДЕНИЯ (чистая логика, D2: host-тесты)
// ============================================================================
// Выделено из AudioService по требованию D2: политика очереди (приоритеты,
// вытеснение, анти-флуд) не зависит от железа и покрывается host-тестами.
//
// Модель приоритетов (единственный «рот» устройства — арбитраж обязателен):
//   Alarm     (3) — сирена: вытесняет всё, зациклена, снимается командой;
//   Important (2) — отказ доступа, тревога датчика: вытесняет 0–1;
//   Normal    (1) — подтверждения: FIFO, без вытеснения;
//   Ambient   (0) — бипы/приветствия: при переполнении теряется первым.
//
// ADVERT-семантика («прервать и продолжить») реализуется СЕРВИСОМ поверх
// этой очереди (аппаратный ADVERT клона ненадёжен — наш опыт); здесь —
// только решения: кого играть, кого прервать, кого вернуть.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>

// Приоритеты фраз (см. таблицу выше)
enum class SndPriority : uint8_t {
    Ambient   = 0,
    Normal    = 1,
    Important = 2,
    Alarm     = 3
};

// Флаги фразы (SoundPhrase.flags)
constexpr uint8_t SND_FLAG_LOOP   = 0x01;  // зациклить (сирена) до alarmOff
constexpr uint8_t SND_FLAG_ADVERT = 0x02;  // «прервать и продолжить» (спец.)

constexpr uint8_t SND_NAME_LEN   = 24;     // семантическое имя фразы
constexpr uint8_t SND_QUEUE_SIZE = 8;      // глубина очереди

// ============================================================================
// ЭЛЕМЕНТ ОЧЕРЕДИ
// ============================================================================
struct SndItem {
    char     name[SND_NAME_LEN];  // "access.granted" — для событий/метрик
    uint8_t  folder;              // адрес на SD: папка
    uint8_t  track;               // ...и трек (playFolder — надёжно на клоне)
    uint8_t  priority;            // SndPriority
    uint8_t  flags;               // SND_FLAG_*
    uint32_t enqueuedMs;          // FIFO внутри приоритета
};

// ============================================================================
// ОЧЕРЕДЬ С ПРИОРИТЕТАМИ (фиксированная, без heap)
// ============================================================================
class SndQueue {
public:
    /// Постановка. Политика переполнения:
    ///   · есть место — встаём;
    ///   · полна: ищем самый старый элемент СТРОГО ниже приоритета нового —
    ///     вытесняем его; если таких нет (все >= нового) — отказ (dropped).
    /// Возвращает true — фраза принята.
    bool enqueue(const SndItem& item) {
        if (_count < SND_QUEUE_SIZE) {
            _items[_count++] = item;
            return true;
        }
        int victim = -1;
        for (uint8_t i = 0; i < _count; ++i) {
            if (_items[i].priority < item.priority &&
                (victim < 0 ||
                 _items[i].enqueuedMs < _items[victim].enqueuedMs)) {
                victim = (int)i;
            }
        }
        if (victim < 0) return false;      // отказ — считает вызывающий
        _items[victim] = item;
        return true;
    }

    /// Извлечь высокоприоритетный (при равенстве — самый старый) элемент.
    bool pop(SndItem& out) {
        int best = peekIndex();
        if (best < 0) return false;
        out = _items[best];
        // Сдвиг остатка (массив крошечный — дёшево)
        for (uint8_t i = (uint8_t)best; i + 1 < _count; ++i) {
            _items[i] = _items[i + 1];
        }
        --_count;
        return true;
    }

    /// Просмотр без извлечения (nullptr — пусто)
    const SndItem* peek() const {
        int best = peekIndex();
        return best < 0 ? nullptr : &_items[best];
    }

    bool empty() const { return _count == 0; }
    uint8_t count() const { return _count; }
    void clear() { _count = 0; }

    /// Вернуть фразу в ГОЛОВУ своего приоритетного слоя (software-resume
    /// после ADVERT: прерванная продолжается раньше ожидающих равных).
    void pushFront(const SndItem& item) {
        if (_count >= SND_QUEUE_SIZE) return;   // некуда — честно теряем
        // Найти позицию: после всех с приоритетом > item.priority
        uint8_t pos = 0;
        while (pos < _count && _items[pos].priority > item.priority) ++pos;
        for (int8_t i = (int8_t)_count; i > (int8_t)pos; --i) {
            _items[i] = _items[i - 1];
        }
        _items[pos] = item;
        ++_count;
    }

private:
    int peekIndex() const {
        if (_count == 0) return -1;
        uint8_t best = 0;
        for (uint8_t i = 1; i < _count; ++i) {
            if (_items[i].priority > _items[best].priority ||
                (_items[i].priority == _items[best].priority &&
                 _items[i].enqueuedMs < _items[best].enqueuedMs)) {
                best = i;
            }
        }
        return (int)best;
    }

    SndItem _items[SND_QUEUE_SIZE];
    uint8_t _count = 0;
};

namespace snd {

/// Вытеснение текущего: новый приоритет СТРОГО выше текущего.
/// (Alarm > всё; Important > Normal/Ambient; равные не вытесняют.)
inline bool shouldPreempt(uint8_t newPrio, uint8_t currentPrio) {
    return newPrio > currentPrio;
}

/// Анти-флуд: повтор ТОЙ ЖЕ фразы внутри окна подавляется.
/// nowMs/lastMs — millis(); окно 0 — подавление выключено.
inline bool isRepeat(uint32_t nowMs, uint32_t lastMs, uint32_t windowMs) {
    return windowMs > 0 && (nowMs - lastMs) < windowMs;
}

} // namespace snd
