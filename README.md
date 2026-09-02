# ESPressio ESP32

ESP32-specific implementations of ESPressio hardware/runtime abstractions and applicable higher-level platform contracts.

**Release target:** `0.1.0`

ESPressio-ESP32 is the concrete platform layer beneath portable ESPressio libraries. ESP-IDF, Arduino-ESP32 and FreeRTOS APIs belong here when they are used to satisfy an abstraction owned by ESPressio-System or a higher-level domain library.

The repository name supplies the platform context. Concrete capability names inside this package therefore do **not** redundantly repeat `ESP32`; where two implementation APIs coexist, the API is used as the discriminator instead, such as `IDFGPIOController` and `ArduinoGPIOController`.

## When to use it

Use ESPressio-ESP32 from a top-level ESP32 application to install concrete providers and compose target-specific implementations required by portable ESPressio code.

Higher-level libraries should depend on their abstraction owners, not on ESPressio-ESP32 itself. Generic runtime consumers depend on ESPressio-System; WiFi-domain code depends on ESPressio-WiFi; persistence-domain code depends on ESPressio-Persistence. ESPressio-ESP32 supplies the ESP32 implementations.

## Installation during coordinated development

```ini
lib_deps =
    https://github.com/ESPressio-Development-Platform/ESPressio-System.git#main
    https://github.com/ESPressio-Development-Platform/ESPressio-ESP32.git#main
```

ESPressio-ESP32 currently also consumes the active WiFi and Persistence contracts because it provides their concrete implementations:

```text
ESPressio-ESP32
    -> ESPressio-System
    -> ESPressio-WiFi        (contract only)
    -> ESPressio-Persistence (contracts only)
```

During the release restructuring, consume ESPressio dependencies from their `main` branches until the new platform-wide 1.0.0 release generation is published.

## Installing the System providers

The current System provider set can be installed together:

```cpp
#include <ESPressio_ESP32.hpp>

ESPressio::ESP32Platform::InstallSystemProviders();
```

This installs:

- the ESP-IDF heap-capability memory provider;
- the FreeRTOS execution provider;
- the FreeRTOS binary-signal provider;
- the FreeRTOS bounded-message-queue provider;
- the `esp_timer` monotonic clock;
- the GPTimer high-resolution counter provider;
- the ESP-IDF GPIO controller;
- the hardware entropy source.

Individual providers can also be installed independently when the application needs explicit control.

## Memory provider

```cpp
ESPressio::ESP32Platform::InstallMemoryProvider();
```

| System policy | ESP32 allocation |
| --- | --- |
| `Automatic` | General `MALLOC_CAP_8BIT` heap. |
| `Internal` | `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. |
| `ExternalPreferred` | PSRAM first, then internal 8-bit memory if external allocation fails. |
| `ExternalRequired` | PSRAM only. |

On devices without PSRAM, `ExternalPreferred` remains portable by falling back internally while `ExternalRequired` remains strict.

System allocators capture the active memory provider when constructed. Install the provider before allocator-aware global ESPressio objects are created, or construct those objects explicitly after platform bootstrap.

Runtime allocation statistics are available through:

```cpp
ESPressio::ESP32Platform::GetMemoryProvider().Statistics();
```

## Execution, synchronization and queues

`ExecutionProvider` maps the System execution contract onto FreeRTOS tasks. It provides task creation/destruction, suspend/resume, current-task identity, stack high-water telemetry, processor-count discovery, sleep/yield and processor affinity.

Native `TaskHandle_t` values do not leave this provider; callers see opaque System execution handles.

`SynchronizationProvider` supplies binary System signals using FreeRTOS semaphores, including ISR-context signalling.

`QueueProvider` supplies bounded message queues using FreeRTOS queues, including callback/ISR-safe enqueue. Higher-level libraries can therefore use deterministic queueing without exposing `QueueHandle_t`, `TickType_t` or related RTOS types.

## Clock providers

`MonotonicClock` maps `IMonotonicClock` to `esp_timer_get_time()` and exposes nanosecond-form timestamps.

`HighResolutionCounterProvider` creates dedicated counters backed by ESP-IDF GPTimer. Higher-level libraries therefore do not need to expose `gptimer_handle_t`, `esp_err_t` or GPTimer driver headers.

Native ESP-IDF results are translated into `System::PlatformResult`; the original numeric error code is retained only as optional diagnostic information.

## GPIO

Two concrete GPIO implementations are provided.

### ESP-IDF GPIO provider

The default `InstallSystemProviders()` path installs `IDFGPIOController`, which maps System GPIO configuration/read/write operations directly to ESP-IDF GPIO facilities.

Interrupt creation returns a System `InterruptCreationResult` containing both an explicit status and a move-only RAII handle. Destroying/resetting the handle removes the registered ISR handler. The handle can also be enabled and disabled while retained.

Specific CPU/core affinity is supported as a request:

```cpp
using namespace ESPressio::System;
using namespace ESPressio::System::GPIO;

InterruptConfiguration interruptConfig;
interruptConfig.Trigger = InterruptTrigger::RisingEdge;
interruptConfig.Affinity = ProcessorAffinity::Specific(1);

auto created = ESPressio::ESP32Platform::IDFGPIO().CreateInterrupt(
    26,
    interruptConfig,
    callback,
    context
);
```

ESP-IDF's GPIO ISR service itself is installed on one processor/core. The first registration that establishes the service therefore fixes that service affinity. A later incompatible specific-core request returns `PlatformStatus::Conflict` rather than silently violating the requested affinity.

### Arduino GPIO provider

Arduino-ESP32 applications can intentionally use the Arduino-facing implementation instead:

```cpp
ESPressio::ESP32Platform::InstallArduinoGPIOController();
```

It uses Arduino `pinMode`, `digitalRead`, `digitalWrite`, `attachInterruptArg` and `detachInterrupt` while satisfying the same System GPIO contract.

The Arduino provider advertises that specific processor affinity is unsupported. A specific-affinity interrupt request returns `PlatformStatus::Unsupported`; `ProcessorAffinity::Any()` remains valid.

## Hardware entropy

`EntropySource` satisfies `System::Entropy::IEntropySource` using the hardware random generator and advertises cryptographic suitability.

Security therefore consumes platform entropy through System rather than calling target APIs directly:

```cpp
ESPressio::ESP32Platform::InstallEntropySource();
```

`InstallSystemProviders()` already includes this source. ESPressio-Security's `RandomSource` then consumes the installed System entropy source without any Security-side ESP32 dependency.

## Arduino byte-stream adapters

`ESPressio_ArduinoByteStream.hpp` adapts Arduino framework streams without leaking `Stream` or `Print` into reusable libraries:

```cpp
#include <ESPressio_ArduinoByteStream.hpp>

ESPressio::ESP32Platform::ArduinoByteStream consoleIO(Serial);
ESPressio::ESP32Platform::ArduinoByteOutput logOutput(Serial);
```

The adapters implement `System::IO::IByteInput`, `IByteOutput` and `IByteStream`. Serial-domain parsing, formatting and logging remain in ESPressio-Serial.

## WiFi platform implementation

The concrete WiFi implementation lives in this repository while the contract and all WiFi-domain policy remain in ESPressio-WiFi:

```cpp
#include <ESPressio_WiFiPlatform.hpp>

ESPressio::WiFi::WiFiPlatform wifiPlatform;
```

`ESPressio::WiFi::IWiFiPlatform`, configuration, runtime state, reconnect policy, scanning and lifecycle remain owned by ESPressio-WiFi. Only Arduino/ESP-IDF implementation details—`WiFi.h`, `esp_wifi`, `esp_netif`, DHCP and radio access—belong here.

`ESPressio_WiFiRadio.hpp` contains RF-level helpers used when native radio policy/fingerprints are required. Its public helper names are likewise contextual (`WiFiRadioFingerprint`, `ReadWiFiRadioFingerprint`, `ApplyWiFiRadioPolicy`) rather than redundantly platform-qualified.

## Persistence platform implementations

Hardware-backed Persistence providers are supplied here rather than by ESPressio-Persistence itself:

```cpp
#include <ESPressio_PersistenceBackends.hpp>
```

Available implementations include:

- `PreferencesStorage` — Preferences/NVS;
- `LittleFSStorage`;
- `SPIFFSStorage`;
- `FFatStorage`;
- `SDStorage` — SPI SD;
- `SDMMCStorage` — native SD/MMC.

They implement the `IKeyValueStorage` / `IFileStorage` contracts owned by ESPressio-Persistence. Atomic replacement, serialization integration, schema migration and persistence policy remain in that domain library.

## Platform bootstrap example

```cpp
#include <ESPressio_ESP32.hpp>

namespace {
struct ESPressioPlatformBootstrap {
    ESPressioPlatformBootstrap() {
        ESPressio::ESP32Platform::InstallSystemProviders();
    }
};

__attribute__((init_priority(101)))
ESPressioPlatformBootstrap espressioPlatformBootstrap;
}
```

Construction order remains especially important for the memory provider because allocator-aware objects retain the provider active when their allocators are constructed.

## Namespace

System platform providers live in:

```cpp
ESPressio::ESP32Platform
```

rather than `ESPressio::ESP32`, because Arduino/ESP32 toolchains may define `ESP32` as a preprocessor macro. The namespace identifies the implementation package; contained provider type names remain contextual and neutral.

Domain implementations continue to satisfy interfaces in their domain namespaces; for example `ESPressio::WiFi::WiFiPlatform` implements `ESPressio::WiFi::IWiFiPlatform`.

## Architectural boundary

ESPressio-ESP32 is allowed to know ESP-IDF, Arduino-ESP32 and FreeRTOS. Portable libraries should not repeat direct platform calls when an ESPressio abstraction exists.

The ownership rule is:

```text
generic hardware/runtime contract       -> ESPressio-System
domain contract/policy                  -> domain library
ESP32 implementation of either          -> ESPressio-ESP32
```

This prevents ESPressio-System from becoming a catch-all while preventing every higher-level library from independently binding to ESP32 APIs.

## Compatibility and requirements

- ESP32 with current ESP-IDF/Arduino-ESP32 facilities.
- C++17.
- No RTTI requirement for the platform layer itself.
- ESPressio-System is the base abstraction dependency.
- WiFi/Persistence dependencies are present because this repository implements those domain contracts.
- Arduino-specific adapters are exposed only when compiling with the Arduino framework.

`PLATFORM_ABSTRACTIONS.md` records the abstraction tranche in detail. See `OPTIMISATIONS.md` for memory-provider implementation history and `CHANGELOG.md` for release-facing changes.
