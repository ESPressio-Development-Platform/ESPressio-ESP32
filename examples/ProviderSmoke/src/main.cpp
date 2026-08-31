#include <Arduino.h>

#include <ESPressio_ESP32.hpp>
#include <ESPressio_Memory.hpp>
#include <ESPressio_Radio.hpp>
#include <ESPressio_Raw80211Radio.hpp>

namespace {
ESPressio::Radio::RadioTransport radioTransport(1);
ESPressio::Radio::RadioWorker radioWorker(radioTransport);
ESPressio::ESP32Platform::Raw80211Radio rawRadio;
}

void setup() {
    ESPressio::ESP32Platform::InstallMemoryProvider();

    ESPressio::System::Memory::Vector<
        int,
        ESPressio::System::Memory::MemoryPolicy::ExternalPreferred
    > values;
    values.push_back(42);

    const auto statistics =
        ESPressio::ESP32Platform::GetMemoryProvider().Statistics();

    // Compile the worker-owned inbound path without starting RF in this smoke target.
    const bool interfaceAttached = radioWorker.AddInterface(rawRadio, true);

    volatile int observed = values.front();
    volatile uint32_t requests = statistics.ExternalPreferredRequests;
    volatile bool attached = interfaceAttached;
    (void)observed;
    (void)requests;
    (void)attached;
}

void loop() {}
