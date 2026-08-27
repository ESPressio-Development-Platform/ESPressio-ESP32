#pragma once
#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_ESP32PersistenceFileStorageBase.hpp"
#include <LittleFS.h>
namespace ESPressio::Persistence {
class LittleFSStorage final : public ESP32FileStorageBase {
public:
    explicit LittleFSStorage(bool formatOnFailure = false)
        : ESP32FileStorageBase(LittleFS), _formatOnFailure(formatOnFailure) {}
    const char* GetBackendName() const override { return "LittleFS"; }
    StorageStatus Initialize() override {
        if (_ready) return StorageStatus::AlreadyInitialized;
        if (!LittleFS.begin(_formatOnFailure)) return StorageStatus::IoError;
        _ready = true;
        return StorageStatus::Success;
    }
    void Shutdown() override {
        if (_ready) LittleFS.end();
        _ready = false;
    }
    StorageStatistics GetStatistics() const override {
        StorageStatistics value{};
        if (!_ready) return value;
        value.totalBytes = LittleFS.totalBytes();
        value.usedBytes = LittleFS.usedBytes();
        value.freeBytes = value.totalBytes >= value.usedBytes ? value.totalBytes - value.usedBytes : 0;
        value.capacityKnown = true;
        return value;
    }
private:
    bool _formatOnFailure;
};
} // namespace ESPressio::Persistence
#endif
