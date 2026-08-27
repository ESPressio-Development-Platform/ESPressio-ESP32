# Platform Abstractions Audit Trail

This file records changes made during the platform-abstraction tranche tracked by issue #5.

## 2026-08-27

### Execution
- Added an ESP32 `IExecutionProvider` backed by FreeRTOS tasks.
- Native task handles remain inside ESPressio-ESP32; higher-level libraries consume opaque System execution handles.
- Task creation supports requested processor affinity, priority and stack sizing.
- Current-execution identity, minimum-free-stack telemetry, sleep and yield are mapped to FreeRTOS here.

### Synchronization
- Added a FreeRTOS-backed binary `ISignal` implementation.
- Normal and ISR-context signalling, timeout waits and reset are translated into the System synchronization contract.

### Clocking
- Added an ESP32 monotonic clock backed by `esp_timer_get_time()`.
- Added an ESP-IDF GPTimer-backed high-resolution counter provider.
- `esp_err_t` values are translated to `PlatformResult` and retained only as optional native diagnostic codes.

### GPIO
- Added an ESP-IDF GPIO controller for configuration, reads and writes.
- Added RAII interrupt registrations using the ESP-IDF GPIO ISR service.
- Specific CPU/core affinity can be requested. Because the ESP-IDF GPIO ISR service itself has a service-core affinity, the first installed GPIO interrupt service establishes that core; a later incompatible affinity request reports `Conflict` rather than silently changing behaviour.
- Added an Arduino GPIO implementation for Arduino-ESP32 applications. It implements the same System contract but explicitly reports specific interrupt affinity as unsupported.
- Interrupt creation preserves an explicit `PlatformResult` alongside the owned RAII interrupt handle.

### Provider bootstrap
- Expanded `ESPressio_ESP32.hpp` to expose all current providers.
- Added `InstallSystemProviders()` to install memory, execution, synchronization, clock and IDF GPIO providers together.
- `InstallArduinoGPIOController()` can intentionally replace only the GPIO provider when an Arduino-facing implementation is preferred.

## Boundary rule

ESP-IDF, Arduino-ESP32 and FreeRTOS types/calls belong in this repository when satisfying portable System abstractions or higher-level domain interfaces. Higher-level libraries should not reproduce those calls when an ESPressio abstraction already exists.
