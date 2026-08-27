#include <Arduino.h>

#include <ESPressio_ESP32.hpp>
#include <ESPressio_Memory.hpp>

void setup() {
    ESPressio::ESP32::InstallMemoryProvider();

    ESPressio::System::Memory::Vector<
        int,
        ESPressio::System::Memory::MemoryPolicy::ExternalPreferred
    > values;
    values.push_back(42);

    volatile int observed = values.front();
    (void)observed;
}

void loop() {}
