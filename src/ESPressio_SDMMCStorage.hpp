#pragma once
#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_FileStorageBase.hpp"
#include <SD_MMC.h>
namespace ESPressio::Persistence {
class SDMMCStorage final : public FileStorageBase {
public:
    explicit SDMMCStorage(bool oneBitMode = false, bool formatOnFailure = false)
        : FileStorageBase(SD_MMC, StorageCapability::Removable),
          _oneBitMode(oneBitMode), _formatOnFailure(formatOnFailure) {}
    const char* GetBackendName() const override { return "SD_MMC"; }
    StorageStatus Initialize() override {
        if (_ready) return StorageStatus::AlreadyInitialized;
        if (!SD_MMC.begin("/sdcard", _oneBitMode, _formatOnFailure)) return StorageStatus::IoError;
        _ready = true;
        return StorageStatus::Success;
    }
    void Shutdown() override {
        if (_ready) SD_MMC.end();
        _ready = false;
    }
    StorageStatistics GetStatistics() const override {
        StorageStatistics value{};
        if (!_ready) return value;
        value.totalBytes = SD_MMC.totalBytes();
        value.usedBytes = SD_MMC.usedBytes();
        value.freeBytes = value.totalBytes >= value.usedBytes ? value.totalBytes - value.usedBytes : 0;
        value.capacityKnown = value.totalBytes != 0;
        return value;
    }
private:
    bool _oneBitMode;
    bool _formatOnFailure;
};
} // namespace ESPressio::Persistence
#endif
