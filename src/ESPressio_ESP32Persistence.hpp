#pragma once

#include <ESPressio_Persistence.hpp>

#if defined(ARDUINO_ARCH_ESP32)
#include "ESPressio_ESP32LittleFSStorage.hpp"
#include "ESPressio_ESP32SPIFFSStorage.hpp"
#include "ESPressio_ESP32FFatStorage.hpp"
#include "ESPressio_ESP32PreferencesStorage.hpp"
#include "ESPressio_ESP32SDStorage.hpp"
#include "ESPressio_ESP32SDMMCStorage.hpp"
#endif
