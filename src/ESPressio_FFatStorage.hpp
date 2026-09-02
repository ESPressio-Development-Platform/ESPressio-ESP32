#pragma once
#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_FileStorageBase.hpp"
#include <FFat.h>
namespace ESPressio::Persistence {
class FFatStorage final : public FileStorageBase {
public:
    explicit FFatStorage(bool formatOnFailure = false)
        : FileStorageBase(FFat), _formatOnFailure(formatOnFailure) {}
    const char* GetBackendName() const override { return "FFat"; }
    StorageStatus Initialize() override {
        if (_ready) return StorageStatus::AlreadyInitialized;
        if (!FFat.begin(_formatOnFailure)) return StorageStatus::IoError;
        _ready = true;
        return StorageStatus::Success;
    }
    void Shutdown() override {
        if (_ready) FFat.end();
        _ready = false;
    }
    StorageStatistics GetStatistics() const override {
        StorageStatistics value{};
        if (!_ready) return value;
        value.totalBytes = FFat.totalBytes();
        value.usedBytes = FFat.usedBytes();
        value.freeBytes = value.totalBytes >= value.usedBytes ? value.totalBytes - value.usedBytes : 0;
        value.capacityKnown = true;
        return value;
    }
private:
    bool _formatOnFailure;
};
} // namespace ESPressio::Persistence
#endif
