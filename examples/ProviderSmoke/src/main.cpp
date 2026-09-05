#include <Arduino.h>

#include <ESPressio_ESP32.hpp>

using MeshCapacityProfile = ESPressio::ESP32Platform::InternalMemoryMeshCapacityProfile<8192U, 16384U>;
static_assert(MeshCapacityProfile::Identifier == 0x45533332U);
static_assert(MeshCapacityProfile::InboundDeliveryPool::MaximumBytesPerSlot == 4096U);
static_assert(MeshCapacityProfile::ControlFramePool::MaximumBytesPerSlot == 512U);
static_assert(MeshCapacityProfile::ApplicationPayloadPool::MaximumBytesPerSlot == 3584U);
#include <ESPressio_Memory.hpp>
#include <ESPressio_Radio.hpp>
#include <ESPressio_Raw80211Radio.hpp>

namespace {
ESPressio::Radio::RadioTransport radioTransport;
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

    // Compile the worker-owned direct-link inbound path without starting RF in this smoke target.
    // Route selection is intentionally absent: it belongs to ESPressio-Mesh, not RadioWorker/RadioTransport.
    const bool interfaceAttached = radioWorker.AddInterface(rawRadio);

    volatile int observed = values.front();
    volatile uint32_t requests = statistics.ExternalPreferredRequests;
    volatile bool attached = interfaceAttached;
    (void)observed;
    (void)requests;
    (void)attached;
}

void loop() {}
