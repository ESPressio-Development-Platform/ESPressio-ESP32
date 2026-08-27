# ESPressio-ESP32

ESP32-specific implementations of abstractions declared by ESPressio-System.

## Memory provider

Call `ESPressio::ESP32Platform::InstallMemoryProvider()` from top-level application startup before constructing ESPressio subsystems that should use ESP32 memory policies. Higher-level ESPressio libraries remain platform-neutral and depend only on ESPressio-System.

Policy mapping:
- Internal -> internal 8-bit-capable heap.
- ExternalPreferred -> PSRAM first, internal fallback.
- ExternalRequired -> PSRAM only.
- Automatic -> general ESP-IDF 8-bit-capable heap.
