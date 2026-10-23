// ============================================================================
// NtpServerService.h — M4-NTP: мастер как NTP-сервер парка (0.9.8)
// ============================================================================
// Концепция §M4: «WAN отключён — время парка едино». Мастер, уже имеющий
// СВОЙ SNTP-клиент (TimeService/SntpCore, 5.5.7), становится раздающим
// для контура: UDP/123, ответы NTPv4 (mode 4). Устройства парка просто
// переключают sys.ntp_server на 10.146.75.50 — прошивка устройств не
// трогается (клиентская часть ядра уже вся на месте).
//
// ЧЕСТНОСТЬ (решение 23.10.2026): время раздаём только при живой
// собственной синхронизации (TimeService::ntpSynced). Не синхронизирован —
// отвечаем LI=3 (clock unsynchronized) + stratum 16: клиент по RFC такой
// ответ не защёлкнет (и наш parseReply отбракует ts < 2025-01-01), но RTT
// и диагностика проходят. Молчать НЕЛЬЗЯ: клиент сидел бы в таймаутах и
// думал, что сеть сломана, а не часы.
//
// Транспорт — сырой lwIP UDP, по уроку 5.5.7: udp_* трогаем ТОЛЬКО из
// tcpip_thread. Здесь это получается бесплатно: создание pcb — через
// tcpip_callback из start/stop, ответ клиенту — прямо в recv-колбэке
// (он уже исполняется в tcpip_thread). Своя задача не нужна вообще.
//
// Формат ответа (48 байт, RFC 5905 §7.3):
//   [0]    LI(0/3)<<6 | VN4<<3 | mode4      — 0x24 synced / 0xE4 unsynced
//   [1]    stratum: 2 (LAN-вторичный) / 16 (unsynchronized)
//   [2]    poll — эхо из запроса
//   [3]    precision 0xEC (~2^-20 c — достаточно честно для lwIP-тика)
//   [4..11]  root delay/dispersion = 0 (не врём про величины, которых нет)
//   [12..15] refid 'M','i','O','S'
//   [16..23] reference timestamp = now (точность до последней синхры —
//            величина известна только TimeService, поле информативное)
//   [24..31] originate = transmit из запроса (обязательно: RTT клиента)
//   [32..39] receive = now
//   [40..47] transmit = now (перед самой отправкой)
// ============================================================================
#pragma once
#include <core/ModuleBase.h>
#include <cstdint>
#include <cstddef>
#include <initializer_list>

namespace sh_ntps {

constexpr size_t   PACKET_LEN   = 48;
constexpr uint16_t PORT         = 123;
constexpr uint32_t EPOCH_OFFSET = 2208988800UL;   // 1900 → 1970
constexpr uint8_t  STRATUM_OK   = 2;              // LAN-вторичный сервер
constexpr uint8_t  STRATUM_UNSYNC = 16;           // «не синхронизирован»

/// Собрать ответ на NTP-запрос. req — 48 байт запроса; unixSec/usec —
/// текущее время (UTC); synced — честность сервера. Чистая функция
/// (host-тестируемая, паттерн SntpCore).
inline void buildReply(const uint8_t* req, uint32_t unixSec, uint32_t usec,
                       bool synced, uint8_t* out) {
    for (size_t i = 0; i < PACKET_LEN; ++i) out[i] = 0;
    out[0] = (uint8_t)(((synced ? 0 : 3) << 6) | (4 << 3) | 4);  // 0x24/0xE4
    out[1] = synced ? STRATUM_OK : STRATUM_UNSYNC;
    out[2] = req[2];                                  // poll — эхо
    out[3] = 0xEC;                                    // precision ~1 мкс
    out[12] = 'M'; out[13] = 'i'; out[14] = 'O'; out[15] = 'S';
    const uint32_t sec  = unixSec + EPOCH_OFFSET;
    const uint32_t frac = (uint32_t)(((uint64_t)usec << 32) / 1000000ULL);
    for (int off : {16, 32, 40}) {                    // ref, recv, xmit = now
        out[off + 0] = (uint8_t)(sec >> 24); out[off + 1] = (uint8_t)(sec >> 16);
        out[off + 2] = (uint8_t)(sec >> 8);  out[off + 3] = (uint8_t)sec;
        out[off + 4] = (uint8_t)(frac >> 24); out[off + 5] = (uint8_t)(frac >> 16);
        out[off + 6] = (uint8_t)(frac >> 8);  out[off + 7] = (uint8_t)frac;
    }
    for (int i = 0; i < 8; ++i) out[24 + i] = req[40 + i];  // originate = эхо
    if (!synced) {  // без валидного времени все метки нулевые (честность)
        for (int off : {16, 32, 40}) for (int i = 0; i < 8; ++i) out[off + i] = 0;
    }
}

} // namespace sh_ntps

class NtpServerService : public ModuleBase {
public:
    static NtpServerService& getInstance();

    const char* getName() const override { return "NtpServerService"; }
    const char* getVersion() const override { return "0.1.0-ntps"; }
    ModuleId getModuleId() const override { return 0x110B; }   // hm: ... 0x110A=PanelDisplay, 0x110B=NtpServer (0x1107 занят дважды — известный долг Backup/HmArchive, не трогаем в этом ките)
    void init() override;
    void start() override;
    void stop() override;
    void tick() override;
    // Событий не слушаем: работаем в udp-колбэке, время читаем из
    // TimeService по требованию.
    void onEvent(int32_t, const ShEventData*) override {}
    bool canHandleEvent(int32_t) const override { return false; }
    bool isReady() const override { return _pcb != nullptr || !_enabled; }

    bool running() const { return _pcb != nullptr; }
    uint32_t rxTotal()      const { return _rx; }
    uint32_t txTotal()      const { return _tx; }
    uint32_t unsyncedReplies() const { return _txUnsynced; }

    // Счётчики для recv-колбэка (писатель — tcpip_thread, читатель —
    // loopTask; volatile uint32 атомарен на Xtensa).
    void noteRx()          { _rx++; }
    void noteTx(bool unsynced) { _tx++; if (unsynced) _txUnsynced++; }
    void setPcb(void* pcb) { _pcb = pcb; }   // писатель — tcpip_callback
    void* pcbRaw() const   { return _pcb; }

private:
    NtpServerService() = default;

    void* _pcb = nullptr;              // udp_pcb*, жизнь — tcpip_thread
    volatile uint32_t _rx = 0;         // запросов принято
    volatile uint32_t _tx = 0;         // ответов отправлено
    volatile uint32_t _txUnsynced = 0; // из них LI=3/stratum16
    uint32_t _lastLogRx = 0;           // для раз в минуту сводки в tick
    uint32_t _lastLogMs = 0;
    bool     _enabled = false;         // снимок ntps.enabled на init
};
