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
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>

namespace ESPressio::ESP32Platform::Detail {

inline esp_err_t StartHttpServerExternalPreferred(
    httpd_handle_t* handle,
    const httpd_config_t* configuration
) {
    if (handle == nullptr || configuration == nullptr) return ESP_ERR_INVALID_ARG;

    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != 0) {
        httpd_config_t externalConfiguration = *configuration;
        externalConfiguration.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        const esp_err_t externalResult = ::httpd_start(handle, &externalConfiguration);
        if (externalResult == ESP_OK) return ESP_OK;
        if (externalResult != ESP_ERR_HTTPD_TASK) return externalResult;
        *handle = nullptr;
    }

    return ::httpd_start(handle, configuration);
}

} // namespace ESPressio::ESP32Platform::Detail

#define httpd_start(...) \
    ::ESPressio::ESP32Platform::Detail::StartHttpServerExternalPreferred(__VA_ARGS__)
#include "ESPressio_HttpServerPlatform.hpp"
#undef httpd_start
#endif
#if __has_include(<ESPressio_Dns.hpp>)
#include "ESPressio_DnsServerPlatform.hpp"
#endif
#if defined(CONFIG_HTTPD_WS_SUPPORT) && __has_include(<ESPressio_WebSocketEndpoint.hpp>)
#include "ESPressio_WebSocketEndpointPlatform.hpp"
#endif
#if __has_include(<ESPressio_WebSocketClient.hpp>) && __has_include(<esp_websocket_client.h>)
#include "ESPressio_WebSocketClientPlatform.hpp"
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
