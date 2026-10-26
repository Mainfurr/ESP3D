/* OctoPrint API emulation beyond the upload endpoint: printer and job status
   for slicers such as Cura, and OctoPrint's Application Keys pairing. */
#ifndef _OCTOPRINT_API_H
#define _OCTOPRINT_API_H

#include "../../../include/esp3d_config.h"

#if COMMUNICATION_PROTOCOL == RAW_SERIAL && defined(ARDUINO_ARCH_ESP32)
#define OCTOPRINT_STATUS_FEATURE
#if defined(AUTHENTICATION_FEATURE)
#define OCTOPRINT_APPKEYS_FEATURE
#endif  // AUTHENTICATION_FEATURE
#endif  // RAW_SERIAL && ESP32

#ifdef OCTOPRINT_STATUS_FEATURE
#include <Arduino.h>

class WebServer;

// Shared by the OctoPrint handlers: X-Api-Key (ESP3D password or paired app
// key) or an ESP3D login.
bool octoPrintRequestAuthorized(WebServer* server);
bool octoPrintApiKeyAuthorized(const String& apiKey);

// Minimal JSON helpers for the small bodies OctoPrint clients send.
namespace OctoPrintJson {
void appendQuoted(String& out, const char* value);
const char* findValue(const String& body, const char* key);
const char* readString(const char* p, char* out, size_t capacity);
bool jsonString(const String& body, const char* key, char* out,
                size_t capacity);
bool jsonTrue(const String& body, const char* key);
}  // namespace OctoPrintJson

namespace OctoPrintStatus {
void install(WebServer* server);
// Every line Marlin sends, to follow temperatures and the SD print state.
void observeLine(const char* line, size_t size);
// True when no SD job is known to be running or paused.
bool printerIdle();
}  // namespace OctoPrintStatus

#ifdef OCTOPRINT_APPKEYS_FEATURE
namespace OctoPrintAppKeys {
void install(WebServer* server);
void handle();
void observeLine(const char* line, size_t size);
bool valid(const String& apiKey);
}  // namespace OctoPrintAppKeys
#endif  // OCTOPRINT_APPKEYS_FEATURE

namespace OctoPrintApi {
void install(WebServer* server);
void handle();
void observeSerial(const uint8_t* data, size_t size);
}  // namespace OctoPrintApi
#endif  // OCTOPRINT_STATUS_FEATURE

#endif  // _OCTOPRINT_API_H
