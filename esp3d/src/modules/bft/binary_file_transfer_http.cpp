/* MKS-compatible HTTP facade for Marlin Binary File Transfer. */
#include "../../include/esp3d_config.h"

#if COMMUNICATION_PROTOCOL == RAW_SERIAL && defined(ARDUINO_ARCH_ESP8266)

#include <ESP8266WebServer.h>
#ifdef AUTHENTICATION_FEATURE
#include <libb64/cdecode.h>
#endif  // AUTHENTICATION_FEATURE

#include "../authentication/authentication_service.h"
#include "binary_file_transfer_http.h"
#include "binary_file_transfer_service.h"

namespace {
constexpr size_t kHttpReadBufferSize = 512;
constexpr uint32_t kHttpBodyTimeout = 5000;
constexpr uint32_t kHttpUploadTimeout = 30UL * 60 * 1000;

void sendJson(WiFiClient* client, uint16_t status, uint8_t error) {
  const char* reason = status == 200   ? "OK"
                       : status == 401 ? "Unauthorized"
                                       : "Bad Request";
  client->printf(
      "HTTP/1.1 %u %s\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 9\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-cache\r\n\r\n"
      "{\"err\":%u}",
      status, reason, error);
}

bool parseFilename(const String& url, String* filename) {
  const int queryStart = url.indexOf('?');
  if (queryStart < 0 || url.substring(0, queryStart) != "/upload") return false;
  const String query = url.substring(queryStart + 1);
  int offset = 0;
  while (offset <= static_cast<int>(query.length())) {
    const int separator = query.indexOf('&', offset);
    const String item =
        query.substring(offset, separator < 0 ? query.length() : separator);
    const int equal = item.indexOf('=');
    if (equal > 0 && item.substring(0, equal) == "X-Filename") {
      *filename = ESP8266WebServer::urlDecode(item.substring(equal + 1));
      break;
    }
    if (separator < 0) break;
    offset = separator + 1;
  }
  if (filename->length() == 0 || filename->length() > 120) return false;
  for (size_t i = 0; i < filename->length(); ++i) {
    const uint8_t value = filename->charAt(i);
    if (value < 0x20 || value == 0x7f || strchr("/\\:*?\"<>|", value))
      return false;
  }
  return true;
}

bool parseContentLength(const String& value, uint32_t* length) {
  if (value.length() == 0) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    if (c < '0' || c > '9' || parsed > (UINT32_MAX - (c - '0')) / 10)
      return false;
    parsed = parsed * 10 + c - '0';
  }
  *length = parsed;
  return true;
}

#ifdef AUTHENTICATION_FEATURE
bool isAuthenticated(const String& authorization, const String& cookie,
                     const IPAddress& remoteIp) {
  if (authorization.startsWith("Basic ")) {
    const String encoded = authorization.substring(6);
    char decoded[128] = {0};
    const int decodedLength =
        base64_decode_chars(encoded.c_str(), encoded.length(), decoded);
    if (decodedLength > 0 &&
        decodedLength < static_cast<int>(sizeof(decoded))) {
      decoded[decodedLength] = '\0';
      const char* separator = strchr(decoded, ':');
      if (separator != nullptr) {
        const String user = String(decoded).substring(0, separator - decoded);
        const char* password = separator + 1;
        if ((user == DEFAULT_ADMIN_LOGIN &&
             AuthenticationService::isadmin(password)) ||
            (user == DEFAULT_USER_LOGIN &&
             AuthenticationService::isuser(password))) {
          return true;
        }
      }
    }
  }
  const int cookieStart = cookie.indexOf("ESPSESSIONID=");
  if (cookieStart < 0) return false;
  const int valueStart = cookieStart + strlen("ESPSESSIONID=");
  const int cookieEnd = cookie.indexOf(';', valueStart);
  const String session =
      cookie.substring(valueStart, cookieEnd < 0 ? cookie.length() : cookieEnd);
  return AuthenticationService::getAuthenticatedLevel(
             remoteIp, session.c_str()) != ESP3DAuthenticationLevel::guest;
}
#endif  // AUTHENTICATION_FEATURE
}  // namespace

void BinaryFileTransferHttp::begin(ESP8266WebServer* server) {
  server->addHook([](const String& method, const String& url,
                     WiFiClient* client,
                     ESP8266WebServer::ContentTypeFunction) {
    return handleRequest(method, url, client);
  });
}

ESP8266WebServer::ClientFuture BinaryFileTransferHttp::handleRequest(
    const String& method, const String& url, WiFiClient* client) {
  if (method != "POST" || !(url == "/upload" || url.startsWith("/upload?"))) {
    return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
  }

  String filename;
  if (!parseFilename(url, &filename)) {
    sendJson(client, 400, 1);
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }

  String contentLengthHeader;
  String authorization;
  String cookie;
  bool chunked = false;
  while (client->connected()) {
    String line = client->readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    const int separator = line.indexOf(':');
    if (separator <= 0) {
      sendJson(client, 400, 1);
      return ESP8266WebServer::CLIENT_IS_GIVEN;
    }
    const String name = line.substring(0, separator);
    String value = line.substring(separator + 1);
    value.trim();
    if (name.equalsIgnoreCase("Content-Length")) {
      contentLengthHeader = value;
    } else if (name.equalsIgnoreCase("Transfer-Encoding") &&
               value.indexOf("chunked") >= 0) {
      chunked = true;
    } else if (name.equalsIgnoreCase("Authorization")) {
      authorization = value;
    } else if (name.equalsIgnoreCase("Cookie")) {
      cookie = value;
    }
  }

  uint32_t contentLength = 0;
  if (chunked || !parseContentLength(contentLengthHeader, &contentLength)) {
    sendJson(client, 400, 1);
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }
#ifdef AUTHENTICATION_FEATURE
  if (!isAuthenticated(authorization, cookie, client->remoteIP())) {
    sendJson(client, 401, 1);
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }
#endif  // AUTHENTICATION_FEATURE

  // Do not let a second request abort an existing or unresolved session.
  if (BinaryFileTransferService::active()) {
    sendJson(client, 400, 1);
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }
  if (!BinaryFileTransferService::begin(filename.c_str())) {
    BinaryFileTransferService::abort();
    sendJson(client, 400, 1);
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }

  uint8_t buffer[kHttpReadBufferSize];
  uint32_t remaining = contentLength;
  bool successful = true;
  uint32_t lastData = millis();
  const uint32_t started = lastData;
  while (remaining > 0) {
    if (!BinaryFileTransferService::idle() ||
        millis() - started >= kHttpUploadTimeout) {
      successful = false;
      break;
    }
    const int available = client->available();
    if (available > 0) {
      size_t requested =
          remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
      if (requested > static_cast<size_t>(available)) requested = available;
      // Read only already buffered bytes; never block UART/keepalive on TCP.
      const int received = client->read(buffer, requested);
      if (received <= 0 || !BinaryFileTransferService::write(
                               buffer, static_cast<size_t>(received))) {
        successful = false;
        break;
      }
      remaining -= received;
      lastData = millis();
    } else if (!client->connected() ||
               millis() - lastData >= kHttpBodyTimeout) {
      successful = false;
      break;
    }
    ESP3DHal::wait(1);
  }
  if (successful) successful = BinaryFileTransferService::finish();
  if (!successful) BinaryFileTransferService::abort();
  sendJson(client, successful ? 200 : 400, successful ? 0 : 1);
  return ESP8266WebServer::CLIENT_IS_GIVEN;
}

#endif  // RAW_SERIAL && ARDUINO_ARCH_ESP8266
