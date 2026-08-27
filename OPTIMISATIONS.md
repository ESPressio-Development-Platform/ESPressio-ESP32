# Optimisations

## 2026-08-27

- **#1** Implemented ESP32 heap-capability mapping for ESPressio-System `MemoryPolicy`.
- **#1** Implemented PSRAM-first/internal-fallback semantics for `ExternalPreferred`.
- **#2** Added application-boundary `InstallMemoryProvider()` without introducing ESP32 dependencies into higher-level libraries.
