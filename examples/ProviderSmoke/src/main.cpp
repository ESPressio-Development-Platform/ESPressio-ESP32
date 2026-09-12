#include <Arduino.h>
#include <sdkconfig.h>

#include <ESPressio_ESP32.hpp>
#include <ESPressio_Radio.hpp>

namespace {
ESPressio::ESP32Platform::Raw80211Radio rawRadio;
#if defined(CONFIG_BT_BLE_ENABLED) && CONFIG_BT_BLE_ENABLED && defined(CONFIG_BT_BLUEDROID_ENABLED) && CONFIG_BT_BLUEDROID_ENABLED
ESPressio::ESP32Platform::BLERadio bleRadio;
#endif
}

void setup() {
    ESPressio::ESP32Platform::InstallSystemProviders();

    const auto rawResources = rawRadio.ProviderResources();
    const auto rawDomain = rawRadio.ContentionDomain();
    const auto rawCost = rawRadio.EstimateTransmissionCost(
        ESPressio::Radio::RadioAddress::Broadcast(6),
        32,
        {ESPressio::Radio::RadioServiceClass::BestEffort,
         ESPressio::Radio::RadioDeadlineTreatment::ExpiryOnly,
         ESPressio::Radio::RadioDirectLinkEvidenceRequirement::TransmissionCompletion});

    volatile bool rawFiniteIngress = rawResources.HasFiniteIngressService();
    volatile bool rawDomainValid = static_cast<bool>(rawDomain);
    volatile bool rawCostValid = rawCost.IsValid();
    (void)rawFiniteIngress;
    (void)rawDomainValid;
    (void)rawCostValid;

#if defined(CONFIG_BT_BLE_ENABLED) && CONFIG_BT_BLE_ENABLED && defined(CONFIG_BT_BLUEDROID_ENABLED) && CONFIG_BT_BLUEDROID_ENABLED
    const auto bleCapabilities = bleRadio.Capabilities();
    const auto bleResources = bleRadio.ProviderResources();
    volatile bool bleV3Compatible = bleCapabilities.MaximumPayloadBytes >= 21;
    volatile bool bleFiniteIngress = bleResources.HasFiniteIngressService();
    volatile bool bleBroadcast = bleCapabilities.Has(ESPressio::Radio::RadioCapability::Broadcast);
    (void)bleV3Compatible;
    (void)bleFiniteIngress;
    (void)bleBroadcast;
#endif
}

void loop() {}
