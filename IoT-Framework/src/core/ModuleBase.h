// ============================================================================
// ModuleBase.h — БАЗОВЫЙ КЛАСС МОДУЛЯ (устранение копипасты v4.2.2)
// ============================================================================
// Фаза 0. В v4.2.2 каждый из 15 модулей содержал СВОЮ копию: safeStrCopy,
// logMessage (2 перегрузки), publishErrorEvent, handleCommand,
// streamDiagnosticInfo, printStats, флаги состояния, таймаут мьютекса.
// Здесь — одна реализация на всех. Наследник получает:
//
//   log(LogLevel, fmt, ...)   — логирование с тегом = именем модуля;
//                               в Phase 2 перенаправляется в LogService,
//                               сейчас — Serial с префиксом.
//   safeStrCopy(dst, n, src)  — копирование без переполнения и без String;
//   publishError(code)        — системное событие ошибки модуля;
//   postEvent(id, data)       — публикация в шину с простановкой sourceModule;
//   takeMutex()/giveMutex()   — рекурсивный мьютекс модуля с таймаутом;
//   флаги _initialized/_started + isReady() по умолчанию.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include "IModule.h"
#include "EventBus.h"

/// Таймаут захвата мьютекса модуля. Из аудита v4.2.2: 500 мс — баланс между
/// "не подвесить модуль" и "дождаться освобождения при пиковой нагрузке".
constexpr TickType_t SH_MUTEX_TIMEOUT_MS = 500;

class ModuleBase : public IModule {
public:
    ModuleBase() = default;
    virtual ~ModuleBase() = default;

    // --- СОСТОЯНИЕ (умолчания, наследник может переопределить) -----------
    bool isReady() const override { return _initialized && _started; }

    // --- ПРИЁМНИК ЛОГОВ (Фаза 3, LogService) ------------------------------
    /// Сигнатура приёмника строки лога: уровень, тег (имя модуля), текст.
    /// Вызывается ИЗ КОНТЕКСТА ЛОГИРУЮЩЕГО — приёмник обязан быть быстрым
    /// и не делать FS/heap-операций (LogService пишет только в RAM-кольцо).
    using LogSink = void (*)(LogLevel level, const char* tag, const char* body);
    /// Подключить приёмник (LogService делает это в describe() — самой ранней
    /// фазой, чтобы не потерять логи registerExtensions/init других модулей).
    static void setLogSink(LogSink sink);

protected:
    // --- ЛОГИРОВАНИЕ ------------------------------------------------------
    /// Форматированный лог с тегом модуля. Уровень влияет на префикс;
    /// в Phase 2 LogService добавит запись в кольцевой буфер/файл.
    void log(LogLevel level, const char* fmt, ...) const;

    // --- БЕЗОПАСНОЕ КОПИРОВАНИЕ СТРОК -------------------------------------
    /// Гарантированная '\0'-терминация, без выделения памяти.
    static void safeStrCopy(char* dst, size_t dstSize, const char* src);

    // --- СОБЫТИЯ ------------------------------------------------------------
    /// Публикация события от имени модуля (sourceModule подставляется сам).
    void postEvent(int32_t eventId, ShEventData* data = nullptr);

    /// Публикация системной ошибки модуля (короткий код в payload).
    void publishError(const char* errorCode);

    // --- МЬЮТЕКС МОДУЛЯ -------------------------------------------------------
    /// Создать рекурсивный мьютекс (в init()). Безопасно при повторном вызове.
    bool ensureMutex();

    /// Захватить с таймаутом. false — мьютекс занят дольше SH_MUTEX_TIMEOUT_MS:
    /// наследник обязан обработать (пропустить операцию), а не ждать вечно
    /// (урок addLog v2.5.0: событие лучше потерять, чем уронить ядро).
    bool takeMutex(TickType_t timeoutMs = SH_MUTEX_TIMEOUT_MS) const;
    void giveMutex() const;

    // --- ФЛАГИ СОСТОЯНИЯ ------------------------------------------------------
    bool _initialized = false;   // init() завершён
    bool _started     = false;   // start() завершён
    bool _initInProgress = false;// защита от повторного входа в init()

private:
    mutable SemaphoreHandle_t _mutex = nullptr;
};
