# ESPressio ESP32

ESP32-specific implementations of abstractions declared by ESPressio System.

**Release target:** `0.1.0`

The first implementation supplied by this library is an ESP-IDF heap-capability-backed System memory provider. It lets platform-neutral ESPressio libraries request internal or PSRAM-backed storage without those libraries depending directly on ESP-IDF.

## When to use it

Use ESPressio ESP32 in the **top-level ESP32 application** when one or more ESPressio libraries use `ESPressio::System::Memory` policies and you want ESP32-aware allocation behaviour.

Higher-level libraries should depend on ESPressio System only; they should not depend on this repository.

## Installation during coordinated development

```ini
lib_deps =
    https://github.com/ESPressio-Development-Platform/ESPressio-System.git#feature/1-system-memory-policy
    https://github.com/ESPressio-Development-Platform/ESPressio-ESP32.git#feature/1-system-memory-provider
```

After the staged releases are published, use the released package versions instead of development branches.

## Installing the memory provider

```cpp
#include <ESPressio_ESP32.hpp>

ESPressio::ESP32Platform::InstallMemoryProvider();
```

The provider maps System policies as follows:

| System policy | ESP32 allocation |
| --- | --- |
| `Automatic` | General `MALLOC_CAP_8BIT` heap. |
| `Internal` | `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. |
| `ExternalPreferred` | PSRAM (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`) first, then internal 8-bit memory if external allocation fails. |
| `ExternalRequired` | PSRAM only; allocation fails if PSRAM cannot satisfy the request. |

On ESP32 variants without PSRAM, `ExternalPreferred` therefore remains portable by falling back internally. `ExternalRequired` is intentionally strict.

## Install it before allocator-aware global objects

System allocators capture the active provider when the allocator is constructed. Installing the provider halfway through `setup()` is therefore too late for application-global ESPressio objects that have already constructed their allocator-aware containers.

A top-level application should arrange installation before those globals are constructed. One Arduino-compatible approach is an early bootstrap object declared before ESPressio subsystem globals:

```cpp
#include <ESPressio_ESP32.hpp>

namespace {
struct ESPressioPlatformBootstrap {
    ESPressioPlatformBootstrap() {
        ESPressio::ESP32Platform::InstallMemoryProvider();
    }
};

// Choose construction order appropriate for your application/toolchain. The
// important contract is that this runs before allocator-aware ESPressio globals.
__attribute__((init_priority(101)))
ESPressioPlatformBootstrap espressioPlatformBootstrap;
}
```

Alternatively, avoid allocator-aware ESPressio globals and construct your subsystems explicitly after calling `InstallMemoryProvider()`.

The latter is the most portable C++ lifecycle pattern.

## Runtime allocation statistics

The provider records allocation requests and bytes by policy:

```cpp
const auto stats = ESPressio::ESP32Platform::MemoryProvider().Statistics();

Serial.printf(
    "external preferred: %u requests / %u bytes, "
    "external success: %u / %u bytes, fallback: %u / %u bytes\n",
    stats.ExternalPreferredRequests,
    stats.ExternalPreferredBytes,
    stats.ExternalPreferredExternalSuccesses,
    stats.ExternalPreferredExternalBytes,
    stats.ExternalPreferredInternalFallbacks,
    stats.ExternalPreferredInternalFallbackBytes
);
```

This is useful for confirming that a PSRAM-equipped device is genuinely offloading eligible ESPressio storage rather than silently consuming internal DRAM.

## What the provider does not move

Installing this provider does **not** move every ESP32 allocation into PSRAM. A library must explicitly request a System memory policy. Native ESP-IDF WiFi/ESP-NOW memory, FreeRTOS task stacks/queues, DMA buffers, ISR-critical storage and other platform-owned allocations remain under their appropriate platform allocation rules.

This distinction is deliberate: PSRAM is appropriate for long-lived application metadata and non-DMA buffers, but not for every real-time or peripheral resource.

## Namespace

The provider lives in:

```cpp
ESPressio::ESP32Platform
```

rather than `ESPressio::ESP32`, because Arduino/ESP32 toolchains may define `ESP32` as a preprocessor macro.

## Compatibility and requirements

- ESP32 / ESP-IDF heap capabilities are required.
- The current implementation is used from Arduino-ESP32 but is based on ESP-IDF heap APIs.
- C++17 is required by the coordinated ESPressio generation.
- No RTTI is required; integration CI builds with RTTI disabled.
- ESPressio System remains the abstraction boundary and is the only mandatory ESPressio dependency.

See `OPTIMISATIONS.md` for the implementation history and `CHANGELOG.md` for release-facing changes.
