#pragma once

#include <ESPressio_Platform_IDFOTA.hpp>

namespace ESPressio::ESP32Platform {

/**
 * ESP32 OTA Platform mechanics deliberately reuse the pure ESP-IDF backend.
 * Arduino-ESP32 exposes the same ESP-IDF OTA/partition primitives, so no
 * Arduino-specific Update abstraction is inserted into the portable path.
 */
using OTAApplicationImageStaging = Platform::IDF::OTAApplicationImageStaging;
using OTABootControl = Platform::IDF::OTABootControl;
using OTATrialBoot = Platform::IDF::OTATrialBoot;
using OTASystemRestart = Platform::IDF::OTASystemRestart;
using OTAStorageLayoutInspection = Platform::IDF::OTAStorageLayoutInspection;

static_assert(Platform::OTA::IsApplicationImageStagingProviderV<OTAApplicationImageStaging>);
static_assert(Platform::OTA::IsBootControlProviderV<OTABootControl>);
static_assert(Platform::OTA::IsTrialBootProviderV<OTATrialBoot>);
static_assert(Platform::OTA::IsSystemRestartProviderV<OTASystemRestart>);
static_assert(Platform::OTA::IsStorageLayoutInspectionProviderV<OTAStorageLayoutInspection>);

} // namespace ESPressio::ESP32Platform
