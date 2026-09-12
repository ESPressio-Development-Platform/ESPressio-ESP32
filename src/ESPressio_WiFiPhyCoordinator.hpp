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

/// <summary>Stable Radio contention-domain value for providers using the hardware-global ESP32 Wi-Fi PHY.</summary>
inline constexpr std::uint32_t ESP32WiFiRadioContentionDomain = 0x45535746U; // "ESWF"

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
    constexpr bool operator==(const RawWiFiPhyAccess& other) const noexcept {
        return Status == other.Status && EffectiveChannel == other.EffectiveChannel;
    }
    constexpr bool operator!=(const RawWiFiPhyAccess& other) const noexcept { return !(*this == other); }
};

/// <summary>Fixed non-owning lifecycle observation used only by the concrete Raw80211 provider.</summary>
class IRawWiFiPhyAccessObserver {
public:
    virtual ~IRawWiFiPhyAccessObserver() = default;
    virtual void RawWiFiPhyAccessChanged(const RawWiFiPhyAccess& access) noexcept = 0;
};

/// <summary>
/// Platform-level owner/arbitrator for ESP32 hardware-global Wi-Fi PHY settings shared by ordinary Wi-Fi and Raw80211.
/// </summary>
/// <remarks>
/// ESPressio-Radio has no ESP32 channel semantics. Ordinary Wi-Fi publishes lifecycle/channel transitions here while
/// Raw80211 registers one fixed request and consumes a lock-free cached readiness snapshot on packet hot paths. Native
/// driver queries and policy writes are lifecycle work only. The optional observer is a fixed infrastructure callback;
/// it exists solely to wake the Radio domain when cached readiness changes and never invokes application/family code.
/// </remarks>
class WiFiPhyCoordinator final {
public:
    WiFiPhyCoordinator(const WiFiPhyCoordinator&) = delete;
    WiFiPhyCoordinator& operator=(const WiFiPhyCoordinator&) = delete;

    void SetRawAccessObserver(IRawWiFiPhyAccessObserver* observer) noexcept {
        _rawObserver.store(observer, std::memory_order_release);
    }

    void SetWiFiServiceActive(bool active) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        if (_wifiServiceActive == active) return;
        _wifiServiceActive = active;
        if (_rawRegistered) (void)RefreshRawAccessLocked(!active);
    }

    bool WiFiServiceActive() const noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        return _wifiServiceActive;
    }

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
        const bool effectivePowerSave = powerSave && !_precisionReceiveTimestampsRequired;
        return esp_wifi_set_ps(effectivePowerSave ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) == ESP_OK;
    }

    bool RequirePrecisionReceiveTimestamps() noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        _precisionReceiveTimestampsRequired = true;
        return esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    }

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
        _precisionReceiveTimestampsRequired = requirePrecisionReceiveTimestamps;

        if (requirePrecisionReceiveTimestamps && esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
            RollbackRawRegistrationLocked();
            return CachedRawAccess();
        }

        const auto access = RefreshRawAccessLocked(!_wifiServiceActive);
        if (!access) RollbackRawRegistrationLocked();
        return access;
    }

    void ReleaseRawAccess() noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        RollbackRawRegistrationLocked();
    }

    RawWiFiPhyAccess CachedRawAccess() const noexcept {
        return Decode(_cachedRawAccess.load(std::memory_order_acquire));
    }

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
        const auto access = RefreshRawAccessLocked(!_wifiServiceActive);
        if (!access) RollbackRawRegistrationLocked();
        return access;
    }

private:
    WiFiPhyCoordinator() = default;

    static constexpr std::uint32_t Encode(RawWiFiPhyAccessStatus status, uint8_t channel) noexcept {
        return (static_cast<std::uint32_t>(status) << 8U) | static_cast<std::uint32_t>(channel);
    }
    static constexpr RawWiFiPhyAccess Decode(std::uint32_t encoded) noexcept {
        return {static_cast<RawWiFiPhyAccessStatus>((encoded >> 8U) & 0xFFU),
                static_cast<uint8_t>(encoded & 0xFFU)};
    }

    void PublishRawAccessLocked(RawWiFiPhyAccessStatus status, uint8_t channel) noexcept {
        const auto encoded = Encode(status, channel);
        const auto previous = _cachedRawAccess.exchange(encoded, std::memory_order_acq_rel);
        if (previous == encoded) return;
        if (auto* observer = _rawObserver.load(std::memory_order_acquire)) {
            observer->RawWiFiPhyAccessChanged(Decode(encoded));
        }
    }

    void RollbackRawRegistrationLocked() noexcept {
        _rawRegistered = false;
        _rawRequestedChannel = 0U;
        _precisionReceiveTimestampsRequired = false;
        PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, _lastKnownChannel);
    }

    RawWiFiPhyAccess RefreshRawAccessLocked(bool applyWhenUnconstrained) noexcept {
        if (!_rawRegistered) {
            PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, _lastKnownChannel);
            return CachedRawAccess();
        }

        uint8_t currentChannel = _lastKnownChannel;
        wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
        if (_wifiServiceActive) {
            if (currentChannel == 0U && esp_wifi_get_channel(&currentChannel, &secondary) != ESP_OK) {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, 0U);
                return CachedRawAccess();
            }
            _lastKnownChannel = currentChannel;
            PublishRawAccessLocked(
                _rawRequestedChannel != 0U && _rawRequestedChannel != currentChannel
                    ? RawWiFiPhyAccessStatus::WiFiServiceConflict
                    : RawWiFiPhyAccessStatus::Available,
                currentChannel);
            return CachedRawAccess();
        }

        if (_rawRequestedChannel != 0U && applyWhenUnconstrained) {
            if (esp_wifi_set_channel(_rawRequestedChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, currentChannel);
                return CachedRawAccess();
            }
            _lastKnownChannel = _rawRequestedChannel;
            PublishRawAccessLocked(RawWiFiPhyAccessStatus::Available, _rawRequestedChannel);
            return CachedRawAccess();
        }

        if (currentChannel == 0U) {
            if (esp_wifi_get_channel(&currentChannel, &secondary) != ESP_OK || currentChannel == 0U) {
                PublishRawAccessLocked(RawWiFiPhyAccessStatus::DriverUnavailable, 0U);
                return CachedRawAccess();
            }
            _lastKnownChannel = currentChannel;
        }
        PublishRawAccessLocked(RawWiFiPhyAccessStatus::Available, currentChannel);
        return CachedRawAccess();
    }

    mutable System::Synchronization::Mutex _mutex;
    bool _wifiServiceActive = false;
    bool _rawRegistered = false;
    bool _precisionReceiveTimestampsRequired = false;
    uint8_t _rawRequestedChannel = 0U;
    uint8_t _lastKnownChannel = 0U;
    std::atomic<std::uint32_t> _cachedRawAccess{Encode(RawWiFiPhyAccessStatus::DriverUnavailable, 0U)};
    std::atomic<IRawWiFiPhyAccessObserver*> _rawObserver{nullptr};

    friend WiFiPhyCoordinator& SharedWiFiPhy() noexcept;
};

inline WiFiPhyCoordinator& SharedWiFiPhy() noexcept {
    static WiFiPhyCoordinator instance;
    return instance;
}

} // namespace ESPressio::ESP32Platform
