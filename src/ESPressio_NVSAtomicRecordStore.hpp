#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include <ESPressio_IAtomicRecordStore.hpp>

#include <nvs.h>
#include <nvs_flash.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace ESPressio::Persistence {

/// <summary>
/// Fixed-capacity ESP32 NVS implementation of the ESPressio durable atomic-record contract.
/// </summary>
/// <remarks>
/// Each registered semantic key maps to one deterministic NVS key. The persisted blob is
/// self-framed with the complete semantic key, a monotonic generation, payload length and
/// CRC32 so accidental key-map/configuration drift or a damaged value is never accepted as
/// authoritative OTA/Persistence state. ESP-IDF NVS supplies the underlying power-loss-safe
/// atomic key update; this wrapper treats nvs_commit() as the durability boundary and returns
/// CommitAmbiguous whenever an attempted publication/removal cannot be proven durable.
///
/// The wrapper retains no caller buffer, performs no dynamic allocation itself, and bounds
/// all record work by MaximumRecordBytes and RecordCount. Composition still owns serialized
/// access, as required by IAtomicRecordStore.
/// </remarks>
template<std::size_t MaximumRecordBytes, std::size_t RecordCount>
class NVSAtomicRecordStore final : public IAtomicRecordStore {
    static_assert(MaximumRecordBytes > 0U,
                  "NVS atomic record payload capacity must be positive");
    static_assert(MaximumRecordBytes <= std::numeric_limits<std::uint32_t>::max() - 64U,
                  "NVS atomic record payload capacity must leave room for framing");
    static_assert(RecordCount > 0U,
                  "NVS atomic record store requires at least one registered key");

    static constexpr std::size_t HeaderBytes = 64U;
    static constexpr std::uint16_t FrameVersion = 1U;
    static constexpr std::size_t MaximumNamespaceBytes = 15U;

    const std::array<AtomicRecordKey, RecordCount> keys_;
    std::array<char, MaximumNamespaceBytes + 1U> namespace_{};
    std::array<std::uint8_t, MaximumRecordBytes + HeaderBytes> scratch_{};
    nvs_handle_t handle_{};
    bool valid_{true};
    bool ready_{false};

    static void Put(std::uint8_t* bytes,
                    std::size_t offset,
                    std::uint64_t value,
                    unsigned width) noexcept {
        for (unsigned index = 0U; index < width; ++index) {
            bytes[offset + index] = static_cast<std::uint8_t>(value);
            value >>= 8U;
        }
    }

    static std::uint64_t Get(const std::uint8_t* bytes,
                             std::size_t offset,
                             unsigned width) noexcept {
        std::uint64_t value = 0U;
        for (unsigned index = 0U; index < width; ++index) {
            value |= std::uint64_t(bytes[offset + index]) << (8U * index);
        }
        return value;
    }

    static std::uint32_t CRC32(const std::uint8_t* bytes, std::size_t size) noexcept {
        std::uint32_t crc = 0xffffffffU;
        for (std::size_t index = 0U; index < size; ++index) {
            // Bytes 56..59 hold the CRC itself and are treated as zero while calculating it.
            if (index >= 56U && index < 60U) continue;
            crc ^= bytes[index];
            for (unsigned bit = 0U; bit < 8U; ++bit) {
                crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
            }
        }
        return ~crc;
    }

    static std::uint32_t KeyToken(const AtomicRecordKey& key) noexcept {
        // FNV-1a is used only as a compact NVS namespace token. The complete semantic key is
        // persisted in the frame and constructor-time token collisions are rejected.
        std::uint32_t value = 2166136261U;
        for (std::size_t index = 0U; index < key.Size(); ++index) {
            value ^= key.Data()[index];
            value *= 16777619U;
        }
        return value;
    }

    static void NVSKeyName(const AtomicRecordKey& key, std::array<char, 10U>& output) noexcept {
        static constexpr char Hex[] = "0123456789abcdef";
        const std::uint32_t token = KeyToken(key);
        output[0] = 'r';
        for (unsigned nibble = 0U; nibble < 8U; ++nibble) {
            const unsigned shift = 28U - (nibble * 4U);
            output[1U + nibble] = Hex[(token >> shift) & 0x0fU];
        }
        output[9] = '\0';
    }

    static AtomicRecordStatus MapReadError(esp_err_t error) noexcept {
        switch (error) {
            case ESP_OK: return AtomicRecordStatus::Success;
            case ESP_ERR_NVS_NOT_FOUND: return AtomicRecordStatus::NotFound;
            case ESP_ERR_NVS_INVALID_LENGTH: return AtomicRecordStatus::Corrupt;
            case ESP_ERR_NVS_TYPE_MISMATCH: return AtomicRecordStatus::Corrupt;
            case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return AtomicRecordStatus::NoSpace;
            case ESP_ERR_NVS_INVALID_NAME:
            case ESP_ERR_NVS_KEY_TOO_LONG:
            case ESP_ERR_INVALID_ARG:
                return AtomicRecordStatus::InvalidArgument;
            case ESP_ERR_NVS_INVALID_STATE:
            case ESP_ERR_NVS_NEW_VERSION_FOUND:
                return AtomicRecordStatus::Corrupt;
            default:
                return AtomicRecordStatus::StorageFailure;
        }
    }

    bool Resolve(const AtomicRecordKey& key, std::size_t& index) const noexcept {
        if (!key) return false;
        for (std::size_t candidate = 0U; candidate < RecordCount; ++candidate) {
            if (keys_[candidate] == key) {
                index = candidate;
                return true;
            }
        }
        return false;
    }

    AtomicRecordStatus Load(const AtomicRecordKey& key,
                            std::size_t& payloadBytes,
                            std::uint64_t& generation) noexcept {
        payloadBytes = 0U;
        generation = 0U;
        if (!handle_) return AtomicRecordStatus::StorageFailure;

        std::array<char, 10U> nvsKey{};
        NVSKeyName(key, nvsKey);

        std::size_t storedBytes = scratch_.size();
        const esp_err_t read = nvs_get_blob(handle_, nvsKey.data(), scratch_.data(), &storedBytes);
        if (read != ESP_OK) return MapReadError(read);

        if (storedBytes < HeaderBytes || storedBytes > scratch_.size()) {
            return AtomicRecordStatus::Corrupt;
        }

        const auto* bytes = scratch_.data();
        if (bytes[0] != 'E' || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'V' ||
            Get(bytes, 4U, 2U) != FrameVersion ||
            Get(bytes, 6U, 2U) != HeaderBytes) {
            return AtomicRecordStatus::Corrupt;
        }

        generation = Get(bytes, 8U, 8U);
        payloadBytes = static_cast<std::size_t>(Get(bytes, 20U, 4U));
        if (generation == 0U ||
            bytes[16] != key.Size() ||
            payloadBytes > MaximumRecordBytes ||
            storedBytes != HeaderBytes + payloadBytes) {
            return AtomicRecordStatus::Corrupt;
        }

        for (std::size_t index = 17U; index < 20U; ++index) {
            if (bytes[index] != 0U) return AtomicRecordStatus::Corrupt;
        }
        for (std::size_t index = 60U; index < 64U; ++index) {
            if (bytes[index] != 0U) return AtomicRecordStatus::Corrupt;
        }
        for (std::size_t index = 0U; index < 32U; ++index) {
            const std::uint8_t expected = index < key.Size() ? key.Data()[index] : 0U;
            if (bytes[24U + index] != expected) return AtomicRecordStatus::Corrupt;
        }
        if (Get(bytes, 56U, 4U) != CRC32(bytes, storedBytes)) {
            return AtomicRecordStatus::Corrupt;
        }
        return AtomicRecordStatus::Success;
    }

    void BuildFrame(const AtomicRecordKey& key,
                    std::uint64_t generation,
                    const std::uint8_t* data,
                    std::size_t size) noexcept {
        for (std::size_t index = 0U; index < HeaderBytes; ++index) scratch_[index] = 0U;
        auto* bytes = scratch_.data();
        bytes[0] = 'E';
        bytes[1] = 'P';
        bytes[2] = 'N';
        bytes[3] = 'V';
        Put(bytes, 4U, FrameVersion, 2U);
        Put(bytes, 6U, HeaderBytes, 2U);
        Put(bytes, 8U, generation, 8U);
        bytes[16] = static_cast<std::uint8_t>(key.Size());
        Put(bytes, 20U, size, 4U);
        for (std::size_t index = 0U; index < key.Size(); ++index) {
            bytes[24U + index] = key.Data()[index];
        }
        if (size != 0U) std::memcpy(bytes + HeaderBytes, data, size);
        Put(bytes, 56U, CRC32(bytes, HeaderBytes + size), 4U);
    }

public:
    /// <summary>
    /// Copies the complete finite semantic-key set and NVS namespace. No borrowed constructor
    /// storage is retained. Invalid/duplicate semantic keys, token collisions or an invalid
    /// namespace make Recover fail with InvalidArgument before opening NVS.
    /// </summary>
    NVSAtomicRecordStore(const std::array<AtomicRecordKey, RecordCount>& keys,
                         std::string_view nvsNamespace) noexcept
        : keys_(keys) {
        if (nvsNamespace.empty() || nvsNamespace.size() > MaximumNamespaceBytes) {
            valid_ = false;
        } else {
            for (std::size_t index = 0U; index < nvsNamespace.size(); ++index) {
                if (nvsNamespace[index] == '\0') {
                    valid_ = false;
                    break;
                }
                namespace_[index] = nvsNamespace[index];
            }
        }

        for (std::size_t left = 0U; left < RecordCount; ++left) {
            if (!keys_[left]) valid_ = false;
            for (std::size_t right = 0U; right < left; ++right) {
                if (keys_[left] == keys_[right] || KeyToken(keys_[left]) == KeyToken(keys_[right])) {
                    valid_ = false;
                }
            }
        }
    }

    ~NVSAtomicRecordStore() override {
        if (handle_) nvs_close(handle_);
    }

    NVSAtomicRecordStore(const NVSAtomicRecordStore&) = delete;
    NVSAtomicRecordStore& operator=(const NVSAtomicRecordStore&) = delete;

    AtomicRecordCapabilities Capabilities() const noexcept override {
        return {true, true, MaximumRecordBytes, RecordCount};
    }

    AtomicRecordStatus Recover() noexcept override {
        ready_ = false;
        if (!valid_) return AtomicRecordStatus::InvalidArgument;

        if (handle_) {
            nvs_close(handle_);
            handle_ = 0;
        }

        const esp_err_t initialize = nvs_flash_init();
        if (initialize == ESP_ERR_NVS_NO_FREE_PAGES ||
            initialize == ESP_ERR_NVS_NEW_VERSION_FOUND ||
            initialize == ESP_ERR_NVS_INVALID_STATE) {
            return AtomicRecordStatus::Corrupt;
        }
        if (initialize != ESP_OK) return AtomicRecordStatus::StorageFailure;

        const esp_err_t open = nvs_open(namespace_.data(), NVS_READWRITE, &handle_);
        if (open != ESP_OK) {
            handle_ = 0;
            return MapReadError(open);
        }

        for (const auto& key : keys_) {
            std::size_t payload = 0U;
            std::uint64_t generation = 0U;
            const auto status = Load(key, payload, generation);
            if (status != AtomicRecordStatus::Success && status != AtomicRecordStatus::NotFound) {
                nvs_close(handle_);
                handle_ = 0;
                return status;
            }
        }

        ready_ = true;
        return AtomicRecordStatus::Success;
    }

    AtomicRecordStatus Read(const AtomicRecordKey& key,
                            std::uint8_t* buffer,
                            std::size_t capacity,
                            std::size_t& bytesRead) noexcept override {
        bytesRead = 0U;
        if (!ready_ || !handle_) return AtomicRecordStatus::StorageFailure;
        if (buffer == nullptr && capacity != 0U) return AtomicRecordStatus::InvalidArgument;

        std::size_t keyIndex = 0U;
        if (!Resolve(key, keyIndex)) return AtomicRecordStatus::InvalidArgument;
        (void)keyIndex;

        std::size_t payload = 0U;
        std::uint64_t generation = 0U;
        const auto status = Load(key, payload, generation);
        if (status != AtomicRecordStatus::Success) return status;
        if (payload > capacity) return AtomicRecordStatus::BufferTooSmall;

        if (payload != 0U) std::memcpy(buffer, scratch_.data() + HeaderBytes, payload);
        bytesRead = payload;
        return AtomicRecordStatus::Success;
    }

    AtomicRecordStatus ReplaceAtomically(const AtomicRecordKey& key,
                                         const std::uint8_t* data,
                                         std::size_t size) noexcept override {
        if (!ready_ || !handle_) return AtomicRecordStatus::StorageFailure;
        if ((data == nullptr && size != 0U) || size > MaximumRecordBytes) {
            return AtomicRecordStatus::InvalidArgument;
        }

        std::size_t keyIndex = 0U;
        if (!Resolve(key, keyIndex)) return AtomicRecordStatus::InvalidArgument;
        (void)keyIndex;

        std::size_t existingPayload = 0U;
        std::uint64_t generation = 0U;
        const auto existing = Load(key, existingPayload, generation);
        if (existing != AtomicRecordStatus::Success && existing != AtomicRecordStatus::NotFound) {
            return existing;
        }
        if (existing == AtomicRecordStatus::NotFound) generation = 0U;
        if (generation == std::numeric_limits<std::uint64_t>::max()) {
            return AtomicRecordStatus::NoSpace;
        }

        const std::uint64_t nextGeneration = generation + 1U;
        BuildFrame(key, nextGeneration, data, size);

        std::array<char, 10U> nvsKey{};
        NVSKeyName(key, nvsKey);
        const esp_err_t set = nvs_set_blob(handle_, nvsKey.data(), scratch_.data(), HeaderBytes + size);
        if (set != ESP_OK) {
            if (set == ESP_ERR_NVS_NOT_ENOUGH_SPACE) return AtomicRecordStatus::NoSpace;
#ifdef ESP_ERR_NVS_REMOVE_FAILED
            if (set == ESP_ERR_NVS_REMOVE_FAILED) return AtomicRecordStatus::CommitAmbiguous;
#endif
            return MapReadError(set);
        }

        // From the first durability attempt onward, any error is ambiguous: ESP-IDF may have
        // reached flash even when the caller did not observe a successful return.
        if (nvs_commit(handle_) != ESP_OK) return AtomicRecordStatus::CommitAmbiguous;

        std::size_t verifiedPayload = 0U;
        std::uint64_t verifiedGeneration = 0U;
        const auto verify = Load(key, verifiedPayload, verifiedGeneration);
        if (verify != AtomicRecordStatus::Success ||
            verifiedPayload != size ||
            verifiedGeneration != nextGeneration) {
            return AtomicRecordStatus::CommitAmbiguous;
        }
        return AtomicRecordStatus::Success;
    }

    AtomicRecordStatus RemoveAfterCommit(const AtomicRecordKey& key) noexcept override {
        if (!ready_ || !handle_) return AtomicRecordStatus::StorageFailure;

        std::size_t keyIndex = 0U;
        if (!Resolve(key, keyIndex)) return AtomicRecordStatus::InvalidArgument;
        (void)keyIndex;

        std::array<char, 10U> nvsKey{};
        NVSKeyName(key, nvsKey);
        const esp_err_t erase = nvs_erase_key(handle_, nvsKey.data());
        if (erase == ESP_ERR_NVS_NOT_FOUND) return AtomicRecordStatus::Success;
        if (erase != ESP_OK) return MapReadError(erase);
        if (nvs_commit(handle_) != ESP_OK) return AtomicRecordStatus::CommitAmbiguous;

        std::size_t remaining = 0U;
        const esp_err_t probe = nvs_get_blob(handle_, nvsKey.data(), nullptr, &remaining);
        return probe == ESP_ERR_NVS_NOT_FOUND
            ? AtomicRecordStatus::Success
            : AtomicRecordStatus::CommitAmbiguous;
    }
};

} // namespace ESPressio::Persistence

#endif // ARDUINO_ARCH_ESP32
