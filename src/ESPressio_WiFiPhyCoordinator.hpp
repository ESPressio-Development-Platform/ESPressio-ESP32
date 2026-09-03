#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_WiFiPhyCoordinator.hpp requires an ESP32 Arduino target"
#endif

#include <cstdint>
#include <mutex>

#include <esp_err.h>
#include <esp_wifi.h>

#include <ESPressio_Synchronization.hpp>

namespace ESPressio::ESP32Platform {

enum class RawWiFiPhyAccessStatus : uint8_t {
    Available,
    WiFiServiceConflict,
    DriverUnavailable
};

struct RawWiFiPhyAccess {
    RawWiFiPhyAccessStatus Status = RawWiFiPhyAccessStatus::DriverUnavailable;
    uint8_t EffectiveChannel = 0;

    constexpr explicit operator bool() const noexcept {
        return Status == RawWiFiPhyAccessStatus::Available;
    }
};

/// <summary>
/// Platform-level owner/arbitrator for ESP32 hardware-global Wi-Fi PHY settings shared by ordinary Wi-Fi and Raw80211.
/// </summary>
/// <remarks>
/// ESPressio-Radio deliberately has no ESP32 channel semantics. This coordinator exists only in the ESP32 concrete layer.
/// Ordinary Wi-Fi marks itself active while it owns association/AP/scanning behaviour. Raw80211 may set a requested fixed
/// channel only while ordinary Wi-Fi is not active; otherwise it follows the effective Wi-Fi channel or reports a conflict
/// without retuning/disrupting the active Wi-Fi service. Explicit global power-save and transmit-power writes are also
/// centralized here so multiple ESP32 services do not independently fight over shared PHY state.
/// </remarks>
class WiFiPhyCoordinator final {
public:
    WiFiPhyCoordinator(const WiFiPhyCoordinator&) = delete;
    WiFiPhyCoordinator& operator=(const WiFiPhyCoordinator&) = delete;

    void SetWiFiServiceActive(bool active) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        _wifiServiceActive = active;
    }

    bool WiFiServiceActive() const noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        return _wifiServiceActive;
    }

    bool ApplyWiFiPolicy(int8_t txPowerDbm, bool powerSave) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        if (txPowerDbm < 2) txPowerDbm = 2;
        if (txPowerDbm > 20) txPowerDbm = 20;
        if (esp_wifi_set_max_tx_power(static_cast<int8_t>(txPowerDbm * 4)) != ESP_OK) return false;
        return esp_wifi_set_ps(powerSave ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) == ESP_OK;
    }

    RawWiFiPhyAccess ResolveRawAccess(uint8_t requestedChannel, bool applyWhenUnconstrained) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);

        uint8_t currentChannel = 0;
        wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
        const esp_err_t channelResult = esp_wifi_get_channel(&currentChannel, &secondary);

        if (_wifiServiceActive) {
            if (channelResult != ESP_OK || currentChannel == 0) {
                return {RawWiFiPhyAccessStatus::DriverUnavailable, 0};
            }
            if (requestedChannel != 0 && requestedChannel != currentChannel) {
                return {RawWiFiPhyAccessStatus::WiFiServiceConflict, currentChannel};
            }
            return {RawWiFiPhyAccessStatus::Available, currentChannel};
        }

        if (requestedChannel != 0 && applyWhenUnconstrained) {
            if (esp_wifi_set_channel(requestedChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
                return {RawWiFiPhyAccessStatus::DriverUnavailable, currentChannel};
            }
            return {RawWiFiPhyAccessStatus::Available, requestedChannel};
        }

        if (channelResult != ESP_OK || currentChannel == 0) {
            return {RawWiFiPhyAccessStatus::DriverUnavailable, 0};
        }
        return {RawWiFiPhyAccessStatus::Available, currentChannel};
    }

private:
    WiFiPhyCoordinator() = default;

    mutable System::Synchronization::Mutex _mutex;
    bool _wifiServiceActive = false;

    friend WiFiPhyCoordinator& SharedWiFiPhy() noexcept;
};

inline WiFiPhyCoordinator& SharedWiFiPhy() noexcept {
    static WiFiPhyCoordinator instance;
    return instance;
}

} // namespace ESPressio::ESP32Platform
