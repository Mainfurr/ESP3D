
/*
 * Select the configuration from the architecture supplied by the active
 * PlatformIO environment.  Do not switch these includes by hand: choosing an
 * ESP8266 environment selects the ESP-01 profile, and any ESP32 environment
 * selects the ESP32 profile.
 */
#ifndef _ESP3D_CONFIGURATION_SELECTOR_H
#define _ESP3D_CONFIGURATION_SELECTOR_H

#if defined(ARDUINO_ARCH_ESP8266)
#include "configuration-esp8266.h"
#elif defined(ARDUINO_ARCH_ESP32)
#include "configuration-esp32.h"
#else
#error "ESP3D supports only ESP8266 and ESP32 targets"
#endif

#endif  // _ESP3D_CONFIGURATION_SELECTOR_H
