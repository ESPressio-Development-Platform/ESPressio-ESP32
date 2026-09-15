#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <psa/crypto.h>

#include <ESPressio_Verification.hpp>

namespace ESPressio::ESP32Platform {

namespace OTASecurityAlgorithm {
/** ESP32 V1 asymmetric Manifest algorithm: ECDSA P-256 over SHA-256. */
inline constexpr Security::SignatureAlgorithmIdentifier ECDSA_SHA256_P256{1U};
} // namespace OTASecurityAlgorithm

namespace OTASecurityDetail {

inline Security::VerificationResult MapPSA(psa_status_t status) noexcept {
    if (status == PSA_SUCCESS) return Security::VerificationResult::Ok();
    if (status == PSA_ERROR_NOT_SUPPORTED) {
        return {Security::VerificationStatus::UnsupportedAlgorithm, status};
    }
    if (status == PSA_ERROR_INVALID_ARGUMENT || status == PSA_ERROR_BAD_STATE) {
        return {Security::VerificationStatus::InvalidArgument, status};
    }
    if (status == PSA_ERROR_INSUFFICIENT_MEMORY || status == PSA_ERROR_INSUFFICIENT_STORAGE) {
        return {Security::VerificationStatus::CapacityUnavailable, status};
    }
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        return {Security::VerificationStatus::InvalidSignature, status};
    }
    return {Security::VerificationStatus::Failed, status};
}

inline bool ConstantTimeEqual(
    const std::uint8_t* left,
    const std::uint8_t* right,
    std::size_t size) noexcept {
    std::uint8_t difference = 0U;
    for (std::size_t i = 0U; i < size; ++i) difference |= left[i] ^ right[i];
    return difference == 0U;
}

inline Security::VerificationResult EnsurePSA() noexcept {
    return MapPSA(psa_crypto_init());
}

} // namespace OTASecurityDetail

/**
 * Allocation-bounded SHA-256 producer/verifier using ESP-IDF's PSA Crypto path.
 * Separate instances may be composed for production and verification when both
 * operations can be live concurrently.
 */
class OTASHA256 final
    : public Security::IStreamingDigest,
      public Security::IStreamingDigestVerifier {
    psa_hash_operation_t operation_ = PSA_HASH_OPERATION_INIT;
    bool active_{false};

    void Reset() noexcept {
        if (active_) (void)psa_hash_abort(&operation_);
        operation_ = PSA_HASH_OPERATION_INIT;
        active_ = false;
    }

    Security::VerificationResult Finish(std::uint8_t output[32]) noexcept {
        if (!active_) {
            return {Security::VerificationStatus::InvalidArgument, PSA_ERROR_BAD_STATE};
        }
        std::size_t written = 0U;
        const auto status = psa_hash_finish(&operation_, output, 32U, &written);
        active_ = false;
        operation_ = PSA_HASH_OPERATION_INIT;
        if (status != PSA_SUCCESS) return OTASecurityDetail::MapPSA(status);
        if (written != 32U) return {Security::VerificationStatus::Failed, 0};
        return Security::VerificationResult::Ok();
    }

public:
    ~OTASHA256() override { Reset(); }

    bool Supports(Security::DigestAlgorithmIdentifier algorithm) const noexcept override {
        return algorithm == Security::DigestAlgorithm::SHA256;
    }

    std::size_t DigestSize(Security::DigestAlgorithmIdentifier algorithm) const noexcept override {
        return Supports(algorithm) ? 32U : 0U;
    }

    Security::VerificationResult Begin(Security::DigestAlgorithmIdentifier algorithm) noexcept override {
        Reset();
        if (!Supports(algorithm)) {
            return {Security::VerificationStatus::UnsupportedAlgorithm, 0};
        }
        const auto initialized = OTASecurityDetail::EnsurePSA();
        if (!initialized) return initialized;
        const auto status = psa_hash_setup(&operation_, PSA_ALG_SHA_256);
        if (status != PSA_SUCCESS) {
            operation_ = PSA_HASH_OPERATION_INIT;
            return OTASecurityDetail::MapPSA(status);
        }
        active_ = true;
        return Security::VerificationResult::Ok();
    }

    Security::VerificationResult Update(Security::ByteView bytes) noexcept override {
        if (!active_ || !bytes.IsValid()) {
            return {Security::VerificationStatus::InvalidArgument, PSA_ERROR_BAD_STATE};
        }
        if (bytes.Size == 0U) return Security::VerificationResult::Ok();
        return OTASecurityDetail::MapPSA(psa_hash_update(&operation_, bytes.Data, bytes.Size));
    }

    Security::VerificationResult Finalize(
        Security::MutableByteView output,
        std::size_t& written) noexcept override {
        written = 0U;
        if (!output.IsValid() || output.Size < 32U) {
            return {Security::VerificationStatus::CapacityUnavailable, 0};
        }
        std::array<std::uint8_t, 32> digest{};
        const auto result = Finish(digest.data());
        if (!result) return result;
        for (std::size_t i = 0U; i < digest.size(); ++i) output.Data[i] = digest[i];
        written = digest.size();
        return Security::VerificationResult::Ok();
    }

    Security::VerificationResult VerifyFinal(Security::ByteView expectedDigest) noexcept override {
        if (!expectedDigest.IsValid() || expectedDigest.Size != 32U) {
            Reset();
            return {Security::VerificationStatus::InvalidArgument, 0};
        }
        std::array<std::uint8_t, 32> digest{};
        const auto result = Finish(digest.data());
        if (!result) return result;
        return OTASecurityDetail::ConstantTimeEqual(
                   digest.data(), expectedDigest.Data, digest.size())
            ? Security::VerificationResult::Ok()
            : Security::VerificationResult{Security::VerificationStatus::DigestMismatch, 0};
    }
};

/**
 * Verifies raw 64-byte ECDSA P-256 signatures (r || s) over canonical Manifest
 * bytes. TrustAnchorView::PublicKey is the SEC1 uncompressed public point:
 * exactly 65 bytes beginning with 0x04. The wire algorithm identifier remains
 * explicit so future algorithms can coexist without reinterpretation.
 */
class OTAECDSAP256SHA256SignatureVerifier final : public Security::ISignatureVerifier {
public:
    bool Supports(Security::SignatureAlgorithmIdentifier algorithm) const noexcept override {
        return algorithm == OTASecurityAlgorithm::ECDSA_SHA256_P256;
    }

    Security::VerificationResult Verify(
        Security::SignatureAlgorithmIdentifier algorithm,
        Security::ByteView canonicalContent,
        Security::ByteView signature,
        const Security::TrustAnchorView& anchor) noexcept override {
        if (!Supports(algorithm)) {
            return {Security::VerificationStatus::UnsupportedAlgorithm, 0};
        }
        if (!canonicalContent.IsValid() || !signature.IsValid() || !anchor ||
            signature.Size != 64U || anchor.PublicKey.Size != 65U ||
            anchor.PublicKey.Data[0] != 0x04U) {
            return {Security::VerificationStatus::InvalidArgument, 0};
        }

        const auto initialized = OTASecurityDetail::EnsurePSA();
        if (!initialized) return initialized;

        std::array<std::uint8_t, 32> digest{};
        std::size_t digestLength = 0U;
        auto status = psa_hash_compute(
            PSA_ALG_SHA_256,
            canonicalContent.Data,
            canonicalContent.Size,
            digest.data(),
            digest.size(),
            &digestLength);
        if (status != PSA_SUCCESS || digestLength != digest.size()) {
            return status == PSA_SUCCESS
                ? Security::VerificationResult{Security::VerificationStatus::Failed, 0}
                : OTASecurityDetail::MapPSA(status);
        }

        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256U);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

        psa_key_id_t key = 0;
        status = psa_import_key(
            &attributes,
            anchor.PublicKey.Data,
            anchor.PublicKey.Size,
            &key);
        psa_reset_key_attributes(&attributes);
        if (status != PSA_SUCCESS) return OTASecurityDetail::MapPSA(status);

        status = psa_verify_hash(
            key,
            PSA_ALG_ECDSA(PSA_ALG_SHA_256),
            digest.data(),
            digest.size(),
            signature.Data,
            signature.Size);
        const auto destroyStatus = psa_destroy_key(key);
        if (status != PSA_SUCCESS) return OTASecurityDetail::MapPSA(status);
        if (destroyStatus != PSA_SUCCESS) return OTASecurityDetail::MapPSA(destroyStatus);
        return Security::VerificationResult::Ok();
    }
};

/** Immutable single-key trust-anchor binding for a product/application image. */
class OTAImmutableTrustAnchor final : public Security::ITrustAnchorProvider {
    Security::TrustAnchorIdentifier identifier_{};
    Security::ByteView publicKey_{};
public:
    constexpr OTAImmutableTrustAnchor(
        Security::TrustAnchorIdentifier identifier,
        const std::uint8_t* publicKey,
        std::size_t publicKeySize) noexcept
        : identifier_(identifier), publicKey_{publicKey, publicKeySize} {}

    Security::VerificationResult Resolve(
        Security::TrustAnchorIdentifier identifier,
        Security::TrustAnchorView& anchor) const noexcept override {
        anchor = {};
        if (!identifier || identifier != identifier_) {
            return {Security::VerificationStatus::TrustAnchorUnavailable, 0};
        }
        if (!publicKey_.IsValid() || publicKey_.Size == 0U) {
            return {Security::VerificationStatus::InvalidArgument, 0};
        }
        anchor = {identifier_, publicKey_};
        return Security::VerificationResult::Ok();
    }
};

/** Exact bounded Manifest trust policy: one policy identifier and one signer. */
class OTAExactManifestTrustPolicy final : public Security::ITrustPolicy {
    Security::TrustPolicyIdentifier policy_{};
    Security::TrustAnchorIdentifier anchor_{};
public:
    constexpr OTAExactManifestTrustPolicy(
        Security::TrustPolicyIdentifier policy,
        Security::TrustAnchorIdentifier anchor) noexcept
        : policy_(policy), anchor_(anchor) {}

    Security::VerificationResult Authorize(
        Security::TrustPolicyIdentifier policy,
        Security::TrustPurpose purpose,
        Security::TrustAnchorIdentifier anchor) const noexcept override {
        if (!policy || !anchor || purpose != Security::TrustPurpose::SoftwareUpdateManifest) {
            return {Security::VerificationStatus::InvalidArgument, 0};
        }
        return policy == policy_ && anchor == anchor_
            ? Security::VerificationResult::Ok()
            : Security::VerificationResult{Security::VerificationStatus::UntrustedSigner, 0};
    }
};

} // namespace ESPressio::ESP32Platform
