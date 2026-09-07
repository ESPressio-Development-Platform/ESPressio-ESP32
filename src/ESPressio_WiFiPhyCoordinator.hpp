#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_WiFiPhyCoordinator.hpp requires an ESP32 Arduino target"
#endif

#include <atomic>
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
/// Ordinary Wi-Fi publishes lifecycle/channel transitions here. A Raw80211 provider registers its fixed-channel request
/// once at start and subsequently reads an atomic cached access snapshot on packet hot paths.
///
/// Native driver queries and channel/power-policy writes are therefore lifecycle work, never recurring Radio-worker
/// polling. In particular, DrainInbound/DrainControlInbound and Send must not call esp_wifi_get_channel(). This prevents
/// a high-priority precision worker from repeatedly contending with the same Wi-Fi driver task that delivers its RX
/// timestamp callback.
/// </remarks>
class WiFiPhyCoordinator final {
public:
    WiFiPhyCoordinator(const WiFiPhyCoordinator&) = delete;
    WiFiPhyCoordinator& operator=(const WiFiPhyCoordinator&) = delete;

    /// <summary>Publishes whether ordinary ESPressio Wi-Fi currently owns association/AP/scanning PHY behaviour.</summary>
    void SetWiFiServiceActive(bool active) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        if (_wifiServiceActive == active) return;
        _wifiServiceActive = active;
        if (_rawRegistered) {
            // This is a lifecycle transition, so one native channel query/application is acceptable here.
            (void)RefreshRawAccessLocked(!active);
        }
    }

    bool WiFiServiceActive() const noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        return _wifiServiceActive;
    }

    /// <summary>
    /// Publishes an effective ordinary-Wi-Fi channel already learned by WiFiPlatform during normal state refresh.
    /// No native driver query is performed here.
    /// </summary>
    void NotifyWiFiEffectiveChannel(uint8_t channel) noexcept {
        if (channel == 0U) return;
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        _lastKnownChannel = channel;
        if (!_rawRegistered || !_wifiServiceActive) return;
        PublishRawAccessLocked(
            _rawRequestedChannel != 0U && _rawRequestedChannel != channel
                ? RawWiFiPhyAccessStatus::WiFiServiceConflict
                : RawWiFiPhyAccessStatus::Available,
            channel);
    }

    bool ApplyWiFiPolicy(int8_t txPowerDbm, bool powerSave) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        if (txPowerDbm < 2) txPowerDbm = 2;
        if (txPowerDbm > 20) txPowerDbm = 20;
        if (esp_wifi_set_max_tx_power(static_cast<int8_t>(txPowerDbm * 4)) != ESP_OK) return false;
        // A registered precision Raw80211 consumer owns the stronger no-modem-sleep requirement.
        const bool effectivePowerSave = powerSave && !_precisionReceiveTimestampsRequired;
        return esp_wifi_set_ps(effectivePowerSave ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) == ESP_OK;
    }

    /// <summary>
    /// Disables modem sleep while any registered provider advertises precision Wi-Fi receive timestamps.
    /// This is lifecycle work and must not be called per packet/worker iteration.
    /// </summary>
    bool RequirePrecisionReceiveTimestamps() noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        _precisionReceiveTimestampsRequired = true;
        return esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    }

    /// <summary>
    /// Registers one Raw80211 provider and resolves/applies its initial channel policy exactly once for this lifecycle.
    /// </summary>
    RawWiFiPhyAccess RegisterRawAccess(
        uint8_t requestedChannel,
        bool requirePrecisionReceiveTimestamps = true
    ) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        if (_rawRegistered && _rawRequestedChannel != requestedChannel) {
            PublishRawAccessLocked(RawWiFiPhyAccessStatus::WiFiServiceConflict, _lastKnownChannel);
            return CachedRawAccess();
        }
        _rawRegistered = true;
        _rawRequestedChannel = requestedChannel;
        if (requirePrecisionReceiveTimestamps) {
            _precisionReceiveTimestampsRequired = true;
            if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, _lastKnownChannel);
                return CachedRawAccess();
            }
        }
        (void)RefreshRawAccessLocked(!_wifiServiceActive);
        return CachedRawAccess();
    }

    /// <summary>Releases the Raw80211 lifecycle registration and invalidates the hot-path cached access snapshot.</summary>
    void ReleaseRawAccess() noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        _rawRegistered = false;
        _rawRequestedChannel = 0U;
        _precisionReceiveTimestampsRequired = false;
        PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, _lastKnownChannel);
    }

    /// <summary>Returns the lock-free cached Raw80211 PHY access snapshot. No ESP-IDF call occurs.</summary>
    RawWiFiPhyAccess CachedRawAccess() const noexcept {
        return Decode(_cachedRawAccess.load(std::memory_order_acquire));
    }

    /// <summary>
    /// Compatibility resolver. applyWhenUnconstrained=true is treated as an explicit lifecycle refresh; false returns
    /// cached state only and never interrogates the driver.
    /// </summary>
    RawWiFiPhyAccess ResolveRawAccess(uint8_t requestedChannel, bool applyWhenUnconstrained) noexcept {
        if (!applyWhenUnconstrained) {
            const auto cached = CachedRawAccess();
            if (requestedChannel == 0U || cached.EffectiveChannel == 0U ||
                requestedChannel == cached.EffectiveChannel) return cached;
            return {RawWiFiPhyAccessStatus::WiFiServiceConflict, cached.EffectiveChannel};
        }
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        _rawRegistered = true;
        _rawRequestedChannel = requestedChannel;
        (void)RefreshRawAccessLocked(!_wifiServiceActive);
        return CachedRawAccess();
    }

private:
    WiFiPhyCoordinator() = default;

    static constexpr std::uint32_t Encode(RawWiFiPhyAccessStatus status, uint8_t channel) noexcept {
        return (static_cast<std::uint32_t>(status) << 8U) |
               static_cast<std::uint32_t>(channel);
    }

    static constexpr RawWiFiPhyAccess Decode(std::uint32_t encoded) noexcept {
        return {
            static_cast<RawWiFiPhyAccessStatus>((encoded >> 8U) & 0xFFU),
            static_cast<uint8_t>(encoded & 0xFFU)
        };
    }

    void PublishRawAccessLocked(RawWiFiPhyAccessStatus status, uint8_t channel) noexcept {
        _cachedRawAccess.store(Encode(status, channel), std::memory_order_release);
    }

    RawWiFiPhyAccess RefreshRawAccessLocked(bool applyWhenUnconstrained) noexcept {
        if (!_rawRegistered) {
            PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, _lastKnownChannel);
            return Decode(_cachedRawAccess.load(std::memory_order_relaxed));
        }

        uint8_t currentChannel = _lastKnownChannel;
        wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;

        if (_wifiServiceActive) {
            if (currentChannel == 0U && esp_wifi_get_channel(&currentChannel, &secondary) != ESP_OK) {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, 0U);
                return Decode(_cachedRawAccess.load(std::memory_order_relaxed));
            }
            _lastKnownChannel = currentChannel;
            if (_rawRequestedChannel != 0U && _rawRequestedChannel != currentChannel) {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::WiFiServiceConflict, currentChannel);
            } else {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::Available, currentChannel);
            }
            return Decode(_cachedRawAccess.load(std::memory_order_relaxed));
        }

        if (_rawRequestedChannel != 0U && applyWhenUnconstrained) {
            if (esp_wifi_set_channel(_rawRequestedChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, currentChannel);
                return Decode(_cachedRawAccess.load(std::memory_order_relaxed));
            }
            _lastKnownChannel = _rawRequestedChannel;
            PublishRawAccessLocked(RawWiFiPhyAccessStatus::Available, _rawRequestedChannel);
            return Decode(_cachedRawAccess.load(std::memory_order_relaxed));
        }

        if (currentChannel == 0U) {
            if (esp_wifi_get_channel(&currentChannel, &secondary) != ESP_OK || currentChannel == 0U) {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, 0U);
                return Decode(_cachedRawAccess.load(std::memory_order_relaxed));
            }
            _lastKnownChannel = currentChannel;
        }
        PublishRawAccessLocked(RawWiFiPhyAccessStatus::Available, currentChannel);
        return Decode(_cachedRawAccess.load(std::memory_order_relaxed));
    }

    mutable System::Synchronization::Mutex _mutex;
    bool _wifiServiceActive = false;
    bool _rawRegistered = false;
    bool _precisionReceiveTimestampsRequired = false;
    uint8_t _rawRequestedChannel = 0U;
    uint8_t _lastKnownChannel = 0U;
    std::atomic<std::uint32_t> _cachedRawAccess{
        Encode(RawWiFiPhyAccessStatus::DriverUnavailable, 0U)
    };

    friend WiFiPhyCoordinator& SharedWiFiPhy() noexcept;
};

inline WiFiPhyCoordinator& SharedWiFiPhy() noexcept {
    static WiFiPhyCoordinator instance;
    return instance;
}

} // namespace ESPressio::ESP32Platform
