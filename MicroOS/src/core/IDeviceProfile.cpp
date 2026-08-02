// ============================================================================
// IDeviceProfile.cpp — валидация манифеста периферии
// ============================================================================
#include "IDeviceProfile.h"
#include "ResourceManager.h"
#include <Arduino.h>

bool HardwareManifest::validateResources(ResourceManager& rm) const {
    // --- Типизированные пины (только задействованные, т.е. != -1) ---------
    // safeModePin валидировать НЕ нужно: он принадлежит ядру (системная
    // кнопка), профиль лишь сообщает номер.
    struct PinOwner { int8_t pin; const char* role; };
    const PinOwner typed[] = {
        { dfPlayerRx,   "profile.dfplayer.rx"   },
        { dfPlayerTx,   "profile.dfplayer.tx"   },
        { dfPlayerBusy, "profile.dfplayer.busy" },
    };
    for (const auto& po : typed) {
        if (po.pin < 0) continue;
        if (!rm.claimGpio((uint8_t)po.pin, po.role)) return false;
    }

    // UART DFPlayer, если объявлен
    if (dfPlayerUart != 0xFF) {
        if (!rm.claimUart(dfPlayerUart, "profile.dfplayer")) return false;
    }

    // --- Произвольные GPIO профиля -----------------------------------------
    for (uint8_t i = 0; i < gpioCount; ++i) {
        if (!rm.claimGpio(gpio[i].pin, gpio[i].role)) return false;
    }

    return true;
}
