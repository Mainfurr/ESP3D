/* MKS-compatible HTTP facade for Marlin Binary File Transfer. */
#ifndef _BINARY_FILE_TRANSFER_HTTP_H
#define _BINARY_FILE_TRANSFER_HTTP_H

#include "../../include/esp3d_config.h"

#if COMMUNICATION_PROTOCOL == RAW_SERIAL && defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WebServer.h>

class BinaryFileTransferHttp {
 public:
  // Installs POST /upload?X-Filename=<filename> with a streamed raw body.
  static void begin(ESP8266WebServer* server);

 private:
  static ESP8266WebServer::ClientFuture handleRequest(
      const String& method, const String& url, WiFiClient* client);
};
#endif  // RAW_SERIAL && ARDUINO_ARCH_ESP8266
#endif  // _BINARY_FILE_TRANSFER_HTTP_H
