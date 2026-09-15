#pragma once

#include <ESPressio_OTACapacityProfile.hpp>

namespace ESPressio::ESP32Platform {

/**
 * Initial ESP32 OTA convenience profile.
 *
 * The constrained V1 profile is intentionally used as the ESP32 starting point
 * so WROOM-32-class targets remain first-class validation hardware rather than
 * allowing larger ESP32 variants to conceal resource growth. Applications may
 * still bind a different explicit OTA capacity profile when justified.
 */
using ESP32OTACapacityProfile = OTA::ConstrainedV1CapacityProfile;

static_assert(ESP32OTACapacityProfile::IsValid);
static_assert(ESP32OTACapacityProfile::TransferBufferBytes == 4096U);

} // namespace ESPressio::ESP32Platform
