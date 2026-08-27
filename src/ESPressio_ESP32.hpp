#pragma once

#include "ESPressio_MemoryProvider.hpp"
#include "ESPressio_ExecutionProvider.hpp"
#include "ESPressio_SynchronizationProvider.hpp"
#include "ESPressio_QueueProvider.hpp"
#include "ESPressio_ClockProvider.hpp"
#include "ESPressio_IDFGPIOProvider.hpp"
#include "ESPressio_EntropyProvider.hpp"

#if defined(ARDUINO)
#include "ESPressio_ArduinoGPIOProvider.hpp"
#endif

#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_ESP32WiFi.hpp"
#include "ESPressio_ESP32WiFiRadio.hpp"
#endif

namespace ESPressio::ESP32Platform {

inline void InstallSystemProviders() noexcept {
    InstallMemoryProvider();
    InstallExecutionProvider();
    InstallSynchronizationProvider();
    InstallQueueProvider();
    InstallClockProviders();
    InstallGPIOController();
    InstallEntropySource();
}

} // namespace ESPressio::ESP32Platform
