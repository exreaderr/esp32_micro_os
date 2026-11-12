// ============================================================================
// NetworkIpTransition.h — M3.4-Info-IP1: чистая классификация переходов IPv4
// ============================================================================
// Не зависит от Arduino/ESP, heap и String. NetworkService передаёт сюда
// предыдущий кэш и фактический адрес из ETH.localIP(), полученный в tick().
// Нулевой фактический адрес при hasIp=true не считается достоверной сменой:
// кэш и предыдущий признак IP сохраняются до следующего достоверного снимка.
// ============================================================================
#pragma once

#include <cstdint>

namespace network_ip {

enum class TransitionKind : uint8_t {
    None = 0,
    Acquired,   // не было достоверного IP -> появился ненулевой адрес
    Changed,    // ненулевой A -> другой ненулевой B без промежуточной потери
    Lost        // был IP -> колбэк/состояние сообщает об его отсутствии
};

struct Transition {
    TransitionKind kind;
    uint32_t previous;
    uint32_t current;
};

inline Transition classify(bool previousHasIp, uint32_t previousIp,
                           bool hasIp, uint32_t actualIp) {
    Transition result{TransitionKind::None, previousIp,
                      hasIp ? actualIp : 0U};
    if (!hasIp) {
        if (previousHasIp) result.kind = TransitionKind::Lost;
        return result;
    }
    if (actualIp == 0U) return result;
    if (!previousHasIp) {
        result.kind = TransitionKind::Acquired;
    } else if (previousIp != actualIp) {
        result.kind = TransitionKind::Changed;
    }
    return result;
}

inline bool emitsIpChanged(TransitionKind kind) {
    return kind == TransitionKind::Acquired || kind == TransitionKind::Changed;
}

} // namespace network_ip
