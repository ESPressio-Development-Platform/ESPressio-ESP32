#pragma once

#include "ESPressio_ESP32MemoryProvider.hpp"
#include "ESPressio_ESP32ExecutionProvider.hpp"
#include "ESPressio_ESP32SynchronizationProvider.hpp"
#include "ESPressio_ESP32QueueProvider.hpp"
#include "ESPressio_ESP32ClockProvider.hpp"
#include "ESPressio_ESP32GPIOProvider.hpp"

#if defined(ARDUINO)
#include "ESPressio_ArduinoGPIOProvider.hpp"
#endif

namespace ESPressio::ESP32Platform {

inline void InstallSystemProviders() noexcept {
    InstallMemoryProvider();
    InstallExecutionProvider();
    InstallSynchronizationProvider();
    InstallQueueProvider();
    InstallClockProviders();
    InstallGPIOController();
}

} // namespace ESPressio::ESP32Platform
