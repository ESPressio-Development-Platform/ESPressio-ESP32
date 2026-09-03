#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_WiFiRadio.hpp requires an ESP32 Arduino target"
#endif

#include <cstdint>
#include <esp_wifi.h>

#include "ESPressio_WiFiPhyCoordinator.hpp"

namespace ESPressio::WiFi {

/// <summary>Diagnostic snapshot of hardware-global ESP32 Wi-Fi PHY state.</summary>
struct WiFiRadioFingerprint {
    wifi_mode_t Mode = WIFI_MODE_NULL;
    uint8_t PrimaryChannel = 0;
    wifi_second_chan_t SecondaryChannel = WIFI_SECOND_CHAN_NONE;
    wifi_ps_type_t PowerSave = WIFI_PS_NONE;
    int8_t MaximumTxPowerQuarterDbm = 0;
    bool ModeAvailable = false;
    bool ChannelAvailable = false;
    bool PowerSaveAvailable = false;
    bool TxPowerAvailable = false;

    bool operator==(const WiFiRadioFingerprint& other) const noexcept {
        return Mode == other.Mode &&
            PrimaryChannel == other.PrimaryChannel &&
            SecondaryChannel == other.SecondaryChannel &&
            PowerSave == other.PowerSave &&
            MaximumTxPowerQuarterDbm == other.MaximumTxPowerQuarterDbm &&
            ModeAvailable == other.ModeAvailable &&
            ChannelAvailable == other.ChannelAvailable &&
            PowerSaveAvailable == other.PowerSaveAvailable &&
            TxPowerAvailable == other.TxPowerAvailable;
    }

    bool operator!=(const WiFiRadioFingerprint& other) const noexcept {
        return !(*this == other);
    }
};

/// <summary>Reads current shared Wi-Fi PHY state without mutating it.</summary>
inline WiFiRadioFingerprint ReadWiFiRadioFingerprint() {
    WiFiRadioFingerprint result;
    result.ModeAvailable = esp_wifi_get_mode(&result.Mode) == ESP_OK;
    result.ChannelAvailable =
        esp_wifi_get_channel(&result.PrimaryChannel, &result.SecondaryChannel) == ESP_OK;
    result.PowerSaveAvailable = esp_wifi_get_ps(&result.PowerSave) == ESP_OK;
    result.TxPowerAvailable =
        esp_wifi_get_max_tx_power(&result.MaximumTxPowerQuarterDbm) == ESP_OK;
    return result;
}

/// <summary>
/// Applies ordinary Wi-Fi's hardware-global transmit-power/power-save policy through the single ESP32 PHY coordinator.
/// </summary>
inline bool ApplyWiFiRadioPolicy(int8_t txPowerDbm, bool powerSave) {
    return ESP32Platform::SharedWiFiPhy().ApplyWiFiPolicy(txPowerDbm, powerSave);
}

} // namespace ESPressio::WiFi
