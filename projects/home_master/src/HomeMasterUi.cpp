// ============================================================================
// HomeMasterUi.cpp — веб-лицо мастера (M1: discovery-эпоха, mqtt_up в info)
// ============================================================================
#include "HomeMasterUi.h"
#include "HomeMasterApp.h"
#include "SdService.h"
#include "BrokerService.h"
#include "BridgeService.h"
#include "JournalService.h"
#include <services/HttpService.h>
#include <services/ConfigService.h>
#include <services/MqttTransport.h>
#include <services/TimeService.h>
#include <core/Version.h>
#include <platform/BaseProfile.h>
#include <esp_heap_caps.h>
#include <cstdlib>
#include <cstring>

// ============================================================================
// ПУБЛИЧНАЯ СТРАНИЦА "/" (фрагмент, бюджет ~2 КБ)
// ============================================================================
size_t HomeMasterUi::renderPublicHtml(char* buf, size_t bufSize) {
    SdService& sd = SdService::getInstance();
    BrokerService& br = BrokerService::getInstance();
    // Размер карты — только при Mounted, иначе хвост "0" лепится к статусу
    char sdExtra[24] = "";
    if (sd.state() == SdState::Mounted) {
        snprintf(sdExtra, sizeof(sdExtra), " · %llu МБ", sd.sizeMb());
    }
    // Строка брокера (M1 spike): счётчики — только когда слушает порт
    char brokerLine[96] = "<div>MQTT-брокер: <b>выкл</b> (M1 spike)</div>";
    if (br.running()) {
        snprintf(brokerLine, sizeof(brokerLine),
            "<div>MQTT-брокер: <b>:%u</b> · клиентов %u/%u · retained %lu · rx %lu</div>",
            br.port(), br.clients(), br.maxClients(),
            (unsigned long)br.retained(), (unsigned long)br.rxTotal());
    }
    int n = snprintf(buf, bufSize,
        "<div>Мастер системы умного дома · %s · МикроОС %s</div>"
        "<div>Режим: <b>%s</b> · Этап M1 (HA discovery + админка)</div>"
        "<div>SD: <b>%s</b>%s</div>"
        "%s"
        "<div><a href=\"/web/index.html\">Панель администратора</a></div>",
        platform::board().name, MICROOS_VERSION,
        HomeMasterApp::getInstance().modeStr(),
        sd.stateStr(), sdExtra, brokerLine);
    return (n > 0 && (size_t)n < bufSize) ? (size_t)n : bufSize - 1;
}

// ============================================================================
// API /api/dev/hm/*
// ============================================================================
bool HomeMasterUi::handleApi(const char* pathTail, const ShUiRequest& req,
                             char* responseBuf, size_t bufSize,
                             int& statusCode) {
    if (strcmp(pathTail, "hm/info") == 0) {
        statusCode = 200;
        return apiInfo(responseBuf, bufSize);
    }
    if (strcmp(pathTail, "hm/sd/remount") == 0) {
        return apiSdRemount(req, responseBuf, bufSize, statusCode);
    }
    // --- M3.2: вьюер журнала (read-only; фильтры и курсор — аргументами) ---
    if (strcmp(pathTail, "hm/journal/files") == 0) {
        statusCode = 200;
        return JournalService::getInstance().apiFiles(responseBuf, bufSize);
    }
    if (strcmp(pathTail, "hm/journal/tail") == 0) {
        statusCode = 200;
        uint16_t n = JournalService::JRN_PAGE_LINES;
        const char* a = req.getArg("n");
        if (a != nullptr) {   // arg() транзиентен — значение сразу
            long v = strtol(a, nullptr, 10);
            if (v > 0 && v <= JournalService::JRN_PAGE_LINES) n = (uint16_t)v;
        }
        return JournalService::getInstance().apiTail(responseBuf, bufSize, n);
    }
    if (strcmp(pathTail, "hm/journal/read") == 0) {
        statusCode = 200;
        // Каждый аргумент копируем ДО следующего getArg (указатель кольцевой!)
        char name[JRN_NAME_LEN] = "";
        jrn::JrnQuery q; memset(&q, 0, sizeof(q));
        uint32_t off = 0;
        const char* a = req.getArg("file");
        if (a != nullptr) { strncpy(name, a, sizeof(name) - 1); name[sizeof(name)-1] = '\0'; }
        a = req.getArg("src");
        if (a != nullptr) { strncpy(q.src, a, sizeof(q.src) - 1); q.src[sizeof(q.src)-1] = '\0'; }
        a = req.getArg("q");
        if (a != nullptr) { strncpy(q.q, a, sizeof(q.q) - 1); q.q[sizeof(q.q)-1] = '\0'; }
        a = req.getArg("offset");
        if (a != nullptr) off = (uint32_t)strtoul(a, nullptr, 10);
        a = req.getArg("from");
        if (a != nullptr) q.from = (uint32_t)strtoul(a, nullptr, 10);
        a = req.getArg("to");
        if (a != nullptr) q.to = (uint32_t)strtoul(a, nullptr, 10);
        return JournalService::getInstance().apiRead(responseBuf, bufSize,
                                                     name, off, q);
    }
    // --- 5.8.0: «Скачать журнал» — сегмент честным файлом (admin, потоково)
    if (strcmp(pathTail, "hm/journal/dl") == 0) {
        return apiJournalDl(req, responseBuf, bufSize, statusCode);
    }
    return false;   // 404 — нет такого профильного пути
}

// 5.8.0, «Скачать журнал»: потоковая отдача сегмента вложением.
// Ядру говорим status=0 («ответ уже ушёл») — HttpService НЕ шлёт JSON.
bool HomeMasterUi::apiJournalDl(const ShUiRequest& req, char* buf,
                                size_t size, int& status) {
    if (!HttpService::getInstance().isAdminToken(req.token)) {
        status = 401;
        snprintf(buf, size, "{\"ok\":0,\"err\":\"unauthorized\"}");
        return true;
    }
    // getArg() транзиентен (кольцевой буфер) — имя копируем СРАЗУ
    char name[JRN_NAME_LEN] = "";
    const char* a = req.getArg("n");
    if (a != nullptr) {
        strncpy(name, a, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    char path[JRN_NAME_LEN + 12];
    if (!JournalService::getInstance().segmentPath(name, path, sizeof(path))) {
        status = 404;
        snprintf(buf, size, "{\"ok\":0,\"err\":\"no_segment\"}");
        return true;
    }
    fs::FS* sd = SdService::getInstance().fs();
    if (sd == nullptr) {   // страховка: карта — расходник, могла уйти между проверками
        status = 503;
        snprintf(buf, size, "{\"ok\":0,\"err\":\"sd\"}");
        return true;
    }
    fs::File f = sd->open(path, FILE_READ);
    if (!f) {
        status = 404;
        snprintf(buf, size, "{\"ok\":0,\"err\":\"open\"}");
        return true;
    }
    HttpService::getInstance().streamFileDownload(f, name);
    f.close();
    status = 0;   // ответ ушёл потоком — JSON поверх не нужен
    return true;
}

bool HomeMasterUi::apiInfo(char* buf, size_t size) {
    SdService& sd = SdService::getInstance();
    BrokerService& br = BrokerService::getInstance();
    BridgeService& bg = BridgeService::getInstance();
    JournalService& jn = JournalService::getInstance();
    TimeService& ts = TimeService::getInstance();
    // Время устройства для панели (5.5.6): unix UTC (0 = недостоверно) +
    // пояс из конфига — панель тикает локально между опросами и показывает
    // локальное время по той же детерминированной схеме, что и ядро.
    snprintf(buf, size,
        "{\"ok\":1,\"board\":\"%s\",\"profile\":\"home_master\","
        "\"fw\":\"%s\",\"profile_ver\":\"%s\",\"mode\":\"%s\","
        "\"sd\":{\"enabled\":%d,\"state\":\"%s\",\"card\":\"%s\","
        "\"size_mb\":%llu,\"used_mb\":%llu},"
        "\"broker\":{\"enabled\":%d,\"running\":%d,\"port\":%u,"
        "\"clients\":%u,\"max_clients\":%u,\"retained\":%lu,"
        "\"rx\":%lu,\"rejected\":%lu},"
        "\"bridge\":{\"active\":%d,\"up\":%lu,\"down\":%lu,"
        "\"dropped\":%lu,\"synth\":%lu},"
        "\"journal\":{\"enabled\":%d,\"writing\":%d,\"degraded\":%d,"
        "\"segment\":\"%s\",\"seg_kb\":%lu,\"queued\":%u,"
        "\"dropped\":%lu,\"written\":%lu,\"last_flush\":%lu},"
        "\"mqtt_up\":%d,"
        "\"time\":{\"valid\":%d,\"unix\":%ld,\"tz\":%ld,"
        "\"ntp_synced\":%d,\"rtc\":%d,\"ntp_state\":\"%s\","
        "\"ntp_attempts\":%lu,\"ntp_rtt_ms\":%lu},"
        "\"uptime_s\":%lu,\"heap_free\":%lu,\"heap_min\":%lu,\"psram_free\":%lu}",
        platform::board().name, MICROOS_VERSION,
        HomeMasterApp::getInstance().getVersion(),
        HomeMasterApp::getInstance().modeStr(),
        cfgGetBool("sd.enabled", true) ? 1 : 0,
        sd.stateStr(), sd.cardTypeStr(), sd.sizeMb(), sd.usedMb(),
        br.enabled() ? 1 : 0, br.running() ? 1 : 0, br.port(),
        br.clients(), br.maxClients(),
        (unsigned long)br.retained(), (unsigned long)br.rxTotal(),
        (unsigned long)br.rejected(),
        bg.active() ? 1 : 0,
        (unsigned long)bg.bridgedUp(), (unsigned long)bg.bridgedDown(),
        (unsigned long)bg.dropped(), (unsigned long)bg.synthOffline(),
        jn.enabled() ? 1 : 0, jn.writing() ? 1 : 0, jn.degraded() ? 1 : 0,
        jn.segment(), (unsigned long)jn.segmentKb(), (unsigned)jn.queued(),
        (unsigned long)jn.dropped(), (unsigned long)jn.written(),
        (unsigned long)jn.lastFlushUnix(),
        MqttTransport::getInstance().isConnected() ? 1 : 0,
        ts.isTimeValid() ? 1 : 0, (long)ts.getUnixTime(),
        (long)cfgGetInt("sys.tz_offset", 3),
        ts.ntpSynced() ? 1 : 0, ts.rtcAlive() ? 1 : 0,
        ts.ntpStateStr(), (unsigned long)ts.ntpAttempts(),
        (unsigned long)ts.ntpLastRttMs(),
        (unsigned long)(millis() / 1000),
        (unsigned long)ESP.getFreeHeap(),
        (unsigned long)ESP.getMinFreeHeap(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

bool HomeMasterUi::apiSdRemount(const ShUiRequest& req, char* buf,
                                size_t size, int& status) {
    // Только админская сессия ядра (как операции базы карт у smart_lock).
    if (!HttpService::getInstance().isAdminToken(req.token)) {
        status = 401;
        snprintf(buf, size, "{\"ok\":0,\"err\":\"unauthorized\"}");
        return true;
    }
    bool ok = SdService::getInstance().tryMount();
    status = 200;
    snprintf(buf, size, "{\"ok\":%d,\"state\":\"%s\"}",
             ok ? 1 : 0, SdService::getInstance().stateStr());
    return true;
}
