#pragma once
#if defined(ARDUINO_ARCH_ESP32)
#include <ESPressio_IKeyValueStorage.hpp>
#include <Preferences.h>
#include <cstring>
#include <string>
namespace ESPressio::Persistence {
class PreferencesStorage final : public IKeyValueStorage {
public:
    static constexpr std::size_t MaximumNamespaceLength = 15;
    static constexpr std::size_t MaximumKeyLength = 15;

    explicit PreferencesStorage(const char* nameSpace, bool readOnly = false)
        : _namespace(nameSpace == nullptr ? "" : nameSpace), _readOnly(readOnly) {}
    StorageStatus Initialize() override {
        if (_ready) return StorageStatus::AlreadyInitialized;
        if (_namespace.empty() || _namespace.size() > MaximumNamespaceLength) return StorageStatus::InvalidArgument;
        if (!_preferences.begin(_namespace.c_str(), _readOnly)) return StorageStatus::IoError;
        _ready = true; return StorageStatus::Success;
    }
    void Shutdown() override { if (_ready) _preferences.end(); _ready = false; }
    bool IsReady() const override { return _ready; }
    const char* GetBackendName() const override { return "Preferences/NVS"; }
    StorageCapability GetCapabilities() const override { return StorageCapability::KeyValue | StorageCapability::AtomicReplace; }
    StorageStatistics GetStatistics() const override { return {}; }
    StorageStatus Contains(const char* key, bool& exists) const override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!ValidKey(key)) return StorageStatus::InvalidArgument;
        exists = _preferences.isKey(key); return StorageStatus::Success;
    }
    StorageStatus GetSize(const char* key, std::size_t& size) const override {
        size = 0; if (!_ready) return StorageStatus::NotInitialized;
        if (!ValidKey(key)) return StorageStatus::InvalidArgument;
        if (!_preferences.isKey(key)) return StorageStatus::NotFound;
        size = _preferences.getBytesLength(key); return StorageStatus::Success;
    }
    StorageStatus Read(const char* key, uint8_t* buffer, std::size_t capacity, std::size_t& bytesRead) const override {
        bytesRead = 0; if (!_ready) return StorageStatus::NotInitialized;
        if (!ValidKey(key) || (buffer == nullptr && capacity != 0)) return StorageStatus::InvalidArgument;
        const std::size_t required = _preferences.getBytesLength(key);
        if (required == 0 && !_preferences.isKey(key)) return StorageStatus::NotFound;
        if (capacity < required) return StorageStatus::NoSpace;
        bytesRead = required == 0 ? 0 : _preferences.getBytes(key, buffer, capacity);
        return bytesRead == required ? StorageStatus::Success : StorageStatus::IoError;
    }
    StorageStatus Write(const char* key, const uint8_t* data, std::size_t size) override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (_readOnly) return StorageStatus::PermissionDenied;
        if (!ValidKey(key) || (data == nullptr && size != 0)) return StorageStatus::InvalidArgument;
        const std::size_t written = _preferences.putBytes(key, data, size);
        return written == size ? StorageStatus::Success : StorageStatus::PartialWrite;
    }
    StorageStatus Remove(const char* key) override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (_readOnly) return StorageStatus::PermissionDenied;
        if (!ValidKey(key)) return StorageStatus::InvalidArgument;
        if (!_preferences.isKey(key)) return StorageStatus::NotFound;
        return _preferences.remove(key) ? StorageStatus::Success : StorageStatus::IoError;
    }
    StorageStatus Clear() override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (_readOnly) return StorageStatus::PermissionDenied;
        return _preferences.clear() ? StorageStatus::Success : StorageStatus::IoError;
    }
private:
    static bool ValidKey(const char* value) { return value != nullptr && *value != '\0' && std::strlen(value) <= MaximumKeyLength; }
    mutable Preferences _preferences;
    std::string _namespace;
    bool _readOnly = false;
    bool _ready = false;
};
} // namespace ESPressio::Persistence
#endif
