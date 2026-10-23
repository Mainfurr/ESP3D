/*
  ESP3D configuration file

  Copyright (c) 2014 Luc Lebosse. All rights reserved.

  This code is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This code is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
*/
#ifndef _CONFIGURATION_H
#define _CONFIGURATION_H

// Setup station as default, use AP mode first if not done.
// Uncomment and edit both definitions to enable station mode.
//#define STATION_WIFI_SSID "*********"
//#define STATION_WIFI_PASSWORD "*********"

// Optional local configuration file for SSID/password and other overrides.
#if defined(__has_include)
#  if __has_include("myconfig.h")
#    include "myconfig.h"
#  endif
#endif

/************************************
 * Shared ESP3D settings
 ************************************/

#define COMMUNICATION_PROTOCOL RAW_SERIAL
#define ESP_SERIAL_OUTPUT USE_SERIAL_0
#define DEFAULT_FW MARLIN

#define WIFI_FEATURE
#define HTTP_FEATURE
#define TELNET_FEATURE
#define NOTIFICATION_FEATURE
#define NOTIFICATION_ESP_ONLINE "Hi, %ESP_NAME% is now online at %ESP_IP%"
#define ESP_NOTIFICATION_TITLE "ESP3D Notification"


#define CAPTIVE_PORTAL_FEATURE
//Discovery methods of ESP3D
#define SSDP_FEATURE
#define WS_DATA_FEATURE // Enable websocket communications
#define MDNS_FEATURE // This method needs `bonjour` protocol on Windows, or `avahi` on Linux

#define ESP_MODEL_NUMBER "ESP3D 3.0"
#define ESP_MODEL_URL "https://www.espressif.com/en/products/devkits"
#define ESP_MANUFACTURER_NAME "Espressif Systems"
#define ESP_MANUFACTURER_URL "https://www.espressif.com"

#define FILESYSTEM_FEATURE ESP_LITTLEFS_FILESYSTEM
#define WEB_UPDATE_FEATURE // Enable web update, must contain at least 4mb flash
#define PRINTER_HAS_DISPLAY
#define SERIAL_COMMAND_FEATURE
#define ESP_GOT_IP_HOOK "[ESP212]IP:%ESP_IP%" // Hook for when ESP gets an IP address

#define GCODE_HOST_FEATURE
#define ESP_SAVE_SETTINGS SETTINGS_IN_EEPROM



// Keep hardware- and platform-specific settings in their respective files.
#if defined(ARDUINO_ARCH_ESP8266)
#include "configuration-esp8266.h"
#elif defined(ARDUINO_ARCH_ESP32)
#include "configuration-esp32.h"
#else
#error "ESP3D supports only ESP8266 and ESP32 targets"
#endif




/************************************
 * Development settings
 ************************************/

// Enable debug mode only while ESP3D is disconnected from the printer.
//#define ESP_LOG_FEATURE LOG_OUTPUT_SERIAL0
//#define ESP3D_LOG_LEVEL LOG_LEVEL_DEBUG
#ifdef ESP_LOG_FEATURE
#define LOG_ESP3D_BAUDRATE 115200
#define LOG_ESP3D_OUTPUT_PORT 8000
#endif

//#define ESP_BENCHMARK_FEATURE
//#define ESP_NO_SANITY_CHECK

/************************************
 * Sanity checks
 ************************************/

#if defined(ESP_GOT_DATE_TIME_HOOK) || defined(SD_TIMESTAMP_FEATURE) || \
    defined(FILESYSTEM_TIMESTAMP_FEATURE)
#define TIMESTAMP_FEATURE
#endif

#if defined(PRINTER_HAS_DISPLAY)
#define HAS_SERIAL_DISPLAY ""
#endif

#if defined(CAMERA_DEVICE)
#if CAMERA_DEVICE == CAMERA_MODEL_ESP32_CAM_BOARD || \
    CAMERA_DEVICE == CAMERA_MODEL_ESP32S2_CAM_BOARD
#define USE_BOARD_HEARDER 1
#endif
#endif

#if !defined(WIFI_FEATURE) && !defined(ETH_FEATURE)
#undef HTTP_FEATURE
#undef TELNET_FEATURE
#undef WEBDAV_FEATURE
#undef FTP_FEATURE
#undef WEB_UPDATE_FEATURE
#undef CAPTIVE_PORTAL_FEATURE
#undef SSDP_FEATURE
#undef MDNS_FEATURE
#undef NOTIFICATION_FEATURE
#endif

#endif  // _CONFIGURATION_H
