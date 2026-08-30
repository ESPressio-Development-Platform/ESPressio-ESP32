#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <ESPressio_Memory.hpp>

#ifndef ESPRESSIO_ESP32_AUTOMATIC_EXTERNAL_PREFERENCE_ON_INSTALL
#define ESPRESSIO_ESP32_AUTOMATIC_EXTERNAL_PREFERENCE_ON_INSTALL 1
#endif

#ifndef ESPRESSIO_ESP32_AUTOMATIC_EXTERNAL_PREFERENCE_THRESHOLD_BYTES
#define ESPRESSIO_ESP32_AUTOMATIC_EXTERNAL_PREFERENCE_THRESHOLD_BYTES 0U
#endif

namespace ESPressio::ESP32Platform {

/// <summary>Snapshot of allocation traffic and active automatic-allocation placement policy.</summary>
struct MemoryProviderStatistics {
    uint32_t AutomaticRequests{0};
    uint32_t AutomaticBytes{0};
    uint32_t AutomaticExternalSuccesses{0};
    uint32_t AutomaticExternalBytes{0};
    uint32_t AutomaticExternalFallbacks{0};
    uint32_t AutomaticExternalFallbackBytes{0};
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
    uint32_t AutomaticExternalPreferenceThresholdBytes{0};
    bool AutomaticExternalPreferenceEnabled{false};
};

/// <summary>ESP32 implementation of ESPressio System policy-aware memory allocation.</summary>
class MemoryProvider final : public System::Memory::IMemoryProvider {
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
            default: {
                _automaticRequests.fetch_add(1, std::memory_order_relaxed);
                _automaticBytes.fetch_add(recordedBytes, std::memory_order_relaxed);

                const bool preferExternal =
                    _automaticExternalPreferenceEnabled.load(std::memory_order_acquire) &&
                    bytes >= static_cast<std::size_t>(
                        _automaticExternalPreferenceThresholdBytes.load(std::memory_order_relaxed)
                    );
                if (preferExternal) {
                    void* result = TryAllocateWithCaps(
                        bytes,
                        alignment,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                    );
                    if (result != nullptr) {
                        _automaticExternalSuccesses.fetch_add(1, std::memory_order_relaxed);
                        _automaticExternalBytes.fetch_add(recordedBytes, std::memory_order_relaxed);
                        return result;
                    }
                    _automaticExternalFallbacks.fetch_add(1, std::memory_order_relaxed);
                    _automaticExternalFallbackBytes.fetch_add(recordedBytes, std::memory_order_relaxed);
                }
                return AllocateWithCaps(bytes, alignment, MALLOC_CAP_8BIT);
            }
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

    /// <summary>Configures ESP-IDF default <c>malloc()</c> placement so allocations at or above the threshold may use PSRAM.</summary>
    /// <param name="minimumBytes">Smallest ordinary allocation that may prefer PSRAM; zero permits all eligible sizes.</param>
    /// <returns><c>true</c> when ESP-IDF external-memory malloc integration is available and PSRAM exists.</returns>
    /// <remarks>
    /// Capability-constrained allocations such as <c>MALLOC_CAP_INTERNAL</c> or <c>MALLOC_CAP_DMA</c> are not redirected
    /// by this preference. Explicit ESPressio <c>Internal</c>, <c>ExternalPreferred</c>, and <c>ExternalRequired</c>
    /// allocations continue to use their requested capability masks directly. ESPressio <c>Automatic</c> allocations
    /// follow the same threshold and explicitly try PSRAM first before falling back to the ordinary 8-bit heap.
    /// The ESPressio ESP32 installer defaults the threshold to zero so every eligible automatic/default allocation prefers
    /// PSRAM, preserving internal/DMA-capable DRAM for WiFi, TCP/IP, HTTP, I2S and other capability-constrained consumers.
    /// Applications with a workload-specific reason to retain small automatic allocations internally may override
    /// <c>ESPRESSIO_ESP32_AUTOMATIC_EXTERNAL_PREFERENCE_THRESHOLD_BYTES</c> at build time.
    /// </remarks>
    bool ConfigureAutomaticExternalPreference(std::size_t minimumBytes) noexcept override {
#if defined(CONFIG_SPIRAM_USE_MALLOC) && CONFIG_SPIRAM_USE_MALLOC
        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 0) {
            return false;
        }
        heap_caps_malloc_extmem_enable(minimumBytes);
        _automaticExternalPreferenceThresholdBytes.store(
            ClampToUint32(minimumBytes),
            std::memory_order_relaxed
        );
        _automaticExternalPreferenceEnabled.store(true, std::memory_order_release);
        return true;
#else
        (void)minimumBytes;
        return false;
#endif
    }

    /// <summary>Returns cumulative allocator statistics and the configured automatic-allocation threshold.</summary>
    MemoryProviderStatistics Statistics() const noexcept {
        MemoryProviderStatistics statistics;
        statistics.AutomaticRequests = _automaticRequests.load(std::memory_order_relaxed);
        statistics.AutomaticBytes = _automaticBytes.load(std::memory_order_relaxed);
        statistics.AutomaticExternalSuccesses = _automaticExternalSuccesses.load(std::memory_order_relaxed);
        statistics.AutomaticExternalBytes = _automaticExternalBytes.load(std::memory_order_relaxed);
        statistics.AutomaticExternalFallbacks = _automaticExternalFallbacks.load(std::memory_order_relaxed);
        statistics.AutomaticExternalFallbackBytes = _automaticExternalFallbackBytes.load(std::memory_order_relaxed);
        statistics.InternalRequests = _internalRequests.load(std::memory_order_relaxed);
        statistics.InternalBytes = _internalBytes.load(std::memory_order_relaxed);
        statistics.ExternalRequiredRequests = _externalRequiredRequests.load(std::memory_order_relaxed);
        statistics.ExternalRequiredBytes = _externalRequiredBytes.load(std::memory_order_relaxed);
        statistics.ExternalPreferredRequests = _externalPreferredRequests.load(std::memory_order_relaxed);
        statistics.ExternalPreferredBytes = _externalPreferredBytes.load(std::memory_order_relaxed);
        statistics.ExternalPreferredExternalSuccesses = _externalPreferredExternalSuccesses.load(std::memory_order_relaxed);
        statistics.ExternalPreferredExternalBytes = _externalPreferredExternalBytes.load(std::memory_order_relaxed);
        statistics.ExternalPreferredInternalFallbacks = _externalPreferredInternalFallbacks.load(std::memory_order_relaxed);
        statistics.ExternalPreferredInternalFallbackBytes = _externalPreferredInternalFallbackBytes.load(std::memory_order_relaxed);
        statistics.AutomaticExternalPreferenceThresholdBytes =
            _automaticExternalPreferenceThresholdBytes.load(std::memory_order_relaxed);
        statistics.AutomaticExternalPreferenceEnabled =
            _automaticExternalPreferenceEnabled.load(std::memory_order_acquire);
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
    std::atomic<uint32_t> _automaticExternalSuccesses{0};
    std::atomic<uint32_t> _automaticExternalBytes{0};
    std::atomic<uint32_t> _automaticExternalFallbacks{0};
    std::atomic<uint32_t> _automaticExternalFallbackBytes{0};
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
    std::atomic<uint32_t> _automaticExternalPreferenceThresholdBytes{0};
    std::atomic<bool> _automaticExternalPreferenceEnabled{false};
};

/// <summary>Returns the process-wide ESP32 System memory provider.</summary>
inline MemoryProvider& GetMemoryProvider() {
    static MemoryProvider provider;
    return provider;
}

/// <summary>Installs the ESP32 System memory provider, enables aggressive PSRAM preference for every eligible ordinary allocation by default, and returns the previously installed provider.</summary>
inline System::Memory::IMemoryProvider* InstallMemoryProvider() noexcept {
    auto& provider = GetMemoryProvider();
    System::Memory::IMemoryProvider* previous =
        System::Memory::SetProvider(&provider);
#if ESPRESSIO_ESP32_AUTOMATIC_EXTERNAL_PREFERENCE_ON_INSTALL
    (void)provider.ConfigureAutomaticExternalPreference(
        static_cast<std::size_t>(
            ESPRESSIO_ESP32_AUTOMATIC_EXTERNAL_PREFERENCE_THRESHOLD_BYTES
        )
    );
#endif
    return previous;
}

} // namespace ESPressio::ESP32Platform
