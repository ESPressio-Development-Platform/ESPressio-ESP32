#include <Arduino.h>
#include <sdkconfig.h>

#include <array>
#include <cstddef>

#include <ESPressio_ESP32.hpp>
#include <ESPressio_Radio.hpp>

namespace {
ESPressio::ESP32Platform::Raw80211Radio rawRadio;
#if defined(CONFIG_BT_BLE_ENABLED) && CONFIG_BT_BLE_ENABLED && defined(CONFIG_BT_BLUEDROID_ENABLED) && CONFIG_BT_BLUEDROID_ENABLED
ESPressio::ESP32Platform::BLERadio bleRadio;
#endif

#if __has_include(<ESPressio_Platform_IDFOTA.hpp>)
ESPressio::ESP32Platform::OTAApplicationImageStaging otaApplicationStaging;
ESPressio::ESP32Platform::OTABootControl otaBootControl;
ESPressio::ESP32Platform::OTATrialBoot otaTrialBoot;
ESPressio::ESP32Platform::OTASystemRestart otaSystemRestart;
ESPressio::ESP32Platform::OTAStorageLayoutInspection otaStorageLayoutInspection;
static_assert(ESPressio::Platform::OTA::IsApplicationImageStagingProviderV<
              ESPressio::ESP32Platform::OTAApplicationImageStaging>);
static_assert(ESPressio::Platform::OTA::IsBootControlProviderV<
              ESPressio::ESP32Platform::OTABootControl>);
static_assert(ESPressio::Platform::OTA::IsTrialBootProviderV<
              ESPressio::ESP32Platform::OTATrialBoot>);
static_assert(ESPressio::Platform::OTA::IsSystemRestartProviderV<
              ESPressio::ESP32Platform::OTASystemRestart>);
static_assert(ESPressio::Platform::OTA::IsStorageLayoutInspectionProviderV<
              ESPressio::ESP32Platform::OTAStorageLayoutInspection>);
#endif

#if __has_include(<ESPressio_OTACapacityProfile.hpp>)
static_assert(ESPressio::ESP32Platform::ESP32OTACapacityProfile::IsValid);
static_assert(ESPressio::ESP32Platform::ESP32OTACapacityProfile::TransferBufferBytes == 4096U);
#endif

#if __has_include(<ESPressio_Verification.hpp>) && __has_include(<psa/crypto.h>)
ESPressio::ESP32Platform::OTASHA256 otaSHA256;
ESPressio::ESP32Platform::OTAECDSAP256SHA256SignatureVerifier otaSignatureVerifier;
constexpr std::array<std::uint8_t, 65U> otaSmokePublicKey = [] {
    std::array<std::uint8_t, 65U> key{};
    key[0] = 0x04U;
    return key;
}();
constexpr ESPressio::Security::TrustAnchorIdentifier otaSmokeAnchorId{1U};
constexpr ESPressio::Security::TrustPolicyIdentifier otaSmokePolicyId{1U};
ESPressio::ESP32Platform::OTAImmutableTrustAnchor otaTrustAnchor{
    otaSmokeAnchorId, otaSmokePublicKey.data(), otaSmokePublicKey.size()};
ESPressio::ESP32Platform::OTAExactManifestTrustPolicy otaTrustPolicy{
    otaSmokePolicyId, otaSmokeAnchorId};
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

#if __has_include(<ESPressio_Platform_IDFOTA.hpp>)
    // Construction plus compile-time provider traits are the smoke target here;
    // do not mutate flash/boot state merely to prove the integration compiles.
    volatile std::size_t otaPlatformProviderBytes =
        sizeof(otaApplicationStaging) + sizeof(otaBootControl) + sizeof(otaTrialBoot) +
        sizeof(otaSystemRestart) + sizeof(otaStorageLayoutInspection);
    (void)otaPlatformProviderBytes;
#endif

#if __has_include(<ESPressio_Verification.hpp>) && __has_include(<psa/crypto.h>)
    volatile bool sha256Supported = otaSHA256.Supports(ESPressio::Security::DigestAlgorithm::SHA256);
    volatile bool signatureSupported = otaSignatureVerifier.Supports(
        ESPressio::ESP32Platform::OTASecurityAlgorithm::ECDSA_SHA256_P256);
    ESPressio::Security::TrustAnchorView anchor{};
    const auto anchorResult = otaTrustAnchor.Resolve(otaSmokeAnchorId, anchor);
    const auto policyResult = otaTrustPolicy.Authorize(
        otaSmokePolicyId,
        ESPressio::Security::TrustPurpose::SoftwareUpdateManifest,
        otaSmokeAnchorId);
    volatile bool trustBound = static_cast<bool>(anchorResult) && static_cast<bool>(policyResult) &&
                               static_cast<bool>(anchor);
    (void)sha256Supported;
    (void)signatureSupported;
    (void)trustBound;
#endif

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
