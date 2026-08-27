#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_ESP32WiFiRadio.hpp requires an ESP32 Arduino target"
#endif

#include <cstdint>
#include <esp_wifi.h>

namespace ESPressio::WiFi {

struct ESP32WiFiRadioFingerprint {
    wifi_mode_t Mode = WIFI_MODE_NULL;
    uint8_t PrimaryChannel = 0;
    wifi_second_chan_t SecondaryChannel = WIFI_SECOND_CHAN_NONE;
    wifi_ps_type_t PowerSave = WIFI_PS_NONE;
    int8_t MaximumTxPowerQuarterDbm = 0;
    bool ModeAvailable = false;
    bool ChannelAvailable = false;
    bool PowerSaveAvailable = false;
    bool TxPowerAvailable = false;

    bool operator==(const ESP32WiFiRadioFingerprint& other) const noexcept {
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

    bool operator!=(const ESP32WiFiRadioFingerprint& other) const noexcept {
        return !(*this == other);
    }
};

inline ESP32WiFiRadioFingerprint ReadESP32WiFiRadioFingerprint() {
    ESP32WiFiRadioFingerprint result;
    result.ModeAvailable = esp_wifi_get_mode(&result.Mode) == ESP_OK;
    result.ChannelAvailable =
        esp_wifi_get_channel(&result.PrimaryChannel, &result.SecondaryChannel) == ESP_OK;
    result.PowerSaveAvailable = esp_wifi_get_ps(&result.PowerSave) == ESP_OK;
    result.TxPowerAvailable =
        esp_wifi_get_max_tx_power(&result.MaximumTxPowerQuarterDbm) == ESP_OK;
    return result;
}

inline bool ApplyESP32WiFiRadioPolicy(int8_t txPowerDbm, bool powerSave) {
    if (txPowerDbm < 2) txPowerDbm = 2;
    if (txPowerDbm > 20) txPowerDbm = 20;

    if (esp_wifi_set_max_tx_power(static_cast<int8_t>(txPowerDbm * 4)) != ESP_OK)
        return false;
    if (esp_wifi_set_ps(powerSave ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) != ESP_OK)
        return false;
    return true;
}

} // namespace ESPressio::WiFi
