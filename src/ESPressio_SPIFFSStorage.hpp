#pragma once
#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_FileStorageBase.hpp"
#include <SPIFFS.h>
namespace ESPressio::Persistence {
/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 16 bytes [0 bytes dynamic allocation]
 * Members:
 * - _formatOnFailure (bool): 1 bytes [0 bytes dynamic allocation]
 * Total Memory: 20 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
class SPIFFSStorage final : public FileStorageBase {
public:
    explicit SPIFFSStorage(bool formatOnFailure = false)
        : FileStorageBase(SPIFFS), _formatOnFailure(formatOnFailure) {}
    const char* GetBackendName() const override { return "SPIFFS"; }
    StorageStatus Initialize() override {
        if (_ready) return StorageStatus::AlreadyInitialized;
        if (!SPIFFS.begin(_formatOnFailure)) return StorageStatus::IoError;
        _ready = true;
        return StorageStatus::Success;
    }
    void Shutdown() override {
        if (_ready) SPIFFS.end();
        _ready = false;
    }
    StorageStatistics GetStatistics() const override {
        StorageStatistics value{};
        if (!_ready) return value;
        value.totalBytes = SPIFFS.totalBytes();
        value.usedBytes = SPIFFS.usedBytes();
        value.freeBytes = value.totalBytes >= value.usedBytes ? value.totalBytes - value.usedBytes : 0;
        value.capacityKnown = true;
        return value;
    }
private:
    bool _formatOnFailure;
};
} // namespace ESPressio::Persistence
#endif
