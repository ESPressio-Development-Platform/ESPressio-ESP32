#pragma once
#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_ESP32PersistenceFileStorageBase.hpp"
#include <SD.h>
#include <SPI.h>
namespace ESPressio::Persistence {
class SDStorage final : public ESP32FileStorageBase {
public:
    explicit SDStorage(uint8_t chipSelectPin = SS, uint32_t frequency = 4000000U)
        : ESP32FileStorageBase(SD, StorageCapability::Removable),
          _chipSelectPin(chipSelectPin), _frequency(frequency) {}
    const char* GetBackendName() const override { return "SD/SPI"; }
    StorageStatus Initialize() override {
        if (_ready) return StorageStatus::AlreadyInitialized;
        if (!SD.begin(_chipSelectPin, SPI, _frequency)) return StorageStatus::IoError;
        _ready = true;
        return StorageStatus::Success;
    }
    void Shutdown() override {
        if (_ready) SD.end();
        _ready = false;
    }
    StorageStatistics GetStatistics() const override {
        StorageStatistics value{};
        if (!_ready) return value;
        value.totalBytes = SD.totalBytes();
        value.usedBytes = SD.usedBytes();
        value.freeBytes = value.totalBytes >= value.usedBytes ? value.totalBytes - value.usedBytes : 0;
        value.capacityKnown = value.totalBytes != 0;
        return value;
    }
private:
    uint8_t _chipSelectPin;
    uint32_t _frequency;
};
} // namespace ESPressio::Persistence
#endif
