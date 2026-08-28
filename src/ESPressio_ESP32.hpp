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
#include "ESPressio_WiFiPlatform.hpp"
#include "ESPressio_WiFiRadio.hpp"
#if __has_include(<ESPressio_HttpServer.hpp>)
#include "ESPressio_HttpServerPlatform.hpp"
#endif
#if __has_include(<ESPressio_Dns.hpp>)
#include "ESPressio_DnsServerPlatform.hpp"
#endif
#if defined(CONFIG_HTTPD_WS_SUPPORT) && __has_include(<ESPressio_WebSocketEndpoint.hpp>)
#include "ESPressio_WebSocketEndpointPlatform.hpp"
#endif
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
