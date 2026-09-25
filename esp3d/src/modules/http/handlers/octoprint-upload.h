#ifndef _OCTOPRINT_UPLOAD_H
#define _OCTOPRINT_UPLOAD_H

#include "../../../include/esp3d_config.h"

#if COMMUNICATION_PROTOCOL == RAW_SERIAL && defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WebServer.h>
void installOctoPrintUpload(ESP8266WebServer* server);
#elif COMMUNICATION_PROTOCOL == RAW_SERIAL && defined(ARDUINO_ARCH_ESP32)
class WebServer;
void installOctoPrintUpload(WebServer* server);
#endif

#endif  // _OCTOPRINT_UPLOAD_H
