# ESPressio ESP32

ESP32-specific implementations of ESPressio hardware/runtime abstractions and applicable higher-level platform contracts.

**Release target:** `0.1.0`

ESPressio-ESP32 is the concrete platform layer beneath portable ESPressio libraries. ESP-IDF, Arduino-ESP32 and FreeRTOS APIs belong here when they are used to satisfy an abstraction owned by ESPressio-System or a higher-level domain library.

The repository name supplies the platform context. Concrete capability names inside this package therefore do **not** redundantly repeat `ESP32`; where two implementation APIs coexist, the API is used as the discriminator instead, such as `IDFGPIOController` and `ArduinoGPIOController`.

## When to use it

Use ESPressio-ESP32 from a top-level ESP32 application to install concrete providers and compose target-specific implementations required by portable ESPressio code.

Higher-level libraries should depend on their abstraction owners, not on ESPressio-ESP32 itself. Generic runtime consumers depend on ESPressio-System; Radio-domain code depends on ESPressio-Radio; WiFi-domain code depends on ESPressio-WiFi; persistence-domain code depends on ESPressio-Persistence. ESPressio-ESP32 supplies the ESP32 implementations.

## Installation during coordinated development

```ini
lib_deps =
    https://github.com/ESPressio-Development-Platform/ESPressio-System.git#main
    https://github.com/ESPressio-Development-Platform/ESPressio-ESP32.git#main
```

ESPressio-ESP32 currently also consumes the active Radio, WiFi and Persistence contracts because it provides their concrete implementations:

```text
ESPressio-ESP32
    -> ESPressio-System
    -> ESPressio-Radio       (contract only)
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

## BLE Radio implementation

`ESPressio_BLERadio.hpp` provides `ESPressio::ESP32Platform::BLERadio`, a concrete implementation of the hardware-neutral `ESPressio::Radio::IRadio` contract using the ESP32 integrated Bluetooth Low Energy radio.

The baseline bearer deliberately uses legacy non-connectable advertising and passive scanning so it remains applicable to original ESP32-class BLE hardware. BLE is treated only as an opaque Radio bearer: ESPressio-Mesh routing, membership, admission, identities, primitive semantics and policy remain outside this provider.

```cpp
#include <ESPressio_ESP32.hpp>
#include <ESPressio_Radio.hpp>

ESPressio::ESP32Platform::BLERadioConfiguration configuration;
configuration.ManufacturerCompanyIdentifier = 0xFFFF; // development/testing identifier

ESPressio::ESP32Platform::BLERadio bleRadio(configuration);
ESPressio::Radio::RadioTransport radioTransport;
ESPressio::Radio::RadioWorker radioWorker(radioTransport);

radioWorker.AddInterface(bleRadio);
bleRadio.Start();
```

All peers sharing this bearer must use the same `ManufacturerCompanyIdentifier`. `0xFFFF` is the Bluetooth SIG testing value and should be replaced with an appropriately assigned company identifier for a production product.

Legacy advertising provides a 20-byte physical ESPressio Radio payload after the BLE advertisement envelope. `RadioTransport` remains responsible for bounded hop-local fragmentation/reassembly above that physical MTU; `BLERadio` does not duplicate transport fragmentation or acquire Mesh responsibilities.

### Mesh capacity profile

`InternalMemoryMeshCapacityProfile<MeshTaskStackBytes, OtherApplicationAndCompositionBytes>` is the named ESP32 Mesh v1 build profile. It selects 4096 bytes for each complete inbound Mesh delivery, 512 bytes per protected control slot, 3584 bytes per bounded-owned application payload, and requires Radio to compile with four 4096-byte reassembly slots. The application payload value leaves room for both the end-to-end and hop-protection headers/tags inside one 4096-byte Radio logical transfer; the control value holds the largest hop-wrapped confirmed v1 handshake.

The two template arguments are deliberately mandatory. A shipping firmware must account its actual Mesh task stacks and all other application/composition storage instead of inheriting invented platform defaults. Pass the resulting type, concrete security authority and compiled Radio transport to `MeshWholeDeviceMemoryAccounting` for the target-native total.

Transmit packets are accepted into a bounded queue and advertised for a configurable dwell interval. Receive GAP callbacks perform only bounded recognition/copying and wake `RadioWorker`; packet delivery and observer notification are deferred to worker context.

The current Bluedroid GAP callback is process-global, so one started `BLERadio` instance owns GAP callback coordination. Independent application code must not replace that callback while the Radio is active. WiFi may continue to operate concurrently through the ESP32 WiFi/Bluetooth coexistence facilities; actual throughput/latency under application load remains target-dependent and should be validated on-device.

### Clock-synchronisation limitation

`BLERadio` intentionally does **not** advertise `RadioCapability::ReceiveTimestamp` or `RadioCapability::TransmitTimestamp` at present. The ordinary Bluedroid scan-result callback does not expose a demonstrated near-RF receive timestamp, and callback arrival time must not be presented as sufficiently precise timing evidence.

This is especially important for applications requiring reliable inter-node synchronization below 1 ms. BLE may be used as the Mesh bearer independently of whether it later qualifies as a high-precision Timing bearer. Timestamp capabilities should only be enabled after controller/driver-level timing characterization under realistic BLE/WiFi coexistence load demonstrates adequate worst-case uncertainty margin.

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

Domain implementations continue to satisfy interfaces in their domain namespaces; for example `ESPressio::WiFi::WiFiPlatform` implements `ESPressio::WiFi::IWiFiPlatform` and `ESPressio::ESP32Platform::BLERadio` implements `ESPressio::Radio::IRadio`.

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
- Radio/WiFi/Persistence dependencies are present because this repository implements those domain contracts.
- `BLERadio` is exposed when the target SDK configuration enables BLE with the Bluedroid host.
- Arduino-specific adapters are exposed only when compiling with the Arduino framework.

`PLATFORM_ABSTRACTIONS.md` records the abstraction tranche in detail. See `OPTIMISATIONS.md` for memory-provider implementation history and `CHANGELOG.md` for release-facing changes.
