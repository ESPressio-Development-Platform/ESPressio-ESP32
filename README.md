# ESPressio ESP32

ESP32-specific implementations of ESPressio hardware/runtime abstractions and domain-provider contracts.

`ESPressio-ESP32` is the concrete platform layer beneath portable ESPressio libraries. ESP-IDF, Arduino-ESP32 and FreeRTOS APIs belong here when they satisfy an abstraction owned by ESPressio-System or another ESPressio domain library.

Portable consumers should depend on the abstraction owner rather than this repository directly. A top-level ESP32 application composes the concrete providers it needs.

## Coordinated redesign branch

During the Primitive Platform redesign tranche, consume the participating ESPressio repositories from their coordinated `primitives_redesign` branches. No version/tag/main-branch release claim is implied by the tranche implementation.

The relevant dependency direction is:

```text
ESPressio-ESP32
    -> ESPressio-System
    -> ESPressio-Radio       (provider contract)
    -> ESPressio-WiFi        (platform contract)
    -> ESPressio-Persistence (provider contracts)
```

## System providers

The normal ESP32 bootstrap can install the current System provider set together:

```cpp
#include <ESPressio_ESP32.hpp>

ESPressio::ESP32Platform::InstallSystemProviders();
```

This installs the ESP-IDF/FreeRTOS implementations for memory, execution, synchronization, bounded queues, monotonic time, high-resolution counters, GPIO and entropy.

Individual providers may be installed independently when composition requires explicit ownership.

### Memory

`InstallMemoryProvider()` installs the ESP-IDF heap-capability provider.

| System policy | ESP32 allocation |
| --- | --- |
| `Automatic` | general `MALLOC_CAP_8BIT` heap |
| `Internal` | `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` |
| `ExternalPreferred` | PSRAM first, then internal 8-bit memory |
| `ExternalRequired` | PSRAM only |

Allocator-aware ESPressio objects retain the provider active when their allocators are constructed, so install the memory provider before creating such global objects.

### Execution, synchronization and queues

`ExecutionProvider` maps the System execution contract to FreeRTOS tasks, including joinable execution, processor affinity, processor-count discovery, stack telemetry, sleep and yield.

`SynchronizationProvider` supplies System signals using FreeRTOS primitives, including ISR-safe signalling. `QueueProvider` supplies bounded message queues without exposing FreeRTOS handles to portable libraries.

### Clock and GPIO

`MonotonicClock` maps `IMonotonicClock` to `esp_timer_get_time()`. `HighResolutionCounterProvider` uses ESP-IDF GPTimer.

Both ESP-IDF and Arduino GPIO implementations are available. The ESP-IDF controller is the default System-provider path and supports explicit interrupt ownership/affinity semantics; the Arduino controller intentionally reports unsupported affinity where Arduino cannot guarantee it.

### Entropy

`EntropySource` satisfies the System entropy contract using the ESP32 hardware random source and advertises cryptographic suitability.

## Arduino byte-stream adapters

`ESPressio_ArduinoByteStream.hpp` adapts Arduino `Stream`/`Print` to System byte-stream contracts without leaking Arduino types into portable Serial/Logging code.

```cpp
ESPressio::ESP32Platform::ArduinoByteStream consoleIO(Serial);
ESPressio::ESP32Platform::ArduinoByteOutput logOutput(Serial);
```

## WiFi platform implementation

`ESPressio_WiFiPlatform.hpp` supplies the ESP32 concrete for the contract owned by `ESPressio-WiFi`. Configuration, reconnect policy, scanning and WiFi-domain lifecycle remain owned by that library; Arduino/ESP-IDF implementation details remain here.

`ESPressio_WiFiRadio.hpp` contains RF-level helpers used by native radio policy/fingerprint code.

## Managed Radio providers

ESP32 currently supplies two concrete `ESPressio::Radio::IRadio` providers:

- `Raw80211Radio` for the ESP32 raw IEEE 802.11 bearer;
- `BLERadio` for legacy non-connectable BLE advertising/scanning.

Both implement the finite managed-provider contract used by the Radio Q1/R3 runtime. Neither owns a `RadioWorker`, application callback graph, Primitive-family transport, Mesh routing engine or a second fragmentation/reassembly layer.

The Radio library owns protected Q1 storage, v3 transfer IDs, fragmentation/reassembly, class/deadline scheduling, provider completion correlation and logical terminal handoff.

### Shared WiFi physical ownership

`WiFiPhyCoordinator` represents the shared ESP32 WiFi physical contention domain. Raw 802.11 readiness follows the coordinator's cached access state, and coordinator changes wake the Radio runtime rather than requiring polling.

Raw80211 and other users of the same ESP32 WiFi PHY must therefore share one physical contention-domain identity. Radio R3 serializes scheduler-owned physical service within that domain while independent physical domains may progress concurrently.

## Raw 802.11 Radio

`ESPressio_Raw80211Radio.hpp` provides `ESPressio::ESP32Platform::Raw80211Radio`.

### Finite ingress

The ESP-IDF promiscuous RX callback performs only bounded provider work:

1. recognize the private ESPressio 802.11/LLC envelope;
2. capture/calculate provider timestamp evidence;
3. copy the accepted physical payload into a compile-time fixed SPSC ring;
4. coalesce `InboundAvailable()` to the Radio runtime.

No family decode or application callback runs in the driver callback.

`ServiceInbound()` later drains only a finite quantum. The default bound is the configured ring capacity minus one slot. `ProviderResources()` publishes both finite ingress storage and service bounds.

### Physical and logical bounds

Raw80211 exposes a **270-byte** opaque Radio physical payload and a **4096-byte** provider logical ceiling. Radio v3 owns logical fragmentation above that physical payload.

The provider adds its private 802.11/LLC/correlation envelope only below the Radio physical-packet boundary; it does not introduce another logical transfer protocol.

### Managed transmission completion

A call to `esp_wifi_80211_tx()` is only submission, not terminal transmission evidence. `Raw80211Radio` therefore embeds a provider-local correlation tag in its private frame envelope and registers the ESP-IDF raw TX callback.

`Send()` returns `Accepted` with a deferred `RadioTransmissionHandle`. The ESP-IDF TX callback latches terminal success/failure and wakes infrastructure; the normal Radio-domain service quantum later publishes `TransmissionResolved()` after R3 has had the opportunity to install that handle. A reused or stale completion cannot be treated as a current fragment completion.

Raw80211 does not claim peer acknowledgement merely because the ESP-IDF driver reported local transmission completion.

### Cost and readiness

`EstimateTransmissionCost()` publishes a finite relative cost based on the complete private physical frame size. It is intentionally classified `RelativeOnly`; it is useful for fair arbitration but is not presented as a conservative airtime bound for deadline certification.

`IsTransmitReady()` requires the provider to be started, shared WiFi-phy access to be currently available, and no provider-local deferred transmission to be outstanding.

### Receive timestamp evidence

The ESP-IDF `wifi_pkt_rx_ctrl_t::timestamp` field is a WiFi-local 32-bit microsecond timer. `Raw80211Radio` extends it across wraps and aligns it to System monotonic time using the minimum observed callback lag over a fixed recent window.

The resulting historical coordinate is useful evidence, but the current implementation deliberately publishes:

```text
RadioTimestampCaptureSource::Driver
RadioTimestampQuality::Estimated
HasCaptureModel = false
```

It therefore does **not** satisfy the conservative finite capture/model evidence required for certified K1/K2 Clock synchronization. Software must not convert this estimate into a sub-millisecond certification claim until target characterization establishes and exposes a defensible worst-case bound and capture-time model.

## BLE Radio

`ESPressio_BLERadio.hpp` provides `ESPressio::ESP32Platform::BLERadio` using legacy non-connectable advertising plus passive scanning.

BLE is an opaque Radio bearer only. Mesh membership/routing, Primitive semantics and application policy remain outside this provider.

### Redeveloped v3-compatible envelope

The predecessor BLE envelope spent six bytes carrying a redundant destination address and exposed only 20 opaque payload bytes. That could not carry the locked Radio v3 fixed prefix plus six-byte source address.

The redesigned provider treats legacy advertising honestly as a broadcast bearer and removes that redundant destination field. One manufacturer-specific advertising structure now contains:

```text
2 bytes manufacturer company identifier
1 byte ESPressio frame marker
N bytes opaque Radio physical packet
```

After the legacy advertising structure overhead this exposes exactly **26 opaque Radio bytes**.

With the v3 15-byte fixed prefix and six-byte source address, BLE leaves five logical payload bytes per fragment and therefore advertises the exact v3-compatible logical maximum:

```text
5 * 255 = 1275 bytes
```

`Send()` rejects non-broadcast destinations; the provider does not pretend legacy advertisements are unicast.

### Bounded ingress and TX ownership

The scan callback performs bounded recognition/copy into a fixed ring and signals `InboundAvailable()`. `ServiceInbound()` drains a finite quantum.

Only one BLE TX campaign may be outstanding. `Send()` configures the raw advertising payload and returns a deferred transmission handle. After the configured finite advertising dwell, advertising is stopped and terminal completion is published through the normal managed-provider sink. There is no hidden unbounded advertisement queue above R3.

`ProviderResources()` reports the fixed receive-ring bound and a provider TX queue depth of one.

### Cost and Clock limitations

BLE cost is finite but `RelativeOnly`, based on configured advertising dwell plus payload size. It is suitable for fairness accounting but not promoted-deadline certification.

The Bluedroid scan-result callback does not expose a demonstrated provider-proximate receive timestamp. `BLERadio` therefore advertises neither `ReceiveTimestamp` nor `TransmitTimestamp` and returns unqualified timestamp evidence.

The ordinary v3 bearer is now compatible, but the exact compact Radio Clock response is **32 bytes**, larger than BLE's 26-byte physical payload. Consequently this legacy-advertising provider cannot carry that certified single-packet Clock profile and must not be represented as a sub-millisecond Clock bearer.

### Global BLE callback ownership

The current Bluedroid GAP callback is process-global, so at most one started `BLERadio` instance owns that callback coordination. Other application code must not replace the callback while the provider is active.

WiFi may operate concurrently through ESP32 WiFi/Bluetooth coexistence, but throughput/latency under coexistence remains a target-level validation concern.

## Persistence providers

Hardware-backed Persistence implementations remain supplied here rather than by `ESPressio-Persistence` itself. These include Preferences/NVS, LittleFS, SPIFFS, FFat, SPI SD and SD/MMC backends. Atomic replacement, serialization/schema migration and persistence policy remain owned by the Persistence domain.

## Namespace

System and concrete platform providers live in:

```cpp
ESPressio::ESP32Platform
```

rather than `ESPressio::ESP32`, because ESP32 toolchains may define `ESP32` as a preprocessor macro.

## Architectural boundary

The ownership rule is:

```text
generic hardware/runtime contract       -> ESPressio-System
domain contract/policy                  -> domain library
ESP32 implementation of either          -> ESPressio-ESP32
```

ESP32-specific code may know ESP-IDF, Arduino-ESP32 and FreeRTOS. Portable libraries should not repeat direct platform calls when an ESPressio abstraction exists.

## Compatibility and requirements

- ESP32 with the corresponding current ESP-IDF/Arduino-ESP32 facilities;
- C++17;
- no RTTI requirement for the platform layer itself;
- `BLERadio` is exposed only when BLE + Bluedroid are enabled by the target SDK configuration;
- Arduino-specific adapters are exposed only under the Arduino framework.