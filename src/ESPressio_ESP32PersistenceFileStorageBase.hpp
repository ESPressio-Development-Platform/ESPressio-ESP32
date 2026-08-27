#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include <ESPressio_IFileStorage.hpp>
#include <FS.h>
#include <cstring>

namespace ESPressio::Persistence {

class ESP32FileStorageBase : public IFileStorage {
public:
    bool IsReady() const override { return _ready; }

    StorageCapability GetCapabilities() const override {
        return StorageCapability::Hierarchical |
               StorageCapability::Directories |
               StorageCapability::Rename |
               StorageCapability::Append |
               StorageCapability::CapacityReporting |
               StorageCapability::AtomicReplace |
               _additionalCapabilities;
    }

    StorageStatus Exists(const char* path, bool& exists) const override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(path)) return StorageStatus::InvalidArgument;
        exists = _fs.exists(path);
        return StorageStatus::Success;
    }

    StorageStatus Stat(const char* path, StorageEntry& entry) const override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(path)) return StorageStatus::InvalidArgument;
        fs::File file = _fs.open(path, FILE_READ);
        if (!file) return StorageStatus::NotFound;
        CopyPath(entry.path, path);
        entry.isDirectory = file.isDirectory();
        entry.size = entry.isDirectory ? 0 : file.size();
        file.close();
        return StorageStatus::Success;
    }

    StorageStatus Read(const char* path, uint64_t offset, uint8_t* buffer, std::size_t capacity, std::size_t& bytesRead) const override {
        bytesRead = 0;
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(path) || (buffer == nullptr && capacity != 0)) return StorageStatus::InvalidArgument;
        fs::File file = _fs.open(path, FILE_READ);
        if (!file || file.isDirectory()) return StorageStatus::NotFound;
        if (offset > file.size() || !file.seek(offset)) {
            file.close();
            return offset == file.size() ? StorageStatus::Success : StorageStatus::IoError;
        }
        bytesRead = file.read(buffer, capacity);
        file.close();
        return StorageStatus::Success;
    }

    StorageStatus Write(const char* path, const uint8_t* data, std::size_t size, WriteMode mode = WriteMode::Replace) override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(path) || (data == nullptr && size != 0)) return StorageStatus::InvalidArgument;
        const char* openMode = mode == WriteMode::Append ? FILE_APPEND : FILE_WRITE;
        fs::File file = _fs.open(path, openMode);
        if (!file) return StorageStatus::IoError;
        const std::size_t written = size == 0 ? 0 : file.write(data, size);
        file.flush();
        file.close();
        return written == size ? StorageStatus::Success : StorageStatus::PartialWrite;
    }

    StorageStatus Remove(const char* path) override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(path)) return StorageStatus::InvalidArgument;
        if (!_fs.exists(path)) return StorageStatus::NotFound;
        return _fs.remove(path) ? StorageStatus::Success : StorageStatus::IoError;
    }

    StorageStatus Rename(const char* from, const char* to) override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(from) || !Valid(to)) return StorageStatus::InvalidArgument;
        if (!_fs.exists(from)) return StorageStatus::NotFound;
        if (_fs.exists(to)) return StorageStatus::AlreadyExists;
        return _fs.rename(from, to) ? StorageStatus::Success : StorageStatus::IoError;
    }

    StorageStatus CreateDirectory(const char* path) override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(path)) return StorageStatus::InvalidArgument;
        if (_fs.exists(path)) return StorageStatus::AlreadyExists;
        return _fs.mkdir(path) ? StorageStatus::Success : StorageStatus::IoError;
    }

    StorageStatus RemoveDirectory(const char* path) override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(path)) return StorageStatus::InvalidArgument;
        if (!_fs.exists(path)) return StorageStatus::NotFound;
        return _fs.rmdir(path) ? StorageStatus::Success : StorageStatus::IoError;
    }

    StorageStatus List(const char* path, StorageListCallback callback, void* context = nullptr) const override {
        if (!_ready) return StorageStatus::NotInitialized;
        if (!Valid(path) || callback == nullptr) return StorageStatus::InvalidArgument;
        fs::File directory = _fs.open(path, FILE_READ);
        if (!directory || !directory.isDirectory()) return StorageStatus::NotFound;
        fs::File file = directory.openNextFile();
        while (file) {
            StorageEntry entry{};
            CopyPath(entry.path, file.path());
            entry.isDirectory = file.isDirectory();
            entry.size = entry.isDirectory ? 0 : file.size();
            if (!callback(entry, context)) {
                file.close();
                directory.close();
                return StorageStatus::Success;
            }
            file.close();
            file = directory.openNextFile();
        }
        directory.close();
        return StorageStatus::Success;
    }

protected:
    ESP32FileStorageBase(fs::FS& fs, StorageCapability additionalCapabilities = StorageCapability::None)
        : _fs(fs), _additionalCapabilities(additionalCapabilities) {}

    static bool Valid(const char* value) { return value != nullptr && *value != '\0'; }
    static void CopyPath(char* destination, const char* source) {
        if (source == nullptr) source = "";
        std::strncpy(destination, source, StorageEntry::MaximumPathLength - 1);
        destination[StorageEntry::MaximumPathLength - 1] = '\0';
    }

    fs::FS& _fs;
    bool _ready = false;
    StorageCapability _additionalCapabilities = StorageCapability::None;
};

} // namespace ESPressio::Persistence

#endif
