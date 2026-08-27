#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <ESPressio_Memory.hpp>

namespace ESPressio::ESP32Platform {

class ESP32MemoryProvider final : public System::Memory::IMemoryProvider {
public:
    void* Allocate(std::size_t bytes, std::size_t alignment, System::Memory::MemoryPolicy policy) override {
        if (bytes == 0) bytes = 1;
        switch (policy) {
            case System::Memory::MemoryPolicy::Internal:
                return AllocateWithCaps(bytes, alignment, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            case System::Memory::MemoryPolicy::ExternalRequired:
                return AllocateWithCaps(bytes, alignment, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            case System::Memory::MemoryPolicy::ExternalPreferred: {
                void* result = TryAllocateWithCaps(bytes, alignment, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (result) return result;
                return AllocateWithCaps(bytes, alignment, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            case System::Memory::MemoryPolicy::Automatic:
            default:
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

private:
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
};

inline ESP32MemoryProvider& MemoryProvider() {
    static ESP32MemoryProvider provider;
    return provider;
}

inline System::Memory::IMemoryProvider* InstallMemoryProvider() noexcept {
    return System::Memory::SetProvider(&MemoryProvider());
}

} // namespace ESPressio::ESP32Platform
