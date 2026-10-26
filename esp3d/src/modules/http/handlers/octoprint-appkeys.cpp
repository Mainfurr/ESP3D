/* OctoPrint Application Keys pairing (the "Request" button in Cura's
   OctoPrint Connection plugin). A request is approved either in the WebUI or
   by clicking the printer's knob while Marlin shows an M0 prompt. Approved
   keys get user-level access to the OctoPrint API only. */
#include "octoprint-api.h"

#ifdef OCTOPRINT_APPKEYS_FEATURE

#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <string.h>
#include <uri/UriBraces.h>

#include "../../../core/esp3d_log.h"
#include "../../../core/esp3d_settings.h"
#include "../../authentication/authentication_service.h"
#include "../../bft/binary_file_transfer_service.h"
#include "../../serial/serial_service.h"
#include "../../websocket/websocket_server.h"

namespace {
using namespace OctoPrintJson;

constexpr size_t kMaxKeys = 4;
constexpr size_t kMaxRequests = 2;
constexpr size_t kKeySize = 32;
constexpr size_t kTokenSize = 16;
constexpr size_t kAppCapacity = 32;
constexpr uint32_t kPendingTimeout = 120000;
constexpr uint32_t kResultTimeout = 60000;
constexpr uint32_t kNotifyInterval = 5000;
// M0 S30: a click is an "ok" well before the timeout ends the wait.
constexpr uint32_t kLcdSeconds = 30;
constexpr uint32_t kLcdClickLimit = (kLcdSeconds - 1) * 1000;
constexpr uint32_t kLcdStartTimeout = 10000;
constexpr char kNamespace[] = "octokeys";
constexpr char kLcdPrompt[] = "//action:prompt_begin Pair ";

struct StoredKey {
  char key[kKeySize + 1];
  char app[kAppCapacity + 1];
};

enum class RequestState : uint8_t { Free, Pending, Approved, Denied };

struct Request {
  RequestState state = RequestState::Free;
  char appToken[kTokenSize + 1];
  char userToken[kTokenSize + 1];
  char app[kAppCapacity + 1];
  char key[kKeySize + 1];
  uint32_t changedAt = 0;
  uint32_t notifiedAt = 0;
};

enum class LcdState : uint8_t { None, Waiting, Sent, Shown };

StoredKey keys[kMaxKeys];
bool keysLoaded = false;
Request requests[kMaxRequests];
WebServer* server = nullptr;

LcdState lcdState = LcdState::None;
uint8_t lcdRequest = 0;
uint32_t lcdChangedAt = 0;

bool pairingEnabled() {
  return ESP3DSettings::readByte(ESP_OCTOPRINT_PAIRING) ==
         static_cast<uint8_t>(ESP3DState::on);
}

void randomHex(char* out, size_t size) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < size; ++i) out[i] = kHex[esp_random() & 0x0f];
  out[size] = '\0';
}

bool sameSecret(const char* a, const char* b, size_t size) {
  uint8_t diff = 0;
  for (size_t i = 0; i < size; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

void loadKeys() {
  if (keysLoaded) return;
  keysLoaded = true;
  memset(keys, 0, sizeof(keys));
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return;
  for (size_t i = 0; i < kMaxKeys; ++i) {
    char name[4] = {'k', static_cast<char>('0' + i), '\0', '\0'};
    prefs.getString(name, keys[i].key, sizeof(keys[i].key));
    name[0] = 'a';
    prefs.getString(name, keys[i].app, sizeof(keys[i].app));
    if (strlen(keys[i].key) != kKeySize) memset(&keys[i], 0, sizeof(keys[i]));
  }
  prefs.end();
}

bool saveKeys() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  bool ok = true;
  for (size_t i = 0; i < kMaxKeys; ++i) {
    char name[3] = {'k', static_cast<char>('0' + i), '\0'};
    ok = prefs.putString(name, keys[i].key) == strlen(keys[i].key) && ok;
    name[0] = 'a';
    ok = prefs.putString(name, keys[i].app) == strlen(keys[i].app) && ok;
  }
  prefs.end();
  return ok;
}

// A new key replaces one for the same app, else takes a free slot, else the
// oldest slot.
bool storeKey(const char* app, const char* key) {
  loadKeys();
  size_t slot = kMaxKeys;
  for (size_t i = 0; i < kMaxKeys && slot == kMaxKeys; ++i)
    if (keys[i].key[0] && strcmp(keys[i].app, app) == 0) slot = i;
  for (size_t i = 0; i < kMaxKeys && slot == kMaxKeys; ++i)
    if (!keys[i].key[0]) slot = i;
  if (slot == kMaxKeys) {
    memmove(&keys[0], &keys[1], sizeof(StoredKey) * (kMaxKeys - 1));
    slot = kMaxKeys - 1;
  }
  strcpy(keys[slot].key, key);
  strcpy(keys[slot].app, app);
  return saveKeys();
}

Request* findRequest(const String& token, bool user) {
  if (token.length() != kTokenSize) return nullptr;
  for (Request& request : requests) {
    if (request.state == RequestState::Free) continue;
    const char* value = user ? request.userToken : request.appToken;
    if (sameSecret(value, token.c_str(), kTokenSize)) return &request;
  }
  return nullptr;
}

void notifyWebUi(Request& request) {
  String message = "PAIRING:";
  if (request.state == RequestState::Pending) {
    message += request.userToken;
    message += ':';
    message += request.app;
  } else {
    message += "done:";
    message += request.userToken;
  }
  request.notifiedAt = millis();
  websocket_terminal_server.pushMSG(message.c_str());
}

bool sendToPrinter(const char* line) {
  if (BinaryFileTransferService::active()) return false;
  const size_t size = strlen(line);
  return esp3d_serial_service.writeBytes(
             reinterpret_cast<const uint8_t*>(line), size) == size;
}

void decide(Request& request, bool approve) {
  if (request.state != RequestState::Pending) return;
  if (approve) {
    randomHex(request.key, kKeySize);
    if (!storeKey(request.app, request.key)) {
      esp3d_log_e("[OCTO] cannot store app key");
      approve = false;
    }
  }
  request.state = approve ? RequestState::Approved : RequestState::Denied;
  request.changedAt = millis();
  esp3d_log("[OCTO] app key for %s %s", request.app,
            approve ? "approved" : "denied");
  notifyWebUi(request);
  // Release a printer prompt still waiting for this request.
  if (lcdState != LcdState::None && &requests[lcdRequest] == &request) {
    if (lcdState == LcdState::Shown || lcdState == LcdState::Sent)
      sendToPrinter("M108\n");
    lcdState = LcdState::None;
  }
}

void startLcdPrompt() {
  Request& request = requests[lcdRequest];
  char app[11];
  size_t size = 0;
  for (const char* p = request.app; *p && size < sizeof(app) - 1; ++p) {
    const char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.')
      app[size++] = c;
  }
  app[size] = '\0';
  char line[64];
  snprintf(line, sizeof(line), "M0 S%u Pair %s? Click\n",
           static_cast<unsigned>(kLcdSeconds), size ? app : "app");
  if (sendToPrinter(line)) {
    lcdState = LcdState::Sent;
    lcdChangedAt = millis();
  }
}

void sendJson(uint16_t code, const String& body) {
  server->send(code, "application/json", body);
}

void sendError(uint16_t code, const char* error) {
  String body = "{\"error\":";
  appendQuoted(body, error);
  body += '}';
  sendJson(code, body);
}

ESP3DAuthenticationLevel loginLevel() {
  return AuthenticationService::getAuthenticatedLevel();
}

String baseUrl() {
  String host = server->hostHeader();
  if (host.length() == 0) host = WiFi.localIP().toString();
  return "http://" + host;
}

void handleProbe() {
  if (pairingEnabled())
    server->send(204);
  else
    sendError(404, "Pairing is disabled");
}

void handleRequest() {
  if (!pairingEnabled()) {
    sendError(404, "Pairing is disabled");
    return;
  }
  char app[kAppCapacity + 1];
  if (!jsonString(server->arg("plain"), "app", app, sizeof(app)) || !app[0]) {
    sendError(400, "Missing app");
    return;
  }
  Request* request = nullptr;
  for (Request& candidate : requests)
    if (candidate.state != RequestState::Pending) {
      request = &candidate;
      break;
    }
  if (!request) {
    sendError(429, "Too many pending requests");
    return;
  }
  request->state = RequestState::Pending;
  randomHex(request->appToken, kTokenSize);
  randomHex(request->userToken, kTokenSize);
  strcpy(request->app, app);
  request->key[0] = '\0';
  request->changedAt = millis();
  esp3d_log("[OCTO] app key requested by %s", app);
  notifyWebUi(*request);
  if (lcdState == LcdState::None) {
    lcdRequest = request - requests;
    lcdState = LcdState::Waiting;
    lcdChangedAt = millis();
    // Learn the print state before deciding whether M0 may run.
    if (!OctoPrintStatus::printerIdle()) sendToPrinter("M27\n");
  }
  const String base = baseUrl();
  server->sendHeader("Location",
                     base + "/plugin/appkeys/request/" + request->appToken);
  String body = "{\"app_token\":\"";
  body += request->appToken;
  body += "\",\"auth_dialog\":\"";
  body += base;
  body += "/\"}";
  sendJson(201, body);
}

void handlePoll() {
  Request* request = findRequest(server->pathArg(0), false);
  if (!request || request->state == RequestState::Denied) {
    if (request) request->state = RequestState::Free;
    sendError(404, "No such request");
    return;
  }
  if (request->state == RequestState::Pending) {
    sendJson(202, "{}");
    return;
  }
  String body = "{\"api_key\":\"";
  body += request->key;
  body += "\"}";
  request->state = RequestState::Free;
  memset(request->key, 0, sizeof(request->key));
  sendJson(200, body);
}

void handleDecision() {
  if (loginLevel() == ESP3DAuthenticationLevel::guest) {
    sendError(401, "Log in to the WebUI first");
    return;
  }
  Request* request = findRequest(server->pathArg(0), true);
  if (!request || request->state != RequestState::Pending) {
    sendError(404, "No such request");
    return;
  }
  decide(*request, jsonTrue(server->arg("plain"), "decision"));
  server->send(204);
}

void handlePending() {
  if (loginLevel() == ESP3DAuthenticationLevel::guest) {
    sendError(401, "Log in to the WebUI first");
    return;
  }
  String body = "{\"pending\":[";
  bool first = true;
  for (const Request& request : requests) {
    if (request.state != RequestState::Pending) continue;
    if (!first) body += ',';
    first = false;
    body += "{\"user_token\":\"";
    body += request.userToken;
    body += "\",\"app\":";
    appendQuoted(body, request.app);
    body += '}';
  }
  body += "]}";
  sendJson(200, body);
}

void handleKeys() {
  if (loginLevel() != ESP3DAuthenticationLevel::admin) {
    sendError(401, "Admin login required");
    return;
  }
  loadKeys();
  if (server->method() == HTTP_POST) {
    const String body = server->arg("plain");
    char command[16];
    const char* id = findValue(body, "id");
    if (!jsonString(body, "command", command, sizeof(command)) ||
        strcmp(command, "revoke") != 0 || !id || *id < '0' ||
        *id >= static_cast<char>('0' + kMaxKeys)) {
      sendError(400, "Expected {\"command\":\"revoke\",\"id\":N}");
      return;
    }
    memset(&keys[*id - '0'], 0, sizeof(StoredKey));
    if (!saveKeys()) {
      sendError(500, "Cannot save keys");
      return;
    }
    server->send(204);
    return;
  }
  String body = "{\"enabled\":";
  body += pairingEnabled() ? "true" : "false";
  body += ",\"keys\":[";
  bool first = true;
  for (size_t i = 0; i < kMaxKeys; ++i) {
    if (!keys[i].key[0]) continue;
    if (!first) body += ',';
    first = false;
    body += "{\"id\":";
    body += i;
    body += ",\"app\":";
    appendQuoted(body, keys[i].app);
    // Enough to tell keys apart, not enough to use one.
    body += ",\"key\":\"";
    body.concat(keys[i].key, 4);
    body += "...\"}";
  }
  body += "]}";
  sendJson(200, body);
}
}  // namespace

namespace OctoPrintAppKeys {

bool valid(const String& apiKey) {
  if (apiKey.length() != kKeySize) return false;
  loadKeys();
  for (const StoredKey& stored : keys)
    if (stored.key[0] && sameSecret(stored.key, apiKey.c_str(), kKeySize))
      return true;
  return false;
}

void observeLine(const char* line, size_t size) {
  if (lcdState == LcdState::Sent && size >= strlen(kLcdPrompt) &&
      strncmp(line, kLcdPrompt, strlen(kLcdPrompt)) == 0) {
    lcdState = LcdState::Shown;
    lcdChangedAt = millis();
    return;
  }
  // Marlin runs nothing else while M0 waits, so the next "ok" is M0's.
  if (lcdState == LcdState::Shown && size >= 2 && line[0] == 'o' &&
      line[1] == 'k') {
    const bool clicked = millis() - lcdChangedAt < kLcdClickLimit;
    lcdState = LcdState::None;
    if (clicked) decide(requests[lcdRequest], true);
  }
}

void handle() {
  const uint32_t now = millis();
  for (Request& request : requests) {
    if (request.state == RequestState::Free) continue;
    const uint32_t age = now - request.changedAt;
    if (request.state == RequestState::Pending) {
      if (age >= kPendingTimeout) {
        decide(request, false);
      } else if (now - request.notifiedAt >= kNotifyInterval) {
        // Repeat for WebUI tabs opened after the request.
        notifyWebUi(request);
      }
    } else if (age >= kResultTimeout) {
      request.state = RequestState::Free;
      memset(request.key, 0, sizeof(request.key));
    }
  }
  switch (lcdState) {
    case LcdState::Waiting:
      if (requests[lcdRequest].state != RequestState::Pending ||
          now - lcdChangedAt >= kLcdStartTimeout) {
        lcdState = LcdState::None;
      } else if (OctoPrintStatus::printerIdle() &&
                 !BinaryFileTransferService::active()) {
        startLcdPrompt();
      }
      break;
    case LcdState::Sent:
      if (now - lcdChangedAt >= kLcdStartTimeout) lcdState = LcdState::None;
      break;
    case LcdState::Shown:
      if (now - lcdChangedAt >= (kLcdSeconds + 10) * 1000)
        lcdState = LcdState::None;
      break;
    default:
      break;
  }
}

void install(WebServer* webServer) {
  server = webServer;
  server->on("/plugin/appkeys/probe", HTTP_GET, handleProbe);
  server->on("/plugin/appkeys/request", HTTP_POST, handleRequest);
  server->on(UriBraces("/plugin/appkeys/request/{}"), HTTP_GET, handlePoll);
  server->on(UriBraces("/plugin/appkeys/decision/{}"), HTTP_POST,
             handleDecision);
  server->on("/plugin/appkeys/pending", HTTP_GET, handlePending);
  server->on("/api/plugin/appkeys", HTTP_ANY, handleKeys);
}

}  // namespace OctoPrintAppKeys

#endif  // OCTOPRINT_APPKEYS_FEATURE
