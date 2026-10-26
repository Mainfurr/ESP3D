/* OctoPrint printer, job and settings endpoints for slicers such as Cura.
   The state comes from Marlin's own replies (M105, M27, host prompts) as they
   pass through ESP3D; missing data is requested with M105/M27 on demand. */
#include "octoprint-api.h"

#ifdef OCTOPRINT_STATUS_FEATURE

#include <WebServer.h>
#include <stdlib.h>
#include <string.h>
#include <uri/UriBraces.h>

#include "../../../core/esp3d_log.h"
#include "../../../core/esp3d_settings.h"
#include "../../bft/binary_file_transfer_service.h"
#include "../../serial/serial_service.h"

namespace OctoPrintJson {
void appendQuoted(String& out, const char* value) {
  out += '"';
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value);
       *p; ++p) {
    if (*p == '"' || *p == '\\') {
      out += '\\';
      out += static_cast<char>(*p);
    } else if (*p >= 0x20) {
      out += static_cast<char>(*p);
    }
  }
  out += '"';
}

// Minimal JSON readers for the small request bodies OctoPrint clients send.
const char* findValue(const String& body, const char* key) {
  String pattern = "\"";
  pattern += key;
  pattern += '"';
  const int found = body.indexOf(pattern);
  if (found < 0) return nullptr;
  const char* p = body.c_str() + found + pattern.length();
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  if (*p != ':') return nullptr;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return p;
}

// Reads a JSON string starting at the opening quote. Returns the position after
// the closing quote, or nullptr.
const char* readString(const char* p, char* out, size_t capacity) {
  if (!p || *p != '"') return nullptr;
  size_t size = 0;
  for (++p; *p && *p != '"'; ++p) {
    char c = *p;
    if (c == '\\' && p[1]) {
      ++p;
      c = *p == 'n' || *p == 'r' || *p == 't' ? ' ' : *p;
    }
    if (size + 1 < capacity) out[size++] = c;
  }
  out[size] = '\0';
  return *p == '"' ? p + 1 : nullptr;
}

bool jsonString(const String& body, const char* key, char* out,
                size_t capacity) {
  return readString(findValue(body, key), out, capacity) != nullptr;
}

bool jsonTrue(const String& body, const char* key) {
  const char* p = findValue(body, key);
  return p && strncmp(p, "true", 4) == 0;
}

}  // namespace OctoPrintJson

namespace {
using namespace OctoPrintJson;
constexpr uint32_t kTemperatureMaxAge = 5000;
constexpr uint32_t kProgressMaxAge = 5000;
constexpr uint32_t kQueryInterval = 2500;
constexpr size_t kNameCapacity = 128;
constexpr size_t kCommandCapacity = 96;

enum class Job : uint8_t { Unknown, Idle, Printing, Paused };

struct Status {
  bool haveTemperatures = false;
  float toolActual = 0, toolTarget = 0, bedActual = 0, bedTarget = 0;
  uint32_t temperaturesAt = 0;
  Job job = Job::Unknown;
  uint32_t sdPosition = 0, sdSize = 0, progressAt = 0;
  uint32_t jobStarted = 0, pausedSince = 0, pausedTotal = 0;
  bool fileNameRequested = false;
  char fileName[kNameCapacity + 1] = "";
  uint32_t lastTemperatureQuery = 0, lastProgressQuery = 0;
};

Status status;
WebServer* server = nullptr;

bool sendToPrinter(const char* line) {
  if (BinaryFileTransferService::active()) return false;
  const size_t size = strlen(line);
  return esp3d_serial_service.writeBytes(
             reinterpret_cast<const uint8_t*>(line), size) == size;
}

// Parses "T:21.50 /0.00" style values. Returns false if the key is missing.
bool parseTemperature(const char* line, const char* key, float* actual,
                      float* target) {
  const size_t keySize = strlen(key);
  for (const char* p = strstr(line, key); p; p = strstr(p + 1, key)) {
    // Skip matches inside other words, e.g. "B@:" or "ET:".
    if (p != line && p[-1] != ' ') continue;
    char* end = nullptr;
    const float value = strtof(p + keySize, &end);
    if (end == p + keySize) return false;
    *actual = value;
    while (*end == ' ') ++end;
    if (*end == '/') *target = strtof(end + 1, nullptr);
    return true;
  }
  return false;
}

void startJob() {
  status.job = Job::Printing;
  status.jobStarted = millis();
  status.pausedTotal = 0;
  status.pausedSince = 0;
  status.fileNameRequested = false;
  status.fileName[0] = '\0';
}

void setPaused(bool paused) {
  if (paused && status.job == Job::Printing) {
    status.job = Job::Paused;
    status.pausedSince = millis();
  } else if (!paused && status.job == Job::Paused) {
    status.job = Job::Printing;
    status.pausedTotal += millis() - status.pausedSince;
    status.pausedSince = 0;
  }
}

void setIdle() {
  status.job = Job::Idle;
  status.sdPosition = status.sdSize = 0;
}

void appendNumber(String& out, float value) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%.2f", value);
  out += buffer;
}

void sendJson(uint16_t code, const String& body) {
  server->send(code, "application/json", body);
}

void sendNoContent() { server->send(204); }

void sendError(uint16_t code, const char* error) {
  String body = "{\"error\":";
  appendQuoted(body, error);
  body += '}';
  sendJson(code, body);
}

bool authorized() {
  if (octoPrintRequestAuthorized(server)) return true;
  sendError(401, "Invalid or missing API key");
  return false;
}

void refreshTemperatures() {
  const uint32_t now = millis();
  if (status.haveTemperatures && now - status.temperaturesAt < kTemperatureMaxAge)
    return;
  if (status.lastTemperatureQuery &&
      now - status.lastTemperatureQuery < kQueryInterval)
    return;
  status.lastTemperatureQuery = now;
  sendToPrinter("M105\n");
}

void refreshProgress() {
  const uint32_t now = millis();
  const bool stale = status.job == Job::Unknown ||
                     (status.job != Job::Idle &&
                      now - status.progressAt >= kProgressMaxAge);
  if (stale && (!status.lastProgressQuery ||
                now - status.lastProgressQuery >= kQueryInterval)) {
    status.lastProgressQuery = now;
    sendToPrinter("M27\n");
  }
  if (status.job != Job::Idle && status.job != Job::Unknown &&
      !status.fileNameRequested) {
    status.fileNameRequested = true;
    sendToPrinter("M27 C\n");
  }
}

const char* stateText() {
  switch (status.job) {
    case Job::Printing:
      return "Printing";
    case Job::Paused:
      return "Paused";
    default:
      return "Operational";
  }
}

void appendTemperature(String& out, const char* name, float actual,
                       float target) {
  out += '"';
  out += name;
  out += "\":{\"actual\":";
  if (status.haveTemperatures)
    appendNumber(out, actual);
  else
    out += "null";
  out += ",\"target\":";
  if (status.haveTemperatures)
    appendNumber(out, target);
  else
    out += "null";
  out += ",\"offset\":0}";
}

void handlePrinter() {
  if (!authorized()) return;
  refreshTemperatures();
  refreshProgress();
  const bool printing = status.job == Job::Printing;
  const bool paused = status.job == Job::Paused;
  String body = "{\"temperature\":{";
  appendTemperature(body, "tool0", status.toolActual, status.toolTarget);
  body += ',';
  appendTemperature(body, "bed", status.bedActual, status.bedTarget);
  body += "},\"sd\":{\"ready\":true},\"state\":{\"text\":\"";
  body += stateText();
  body += "\",\"flags\":{\"operational\":true,\"printing\":";
  body += printing ? "true" : "false";
  body += ",\"paused\":";
  body += paused ? "true" : "false";
  body += ",\"pausing\":false,\"cancelling\":false,\"resuming\":false,"
          "\"finishing\":false,\"sdReady\":true,\"error\":false,\"ready\":";
  body += printing || paused ? "false" : "true";
  body += ",\"closedOrError\":false}}}";
  sendJson(200, body);
}

void handleJob() {
  if (!authorized()) return;
  refreshProgress();
  const bool active = status.job == Job::Printing || status.job == Job::Paused;
  String body = "{\"job\":{\"file\":{\"name\":";
  if (active && status.fileName[0])
    appendQuoted(body, status.fileName);
  else
    body += "null";
  body += ",\"origin\":\"local\"}},\"progress\":{\"completion\":";
  if (active && status.sdSize) {
    appendNumber(body, 100.0f * status.sdPosition / status.sdSize);
    body += ",\"filepos\":";
    body += status.sdPosition;
  } else {
    body += "null,\"filepos\":null";
  }
  body += ",\"printTime\":";
  if (active) {
    uint32_t paused = status.pausedTotal;
    if (status.job == Job::Paused) paused += millis() - status.pausedSince;
    body += (millis() - status.jobStarted - paused) / 1000;
  } else {
    body += "null";
  }
  body += ",\"printTimeLeft\":null},\"state\":\"";
  body += stateText();
  body += "\"}";
  sendJson(200, body);
}

void handleJobCommand() {
  if (!authorized()) return;
  const String body = server->arg("plain");
  char command[16], action[16] = "toggle";
  if (!jsonString(body, "command", command, sizeof(command))) {
    sendError(400, "Missing command");
    return;
  }
  jsonString(body, "action", action, sizeof(action));
  bool sent = false;
  if (strcmp(command, "pause") == 0) {
    const bool pause = strcmp(action, "pause") == 0 ||
                       (strcmp(action, "toggle") == 0 &&
                        status.job == Job::Printing);
    const bool resume = strcmp(action, "resume") == 0 ||
                        (strcmp(action, "toggle") == 0 &&
                         status.job == Job::Paused);
    if (pause && status.job == Job::Printing) {
      sent = sendToPrinter("M25\n");
      if (sent) setPaused(true);
    } else if (resume && status.job == Job::Paused) {
      sent = sendToPrinter("M24\n");
      if (sent) setPaused(false);
    }
  } else if (strcmp(command, "cancel") == 0) {
    if (status.job == Job::Printing || status.job == Job::Paused) {
      sent = sendToPrinter("M524\n");
      if (sent) setIdle();
    }
  }
  if (sent)
    sendNoContent();
  else
    sendError(409, "Printer is not in a state for this command");
}

void handlePrinterCommand() {
  if (!authorized()) return;
  if (BinaryFileTransferService::active()) {
    sendError(409, "Printer UART is reserved for BFT upload");
    return;
  }
  const String body = server->arg("plain");
  char line[kCommandCapacity + 2];
  size_t sent = 0;
  const char* p = findValue(body, "commands");
  if (p && *p == '[') {
    ++p;
    while (true) {
      while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n') ++p;
      if (*p != '"') break;
      p = readString(p, line, kCommandCapacity + 1);
      if (!p) break;
      if (line[0] && line[0] != '[') {
        strcat(line, "\n");
        if (sendToPrinter(line)) ++sent;
      }
    }
  } else if (jsonString(body, "command", line, kCommandCapacity + 1) &&
             line[0] && line[0] != '[') {
    strcat(line, "\n");
    if (sendToPrinter(line)) ++sent;
  }
  if (sent)
    sendNoContent();
  else
    sendError(400, "No command sent");
}

void urlDecode(const String& in, char* out, size_t capacity) {
  size_t size = 0;
  for (size_t i = 0; i < in.length() && size + 1 < capacity; ++i) {
    char c = in.charAt(i);
    if (c == '%' && i + 2 < in.length()) {
      char hex[3] = {in.charAt(i + 1), in.charAt(i + 2), '\0'};
      c = static_cast<char>(strtol(hex, nullptr, 16));
      i += 2;
    }
    out[size++] = c;
  }
  out[size] = '\0';
}

// POST /api/files/local/<name> {"command":"select","print":true}
void handleFileCommand() {
  if (!authorized()) return;
  const String body = server->arg("plain");
  char command[16];
  char name[kNameCapacity + 1];
  urlDecode(server->pathArg(0), name, sizeof(name));
  if (!jsonString(body, "command", command, sizeof(command)) ||
      strcmp(command, "select") != 0) {
    sendError(400, "Only the select command is supported");
    return;
  }
  if (!name[0] || strchr(name, '\n') || strstr(name, "..")) {
    sendError(400, "Invalid filename");
    return;
  }
  if (status.job == Job::Printing || status.job == Job::Paused) {
    sendError(409, "Printer is busy");
    return;
  }
  bool ok;
  if (jsonTrue(body, "print")) {
    ok = BinaryFileTransferService::startPrint(name);
  } else {
    String line = "M23 ";
    line += name;
    line += '\n';
    ok = sendToPrinter(line.c_str());
  }
  if (ok)
    sendNoContent();
  else
    sendError(409, BinaryFileTransferService::error()[0]
                       ? BinaryFileTransferService::error()
                       : "Printer UART is busy");
}

void handleSettings() {
  if (!authorized()) return;
  sendJson(200,
           "{\"feature\":{\"sdSupport\":false},"
           "\"webcam\":{\"webcamEnabled\":false,\"streamUrl\":\"\"},"
           "\"plugins\":{}}");
}

void handlePrinterProfiles() {
  if (!authorized()) return;
  String body =
      "{\"profiles\":{\"_default\":{\"id\":\"_default\",\"name\":";
  appendQuoted(body, ESP3DSettings::readString(ESP_HOSTNAME));
  body +=
      ",\"model\":\"Marlin\",\"current\":true,\"default\":true,"
      "\"heatedBed\":true,\"heatedChamber\":false,"
      "\"extruder\":{\"count\":1},"
      "\"axes\":{\"x\":{\"speed\":6000,\"inverted\":false},"
      "\"y\":{\"speed\":6000,\"inverted\":false},"
      "\"z\":{\"speed\":200,\"inverted\":false},"
      "\"e\":{\"speed\":300,\"inverted\":false}}}}}";
  sendJson(200, body);
}

void handleLogin() {
  if (!authorized()) return;
  sendJson(200, "{\"name\":\"esp3d\",\"active\":true}");
}

void handleConnection() {
  if (!authorized()) return;
  if (server->method() == HTTP_POST) {
    sendNoContent();
    return;
  }
  sendJson(200,
           "{\"current\":{\"state\":\"Operational\",\"port\":\"ESP3D\","
           "\"baudrate\":0,\"printerProfile\":\"_default\"},"
           "\"options\":{\"ports\":[\"ESP3D\"],\"baudrates\":[],"
           "\"printerProfiles\":[{\"id\":\"_default\"}],"
           "\"portPreference\":\"ESP3D\",\"baudratePreference\":0,"
           "\"printerProfilePreference\":\"_default\",\"autoconnect\":true}}");
}
}  // namespace

namespace OctoPrintStatus {

bool printerIdle() { return status.job == Job::Idle; }

void observeLine(const char* line, size_t size) {
  if (!line || size == 0) return;
  char buffer[160];
  if (size >= sizeof(buffer)) size = sizeof(buffer) - 1;
  memcpy(buffer, line, size);
  buffer[size] = '\0';
  while (size && (buffer[size - 1] == '\n' || buffer[size - 1] == '\r'))
    buffer[--size] = '\0';
  const char* text = buffer;
  if (strncmp(text, "echo:", 5) == 0) text += 5;
  if (strncmp(text, "ok", 2) == 0 && (text[2] == ' ' || text[2] == '\0'))
    text += 2;
  while (*text == ' ') ++text;

  if (strstr(text, "T:")) {
    float toolActual = status.toolActual, toolTarget = status.toolTarget;
    float bedActual = status.bedActual, bedTarget = status.bedTarget;
    if (parseTemperature(text, "T:", &toolActual, &toolTarget)) {
      parseTemperature(text, "B:", &bedActual, &bedTarget);
      status.toolActual = toolActual;
      status.toolTarget = toolTarget;
      status.bedActual = bedActual;
      status.bedTarget = bedTarget;
      status.haveTemperatures = true;
      status.temperaturesAt = millis();
    }
    return;
  }
  static constexpr char kPrinting[] = "SD printing byte ";
  if (strncmp(text, kPrinting, sizeof(kPrinting) - 1) == 0) {
    char* end = nullptr;
    const uint32_t position = strtoul(text + sizeof(kPrinting) - 1, &end, 10);
    const uint32_t total = end && *end == '/' ? strtoul(end + 1, nullptr, 10) : 0;
    if (status.job == Job::Idle || status.job == Job::Unknown ||
        position < status.sdPosition)
      startJob();
    status.sdPosition = position;
    status.sdSize = total;
    status.progressAt = millis();
    return;
  }
  if (strncmp(text, "Not SD printing", 15) == 0 ||
      strncmp(text, "Done printing file", 18) == 0) {
    setIdle();
    status.progressAt = millis();
    return;
  }
  static constexpr char kFile[] = "Current file: ";
  if (strncmp(text, kFile, sizeof(kFile) - 1) == 0) {
    const char* name = text + sizeof(kFile) - 1;
    if (strcmp(name, "(no file)") == 0) {
      status.fileName[0] = '\0';
      return;
    }
    // "DOSNAME~1.GCO Long name.gcode": prefer the long name when present.
    const char* space = strchr(name, ' ');
    if (space && space[1]) name = space + 1;
    strncpy(status.fileName, name, kNameCapacity);
    status.fileName[kNameCapacity] = '\0';
    return;
  }
  // Marlin's host prompts for pause and resume (HOST_PROMPT_SUPPORT).
  static constexpr char kPrompt[] = "//action:prompt_begin ";
  if (strncmp(text, kPrompt, sizeof(kPrompt) - 1) == 0) {
    const char* prompt = text + sizeof(kPrompt) - 1;
    if (strcmp(prompt, "Pause") == 0 || strcmp(prompt, "Pause SD") == 0)
      setPaused(true);
    else if (strncmp(prompt, "Resuming", 8) == 0)
      setPaused(false);
  }
}

void install(WebServer* webServer) {
  server = webServer;
  server->on("/api/printer", HTTP_GET, handlePrinter);
  server->on("/api/printer/command", HTTP_POST, handlePrinterCommand);
  server->on("/api/job", HTTP_GET, handleJob);
  server->on("/api/job", HTTP_POST, handleJobCommand);
  server->on("/api/settings", HTTP_GET, handleSettings);
  server->on("/api/printerprofiles", HTTP_GET, handlePrinterProfiles);
  server->on("/api/login", HTTP_POST, handleLogin);
  server->on("/api/connection", HTTP_ANY, handleConnection);
  server->on(UriBraces("/api/files/local/{}"), HTTP_POST, handleFileCommand);
}

}  // namespace OctoPrintStatus

namespace OctoPrintApi {

void install(WebServer* webServer) {
  OctoPrintStatus::install(webServer);
#ifdef OCTOPRINT_APPKEYS_FEATURE
  OctoPrintAppKeys::install(webServer);
#endif  // OCTOPRINT_APPKEYS_FEATURE
}

void handle() {
#ifdef OCTOPRINT_APPKEYS_FEATURE
  OctoPrintAppKeys::handle();
#endif  // OCTOPRINT_APPKEYS_FEATURE
}

void observeSerial(const uint8_t* data, size_t size) {
  const char* line = reinterpret_cast<const char*>(data);
  OctoPrintStatus::observeLine(line, size);
#ifdef OCTOPRINT_APPKEYS_FEATURE
  OctoPrintAppKeys::observeLine(line, size);
#endif  // OCTOPRINT_APPKEYS_FEATURE
}

}  // namespace OctoPrintApi

#endif  // OCTOPRINT_STATUS_FEATURE
