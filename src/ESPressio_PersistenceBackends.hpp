#pragma once

#include <ESPressio_Persistence.hpp>

#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_LittleFSStorage.hpp"
#include "ESPressio_SPIFFSStorage.hpp"
#include "ESPressio_FFatStorage.hpp"
#include "ESPressio_PreferencesStorage.hpp"
#include "ESPressio_SDStorage.hpp"
#include "ESPressio_SDMMCStorage.hpp"
#endif
