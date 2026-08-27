#pragma once
#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_ESP32PersistenceFileStorageBase.hpp"
#include <SPIFFS.h>
namespace ESPressio::Persistence {
class SPIFFSStorage final : public ESP32FileStorageBase {
public:
    explicit SPIFFSStorage(bool formatOnFailure = false)
        : ESP32FileStorageBase(SPIFFS), _formatOnFailure(formatOnFailure) {}
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
