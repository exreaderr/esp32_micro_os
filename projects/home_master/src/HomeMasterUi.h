// ============================================================================
// HomeMasterUi.h — ВЕБ-ЛИЦО МАСТЕРА (IUiProvider, M0)
// ============================================================================
// M0 — диагностическое лицо bring-up: плата, режим, SD, сеть. Флот-дашборд
// появится на M1+ (реестр парка, журнал). Админские страницы ядра (сеть,
// конфиг master.*/sd.*, телеметрия, аудит, OTA) строятся автоматически.
//
// Эндпоинты /api/dev/hm/*:
//   GET  hm/info        — сводка: плата, версии, режим, SD, память, аптайм;
//   POST hm/sd/remount  — перемонтировать SD (только админская сессия ядра);
//   GET  hm/journal/dl?n=<сегмент> — 5.8.0: скачать сегмент журнала честным
//                                    файлом (admin, потоково, statusCode=0).
// ============================================================================
#pragma once

#include <services/IUiProvider.h>

class HomeMasterUi : public IUiProvider {
public:
    const char* uiTitle() const override { return "home_master"; }

    // 5.9.0/0.8.0: единая инфо-карточка флота — человеческое имя по
    // умолчанию (перебивается sys.name из конфига) и профильные строки.
    const char* uiDisplayName() const override { return "Домашний мастер"; }
    uint8_t infoCardExtras(char labels[][24], char values[][32],
                           uint8_t maxLines) override;

    /// Фрагмент публичной страницы "/" (статус-строки мастера).
    size_t renderPublicHtml(char* buf, size_t bufSize) override;

    /// /api/dev/hm/* — см. таблицу в шапке.
    bool handleApi(const char* pathTail, const ShUiRequest& req,
                   char* responseBuf, size_t bufSize,
                   int& statusCode) override;

private:
    bool apiInfo(char* buf, size_t size);
    bool apiSdRemount(const ShUiRequest& req, char* buf, size_t size,
                      int& status);
    bool apiJournalDl(const ShUiRequest& req, char* buf, size_t size,
                      int& status);
};
