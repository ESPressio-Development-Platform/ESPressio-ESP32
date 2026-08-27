# ESPressio ESP32

ESP32-specific implementations of ESPressio hardware/runtime abstractions and applicable higher-level platform contracts.

**Release target:** `0.1.0`

ESPressio-ESP32 is the concrete platform layer beneath portable ESPressio libraries. ESP-IDF, Arduino-ESP32 and FreeRTOS APIs belong here when they are used to satisfy an abstraction owned by ESPressio-System or a higher-level domain library.

## When to use it

Use ESPressio-ESP32 from a top-level ESP32 application to install the concrete providers required by platform-neutral ESPressio code.

Higher-level libraries should depend on their abstraction owners, not on ESPressio-ESP32 itself. For example, generic runtime consumers depend on ESPressio-System; WiFi-domain code depends on ESPressio-WiFi. ESPressio-ESP32 supplies the target-specific implementations.

## Installation during coordinated development

```ini
lib_deps =
    https://github.com/ESPressio-Development-Platform/ESPressio-System.git#feature/1-system-memory-policy
    https://github.com/ESPressio-Development-Platform/ESPressio-ESP32.git#feature/1-system-memory-provider
```

After the coordinated releases are published, use released versions rather than development branches.

## Installing the System providers

The complete current System provider set can be installed together:

```cpp
#include <ESPressio_ESP32.hpp>

ESPressio::ESP32Platform::InstallSystemProviders();
```

This installs:

- the ESP-IDF heap-capability memory provider;
- the FreeRTOS execution provider;
- the FreeRTOS binary-signal provider;
- the `esp_timer` monotonic clock;
- the GPTimer high-resolution counter provider;
- the ESP-IDF GPIO controller.

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

Runtime allocation statistics remain available through `MemoryProvider().Statistics()`.

## Execution and synchronization

`ESP32ExecutionProvider` maps the System execution contract onto FreeRTOS tasks. It provides task creation/destruction, suspend/resume, current-task identity, stack high-water telemetry, sleep/yield and processor affinity.

Native `TaskHandle_t` values do not leave this provider; callers see opaque System execution handles.

`ESP32SynchronizationProvider` supplies binary System signals using FreeRTOS semaphores, including ISR-context signalling.

## Clock providers

`ESP32MonotonicClock` maps `IMonotonicClock` to `esp_timer_get_time()` and exposes nanosecond-form timestamps.

`ESP32HighResolutionCounterProvider` creates dedicated counters backed by ESP-IDF GPTimer. Higher-level libraries therefore do not need to expose `gptimer_handle_t`, `esp_err_t` or GPTimer driver headers.

Native ESP-IDF results are translated into `System::PlatformResult`; the original numeric error code is retained only as optional diagnostic information.

## GPIO

Two concrete GPIO implementations are provided.

### ESP-IDF GPIO provider

The default `InstallSystemProviders()` path installs `ESP32GPIOController`, which maps System GPIO configuration/read/write operations directly to ESP-IDF GPIO facilities.

Interrupt creation returns a System `InterruptCreationResult` containing both an explicit status and a move-only RAII handle. Destroying/resetting the handle removes the registered ISR handler. The handle can also be enabled and disabled while retained.

ESP32-specific CPU/core affinity is supported as a request:

```cpp
using namespace ESPressio::System;
using namespace ESPressio::System::GPIO;

InterruptConfiguration interruptConfig;
interruptConfig.Trigger = InterruptTrigger::RisingEdge;
interruptConfig.Affinity = ProcessorAffinity::Specific(1);

auto created = ESP32Platform::GPIOController().CreateInterrupt(
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

Providers live in:

```cpp
ESPressio::ESP32Platform
```

rather than `ESPressio::ESP32`, because Arduino/ESP32 toolchains may define `ESP32` as a preprocessor macro.

## Architectural boundary

ESPressio-ESP32 is allowed to know ESP-IDF, Arduino-ESP32 and FreeRTOS. Portable libraries should not repeat direct platform calls when an ESPressio abstraction exists.

Domain-specific abstractions remain with their domain libraries. For example, the concrete ESP32 WiFi implementation belongs here, while `IWiFiPlatform`, WiFi lifecycle and WiFi configuration remain owned by ESPressio-WiFi.

## Compatibility and requirements

- ESP32 with current ESP-IDF/Arduino-ESP32 facilities.
- C++17.
- No RTTI requirement.
- ESPressio-System is the base abstraction dependency.
- The Arduino GPIO provider is only exposed when compiling with the Arduino framework.

`PLATFORM_ABSTRACTIONS.md` records the abstraction tranche in detail. See `OPTIMISATIONS.md` for memory-provider implementation history and `CHANGELOG.md` for release-facing changes.
