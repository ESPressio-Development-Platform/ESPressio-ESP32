#include <Arduino.h>

#include <ESPressio_ESP32.hpp>
#include <ESPressio_Memory.hpp>

void setup() {
    ESPressio::ESP32Platform::InstallMemoryProvider();

    ESPressio::System::Memory::Vector<
        int,
        ESPressio::System::Memory::MemoryPolicy::ExternalPreferred
    > values;
    values.push_back(42);

    const auto statistics =
        ESPressio::ESP32Platform::GetMemoryProvider().Statistics();

    ESPressio::Web::ESP32HttpServerPlatform httpPlatform;
    ESPressio::Web::HttpServer httpServer(httpPlatform);
    const auto httpCapabilities = httpPlatform.Capabilities();

    volatile int observed = values.front();
    volatile uint32_t requests = statistics.ExternalPreferredRequests;
    volatile bool supportsHttp = ESPressio::Web::HasCapability(
        httpCapabilities,
        ESPressio::Web::WebCapability::Http
    );
    (void)observed;
    (void)requests;
    (void)supportsHttp;
}

void loop() {}
