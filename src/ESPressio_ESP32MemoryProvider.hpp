#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <ESPressio_Memory.hpp>

namespace ESPressio::ESP32Platform {

struct MemoryProviderStatistics {
    uint32_t AutomaticRequests{0};
    uint32_t AutomaticBytes{0};
    uint32_t InternalRequests{0};
    uint32_t InternalBytes{0};
    uint32_t ExternalRequiredRequests{0};
    uint32_t ExternalRequiredBytes{0};
    uint32_t ExternalPreferredRequests{0};
    uint32_t ExternalPreferredBytes{0};
    uint32_t ExternalPreferredExternalSuccesses{0};
    uint32_t ExternalPreferredExternalBytes{0};
    uint32_t ExternalPreferredInternalFallbacks{0};
    uint32_t ExternalPreferredInternalFallbackBytes{0};
};

class ESP32MemoryProvider final : public System::Memory::IMemoryProvider {
public:
    void* Allocate(std::size_t bytes, std::size_t alignment, System::Memory::MemoryPolicy policy) override {
        if (bytes == 0) bytes = 1;
        const uint32_t recordedBytes = ClampToUint32(bytes);

        switch (policy) {
            case System::Memory::MemoryPolicy::Internal:
                _internalRequests.fetch_add(1, std::memory_order_relaxed);
                _internalBytes.fetch_add(recordedBytes, std::memory_order_relaxed);
                return AllocateWithCaps(bytes, alignment, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

            case System::Memory::MemoryPolicy::ExternalRequired:
                _externalRequiredRequests.fetch_add(1, std::memory_order_relaxed);
                _externalRequiredBytes.fetch_add(recordedBytes, std::memory_order_relaxed);
                return AllocateWithCaps(bytes, alignment, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

            case System::Memory::MemoryPolicy::ExternalPreferred: {
                _externalPreferredRequests.fetch_add(1, std::memory_order_relaxed);
                _externalPreferredBytes.fetch_add(recordedBytes, std::memory_order_relaxed);

                void* result = TryAllocateWithCaps(
                    bytes,
                    alignment,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                );
                if (result != nullptr) {
                    _externalPreferredExternalSuccesses.fetch_add(1, std::memory_order_relaxed);
                    _externalPreferredExternalBytes.fetch_add(recordedBytes, std::memory_order_relaxed);
                    return result;
                }

                _externalPreferredInternalFallbacks.fetch_add(1, std::memory_order_relaxed);
                _externalPreferredInternalFallbackBytes.fetch_add(recordedBytes, std::memory_order_relaxed);
                return AllocateWithCaps(bytes, alignment, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }

            case System::Memory::MemoryPolicy::Automatic:
            default:
                _automaticRequests.fetch_add(1, std::memory_order_relaxed);
                _automaticBytes.fetch_add(recordedBytes, std::memory_order_relaxed);
                return AllocateWithCaps(bytes, alignment, MALLOC_CAP_8BIT);
        }
    }

    void Deallocate(void* pointer, std::size_t, std::size_t, System::Memory::MemoryPolicy) noexcept override {
        if (pointer) heap_caps_free(pointer);
    }

    bool Supports(System::Memory::MemoryPolicy policy) const noexcept override {
        if (policy == System::Memory::MemoryPolicy::ExternalRequired ||
            policy == System::Memory::MemoryPolicy::ExternalPreferred) {
            return heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != 0;
        }
        return true;
    }

    MemoryProviderStatistics Statistics() const noexcept {
        MemoryProviderStatistics statistics;
        statistics.AutomaticRequests = _automaticRequests.load(std::memory_order_relaxed);
        statistics.AutomaticBytes = _automaticBytes.load(std::memory_order_relaxed);
        statistics.InternalRequests = _internalRequests.load(std::memory_order_relaxed);
        statistics.InternalBytes = _internalBytes.load(std::memory_order_relaxed);
        statistics.ExternalRequiredRequests = _externalRequiredRequests.load(std::memory_order_relaxed);
        statistics.ExternalRequiredBytes = _externalRequiredBytes.load(std::memory_order_relaxed);
        statistics.ExternalPreferredRequests = _externalPreferredRequests.load(std::memory_order_relaxed);
        statistics.ExternalPreferredBytes = _externalPreferredBytes.load(std::memory_order_relaxed);
        statistics.ExternalPreferredExternalSuccesses =
            _externalPreferredExternalSuccesses.load(std::memory_order_relaxed);
        statistics.ExternalPreferredExternalBytes =
            _externalPreferredExternalBytes.load(std::memory_order_relaxed);
        statistics.ExternalPreferredInternalFallbacks =
            _externalPreferredInternalFallbacks.load(std::memory_order_relaxed);
        statistics.ExternalPreferredInternalFallbackBytes =
            _externalPreferredInternalFallbackBytes.load(std::memory_order_relaxed);
        return statistics;
    }

private:
    static uint32_t ClampToUint32(std::size_t value) noexcept {
        return value > static_cast<std::size_t>(UINT32_MAX)
            ? UINT32_MAX
            : static_cast<uint32_t>(value);
    }

    static void* TryAllocateWithCaps(std::size_t bytes, std::size_t alignment, uint32_t caps) noexcept {
#if ESP_IDF_VERSION_MAJOR >= 4
        return heap_caps_aligned_alloc(
            alignment < sizeof(void*) ? sizeof(void*) : alignment,
            bytes,
            caps
        );
#else
        (void)alignment;
        return heap_caps_malloc(bytes, caps);
#endif
    }

    static void* AllocateWithCaps(std::size_t bytes, std::size_t alignment, uint32_t caps) {
        void* result = TryAllocateWithCaps(bytes, alignment, caps);
        if (!result) throw std::bad_alloc();
        return result;
    }

    std::atomic<uint32_t> _automaticRequests{0};
    std::atomic<uint32_t> _automaticBytes{0};
    std::atomic<uint32_t> _internalRequests{0};
    std::atomic<uint32_t> _internalBytes{0};
    std::atomic<uint32_t> _externalRequiredRequests{0};
    std::atomic<uint32_t> _externalRequiredBytes{0};
    std::atomic<uint32_t> _externalPreferredRequests{0};
    std::atomic<uint32_t> _externalPreferredBytes{0};
    std::atomic<uint32_t> _externalPreferredExternalSuccesses{0};
    std::atomic<uint32_t> _externalPreferredExternalBytes{0};
    std::atomic<uint32_t> _externalPreferredInternalFallbacks{0};
    std::atomic<uint32_t> _externalPreferredInternalFallbackBytes{0};
};

inline ESP32MemoryProvider& MemoryProvider() {
    static ESP32MemoryProvider provider;
    return provider;
}

inline System::Memory::IMemoryProvider* InstallMemoryProvider() noexcept {
    return System::Memory::SetProvider(&MemoryProvider());
}

} // namespace ESPressio::ESP32Platform
