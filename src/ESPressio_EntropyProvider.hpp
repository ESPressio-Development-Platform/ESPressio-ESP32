#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_system.h>

#include <ESPressio_SystemPlatformEntropy.hpp>

namespace ESPressio::ESP32Platform {

class EntropySource final : public System::Entropy::IEntropySource {
public:
    System::PlatformResult Fill(void* output, std::size_t size) noexcept override {
        if (output == nullptr && size != 0) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        if (size != 0) {
            esp_fill_random(output, size);
        }
        return System::PlatformResult::Succeeded();
    }

    bool IsCryptographicallySuitable() const noexcept override {
        return true;
    }
};

inline EntropySource& GetEntropySource() noexcept {
    static EntropySource source;
    return source;
}

inline void InstallEntropySource() noexcept {
    System::Entropy::SetSource(&GetEntropySource());
}

} // namespace ESPressio::ESP32Platform
