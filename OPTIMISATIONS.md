# Optimisations

## 2026-08-27

- **#1** Implemented ESP32 heap-capability mapping for ESPressio-System `MemoryPolicy`.
- **#1** Implemented PSRAM-first/internal-fallback semantics for `ExternalPreferred`.
- **#2** Added application-boundary `InstallMemoryProvider()` without introducing ESP32 dependencies into higher-level libraries.
- **#1** Renamed the provider namespace to `ESPressio::ESP32Platform` after CI exposed the Arduino/ESP32 toolchain's `ESP32` preprocessor macro collision.
- **#3** Added lock-free allocation-policy telemetry counters so hardware validation can distinguish `ExternalPreferred` PSRAM successes from internal fallback allocations and quantify requested bytes by policy without introducing provider-side dynamic allocation.