// ============================================================================
// MicroOS.h — UMBRELLA-ЗАГОЛОВОК БИБЛИОТЕКИ МикроОС 5.0
// ============================================================================
// Подключение в .ino проекта устройства:
//   #include <MicroOS.h>
//   #include "src/<MyProfile>.h"
//   void setup() { Kernel::getInstance().run<MyProfile>(); }
//   void loop()  { Kernel::getInstance().loop(); }
//
// Здесь — только верхнеуровневые контракты. Профиль подключает конкретные
// сервисы/драйверы сам: <services/ConfigService.h>, <catalog/wiegand/...>.
// ============================================================================
#pragma once

// Ядро: жизненный цикл, события, ресурсы
#include "core/ShTypes.h"
#include "core/Events.h"
#include "core/EventBus.h"
#include "core/ResourceManager.h"
#include "core/Kernel.h"

// Контракт профиля устройства
#include "core/IDeviceProfile.h"

// Ключевые сервисы (полный список — services/*.h)
#include "services/ConfigService.h"
#include "services/StorageService.h"
#include "services/TimeService.h"
#include "services/DataLogService.h"
#include "services/AudioService.h"
#include "services/HttpService.h"
