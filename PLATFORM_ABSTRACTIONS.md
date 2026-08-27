# Platform Abstractions Audit Trail

This file records changes made during the platform-abstraction tranche tracked by issue #5.

## 2026-08-27

### Execution
- Added an `IExecutionProvider` backed by FreeRTOS tasks.
- Native task handles remain inside ESPressio-ESP32; higher-level libraries consume opaque System execution handles.
- Task creation supports requested processor affinity, priority and stack sizing.
- Current-execution identity, minimum-free-stack telemetry, processor-count discovery, sleep and yield are mapped to FreeRTOS here.

### Synchronization and queues
- Added a FreeRTOS-backed binary `ISignal` implementation.
- Normal and ISR-context signalling, timeout waits and reset are translated into the System synchronization contract.
- Added a FreeRTOS-backed bounded message-queue provider, including callback/ISR-safe enqueue support.

### Clocking
- Added a monotonic clock backed by `esp_timer_get_time()`.
- Added an ESP-IDF GPTimer-backed high-resolution counter provider when the selected ESP-IDF/Arduino-ESP32 toolchain exposes `driver/gptimer.h`.
- Older ESP-IDF generations without the GPTimer driver still install the monotonic clock and report the dedicated high-resolution-counter capability as unavailable rather than failing the platform package to compile.
- Added `HasGPTimerHighResolutionCounter()` for platform-side capability inspection without exposing a native GPTimer type.
- `esp_err_t` values are translated to `PlatformResult` and retained only as optional native diagnostic codes.

### GPIO
- Added an ESP-IDF GPIO controller for configuration, reads and writes.
- Added RAII interrupt registrations using the ESP-IDF GPIO ISR service.
- Specific CPU/core affinity can be requested. Because the ESP-IDF GPIO ISR service itself has a service-core affinity, the first installed GPIO interrupt service establishes that core; a later incompatible affinity request reports `Conflict` rather than silently changing behaviour.
- Added an Arduino GPIO implementation. It implements the same System contract but explicitly reports specific interrupt affinity as unsupported.
- Interrupt creation preserves an explicit `PlatformResult` alongside the owned RAII interrupt handle.

### Entropy
- Added an entropy source backed by `esp_fill_random()`.
- The provider advertises cryptographic suitability through the System entropy contract.
- ESPressio-Security now consumes this provider rather than calling the native random API directly.

### Byte I/O
- Added Arduino `Stream`/`Print` adapters implementing System byte-input/output contracts.
- Framework-specific stream objects therefore remain at the application/platform boundary while Serial logging and console facilities can consume portable byte interfaces.

### Domain platform implementations
- Moved the concrete WiFi platform implementation and radio helpers from ESPressio-WiFi into this repository while retaining `IWiFiPlatform` and WiFi-domain policy in ESPressio-WiFi.
- Moved ESPressio-Persistence concrete storage backends here: Preferences/NVS, LittleFS, SPIFFS, FFat, SD/SPI and SD_MMC, together with their shared Arduino `fs::FS` implementation base.
- ESPressio-ESP32 depends on the corresponding WiFi/Persistence working branches solely to implement their domain-owned contracts.
- Concrete Arduino networking transports are being relocated here from ESPressio-Sockets while socket framing/protocol/domain infrastructure remains in ESPressio-Sockets.

### Naming
- Concrete capabilities inside the ESPressio-ESP32 package do not redundantly repeat the hardware-platform name. Canonical names include `MemoryProvider`, `ExecutionProvider`, `SynchronizationProvider`, `QueueProvider`, `MonotonicClock`, `HighResolutionCounter`, `EntropySource` and the neutral Persistence backend names.
- Where multiple implementation APIs coexist, the API is used as the discriminator rather than the hardware platform. GPIO therefore provides `IDFGPIOController` and `ArduinoGPIOController`.
- Removed the earlier platform-qualified provider/backend files rather than retaining aliases or compatibility names.
- The repository/package identity and its top-level `ESPressio_ESP32.hpp` umbrella remain ESP32-specific by definition; that package context is not repeated in the contained capability names.

### Provider bootstrap
- Expanded `ESPressio_ESP32.hpp` to expose the current System providers.
- `InstallSystemProviders()` installs memory, execution, synchronization, queue, clock, GPIO and entropy providers together.
- `InstallArduinoGPIOController()` can intentionally replace only the GPIO provider when an Arduino-facing implementation is preferred.

## Boundary rule

ESP-IDF, Arduino and FreeRTOS types/calls belong in this repository when satisfying portable System abstractions or higher-level domain interfaces. Higher-level libraries should not reproduce those calls when an ESPressio abstraction already exists.
