#include <Arduino.h>
#include <type_traits>

#include <ESPressio_MemoryProvider.hpp>
#include <ESPressio_ExecutionProvider.hpp>
#include <ESPressio_SynchronizationProvider.hpp>
#include <ESPressio_QueueProvider.hpp>
#include <ESPressio_ClockProvider.hpp>
#include <ESPressio_IDFGPIOProvider.hpp>
#include <ESPressio_EntropyProvider.hpp>
#include <ESPressio_ArduinoByteStream.hpp>
#include <ESPressio_PersistenceBackends.hpp>
#include <ESPressio_WiFiPlatform.hpp>

using namespace ESPressio;

static_assert(std::is_base_of_v<System::Memory::IMemoryProvider, ESP32Platform::MemoryProvider>);
static_assert(std::is_base_of_v<System::Execution::IExecutionProvider, ESP32Platform::ExecutionProvider>);
static_assert(std::is_base_of_v<System::Synchronization::ISynchronizationProvider, ESP32Platform::SynchronizationProvider>);
static_assert(std::is_base_of_v<System::Queue::IQueueProvider, ESP32Platform::QueueProvider>);
static_assert(std::is_base_of_v<System::Clock::IMonotonicClock, ESP32Platform::MonotonicClock>);
static_assert(std::is_base_of_v<System::Clock::IHighResolutionCounterProvider, ESP32Platform::HighResolutionCounterProvider>);
static_assert(std::is_base_of_v<System::GPIO::IController, ESP32Platform::IDFGPIOController>);
static_assert(std::is_base_of_v<System::Entropy::IEntropySource, ESP32Platform::EntropySource>);
static_assert(std::is_base_of_v<System::IO::IByteInput, ESP32Platform::ArduinoByteInput>);
static_assert(std::is_base_of_v<System::IO::IByteOutput, ESP32Platform::ArduinoByteOutput>);
static_assert(std::is_base_of_v<System::IO::IByteStream, ESP32Platform::ArduinoByteStream>);
static_assert(std::is_base_of_v<Persistence::IKeyValueStorage, Persistence::PreferencesStorage>);
static_assert(std::is_base_of_v<Persistence::IFileStorage, Persistence::LittleFSStorage>);
static_assert(std::is_base_of_v<Persistence::IFileStorage, Persistence::SPIFFSStorage>);
static_assert(std::is_base_of_v<Persistence::IFileStorage, Persistence::FFatStorage>);
static_assert(std::is_base_of_v<Persistence::IFileStorage, Persistence::SDStorage>);
static_assert(std::is_base_of_v<Persistence::IFileStorage, Persistence::SDMMCStorage>);
static_assert(std::is_base_of_v<WiFi::IWiFiPlatform, WiFi::WiFiPlatform>);

void setup() {
    ESP32Platform::InstallMemoryProvider();
    ESP32Platform::InstallExecutionProvider();
    ESP32Platform::InstallSynchronizationProvider();
    ESP32Platform::InstallQueueProvider();
    ESP32Platform::InstallClockProviders();
    ESP32Platform::InstallGPIOController();
    ESP32Platform::InstallEntropySource();

    volatile bool clockAvailable =
        ESP32Platform::GetMonotonicClock().ResolutionNanoseconds() <= 1000ULL;
    volatile bool affinityKnown =
        ESP32Platform::GetExecutionProvider().ProcessorCount() >= 1U;
    volatile bool gpioInterrupts =
        ESP32Platform::IDFGPIO().SupportsInterrupts();
    volatile bool entropySuitable =
        ESP32Platform::GetEntropySource().IsCryptographicallySuitable();

    (void)clockAvailable;
    (void)affinityKnown;
    (void)gpioInterrupts;
    (void)entropySuitable;
}

void loop() {}
