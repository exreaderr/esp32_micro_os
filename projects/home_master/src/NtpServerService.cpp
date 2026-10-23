// ============================================================================
// NtpServerService.cpp — M4-NTP: мастер как NTP-сервер парка (0.9.8)
// ============================================================================
#include "NtpServerService.h"
#include <services/ConfigService.h>
#include <services/TimeService.h>
#include <core/Version.h>
#include <sys/time.h>
#include <lwip/udp.h>
#include <lwip/tcpip.h>
#include <string.h>

// --- дальше recv-колбэк исполняется в tcpip_thread (урок 5.5.7) ----------
static void ntpsOnRecv(void* arg, udp_pcb* pcb, pbuf* p,
                       const ip_addr_t* addr, u16_t port) {
    NtpServerService* self = static_cast<NtpServerService*>(arg);
    if (p == nullptr) return;
    if (p->tot_len < sh_ntps::PACKET_LEN) { pbuf_free(p); return; }

    uint8_t req[sh_ntps::PACKET_LEN];
    pbuf_copy_partial(p, req, sizeof(req), 0);
    const u16_t srcPort = port;
    ip_addr_t dst;
    ip_addr_copy(dst, *addr);
    pbuf_free(p);
    self->noteRx();

    // Режим клиента (3) — единственный, кому мы обязаны ответом; прочее
    // (broadcast, symmetric) молча игнорируем, как вежливый сервер.
    if ((req[0] & 0x07) != 3) return;

    const bool synced = TimeService::getInstance().isTimeValid() &&
                        TimeService::getInstance().ntpSynced();
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    uint8_t out[sh_ntps::PACKET_LEN];
    sh_ntps::buildReply(req, (uint32_t)tv.tv_sec, (uint32_t)tv.tv_usec,
                        synced, out);

    pbuf* r = pbuf_alloc(PBUF_TRANSPORT, sh_ntps::PACKET_LEN, PBUF_RAM);
    if (r == nullptr) return;
    pbuf_take(r, out, sh_ntps::PACKET_LEN);
    err_t err = udp_sendto(pcb, r, &dst, srcPort);
    pbuf_free(r);
    if (err == ERR_OK) self->noteTx(!synced);
}

static void ntpsTcpipUp(void* arg) {   // start() из loopTask → исполнение здесь
    NtpServerService* self = static_cast<NtpServerService*>(arg);
    udp_pcb* pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == nullptr) return;
    if (udp_bind(pcb, IP_ANY_TYPE, sh_ntps::PORT) != ERR_OK) {
        udp_remove(pcb);
        return;
    }
    udp_recv(pcb, ntpsOnRecv, self);
    self->setPcb(pcb);
}

static void ntpsTcpipDown(void* arg) {
    NtpServerService* self = static_cast<NtpServerService*>(arg);
    void* cur = self->pcbRaw();
    if (cur != nullptr) {
        udp_remove(static_cast<udp_pcb*>(cur));
        self->setPcb(nullptr);
    }
}

// ============================================================================
NtpServerService& NtpServerService::getInstance() {
    static NtpServerService instance;
    return instance;
}

void NtpServerService::init() {
    _enabled = cfgGetBool("ntps.enabled", true);
    log(LogLevel::Info, "NTP-сервер: %s (M4-NTP)",
        _enabled ? "включён" : "выключен конфигом");
}

void NtpServerService::start() {
    if (!_enabled) return;
    tcpip_callback(ntpsTcpipUp, this);
    // Старт-подтверждение залогируем из tick, когда pcb реально появится
    // (tcpip_callback асинхронен по построению).
}

void NtpServerService::stop() {
    if (_pcb != nullptr) tcpip_callback(ntpsTcpipDown, this);
}

void NtpServerService::tick() {
    if (!_enabled) return;
    const uint32_t now = millis();
    static bool s_announced = false;
    if (!s_announced && _pcb != nullptr) {
        s_announced = true;
        log(LogLevel::Info,
            "NTP-сервер слушает udp/123 (stratum %u при синхре, "
            "LI=3 когда нет)", (unsigned)sh_ntps::STRATUM_OK);
    }
    // Сводка раз в минуту, только если была активность (не мусорим в лог).
    if (now - _lastLogMs >= 60000) {
        _lastLogMs = now;
        const uint32_t rx = _rx;
        if (rx != _lastLogRx) {
            _lastLogRx = rx;
            log(LogLevel::Info, "ntps: запросов %lu, ответов %lu "
                "(из них «не синхронизирован» %lu)",
                (unsigned long)rx, (unsigned long)_tx,
                (unsigned long)_txUnsynced);
        }
    }
}
