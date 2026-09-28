/* Minimal OctoPrint upload API backed directly by Marlin BFT. */
#include "../../../include/esp3d_config.h"

#if COMMUNICATION_PROTOCOL == RAW_SERIAL && \
    (defined(ARDUINO_ARCH_ESP8266) || defined(ARDUINO_ARCH_ESP32))

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../../../core/esp3d_log.h"
#include "../../bft/binary_file_transfer_service.h"
#include "octoprint-upload.h"

namespace {
constexpr size_t kReadBufferSize = 512;
constexpr size_t kHeaderLineCapacity = 384;
constexpr size_t kBoundaryCapacity = 96;
constexpr size_t kDelimiterCapacity = kBoundaryCapacity + 4;
constexpr size_t kFilenameCapacity = 120;
constexpr size_t kFieldValueCapacity = 16;
constexpr uint32_t kHttpBodyTimeout = 5000;
constexpr uint32_t kHttpUploadTimeout = 30UL * 60 * 1000;

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

bool parseBoundary(const String& contentType, char* boundary) {
  static constexpr char kMultipart[] = "multipart/form-data";
  if (contentType.length() < strlen(kMultipart) ||
      strncasecmp(contentType.c_str(), kMultipart, strlen(kMultipart)) != 0)
    return false;
  const int marker = contentType.indexOf("boundary=");
  if (marker < 0) return false;
  String value = contentType.substring(marker + strlen("boundary="));
  const int separator = value.indexOf(';');
  if (separator >= 0) value = value.substring(0, separator);
  value.trim();
  if (value.startsWith("\"") && value.endsWith("\"") && value.length() >= 2)
    value = value.substring(1, value.length() - 1);
  if (value.length() == 0 || value.length() >= kBoundaryCapacity) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = value.charAt(i);
    if (c < 0x20 || c == 0x7f) return false;
  }
  memcpy(boundary, value.c_str(), value.length() + 1);
  return true;
}

bool isTrue(const char* value) {
  return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
}

bool validFilename(const char* filename) {
  const size_t length = filename ? strlen(filename) : 0;
  if (length == 0 || length > kFilenameCapacity || strstr(filename, ".."))
    return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(filename);
       *p; ++p) {
    if (*p < 0x20 || *p == 0x7f || strchr("/\\:*?\"<>|", *p)) return false;
  }
  return true;
}

// This is the former BinaryFileTransferHttp upload loop, kept independent of
// HTTP framing. Multipart parsing may decide which bytes are file data, but it
// must not change BFT ownership, keepalive, write, close, or recovery rules.
class BftUploadSink {
 public:
  bool start(const char* filename) {
    _busy = _failed = false;
    if (BinaryFileTransferService::active()) {
      _busy = true;
      return false;
    }
    _ownsTransfer = true;
    if (!BinaryFileTransferService::begin(filename)) {
      _failed = true;
      BinaryFileTransferService::abort();
      return false;
    }
    return true;
  }

  bool idle() {
    if (!_ownsTransfer) return true;
    if (BinaryFileTransferService::idle()) return true;
    _failed = true;
    return false;
  }

  bool write(const uint8_t* data, size_t size) {
    if (BinaryFileTransferService::write(data, size)) return true;
    _failed = true;
    return false;
  }

  bool finish() {
    if (!BinaryFileTransferService::finish()) {
      _failed = true;
      return false;
    }
    _ownsTransfer = false;
    return true;
  }

  void abort() {
    if (_ownsTransfer) {
      BinaryFileTransferService::abort();
      _ownsTransfer = false;
    }
  }

  bool busy() const { return _busy; }
  bool failed() const { return _failed; }
  const char* error() const {
    return _failed ? BinaryFileTransferService::error() : nullptr;
  }

 private:
  bool _ownsTransfer = false;
  bool _busy = false;
  bool _failed = false;
};

// The parser stores only a boundary-sized look-behind. Bytes are released to
// BFT only after they can no longer be part of a multipart delimiter.
class MultipartUpload {
 public:
  bool begin(const char* boundary) {
    reset();
    if (!boundary || !*boundary || strlen(boundary) >= kBoundaryCapacity) {
      fail("Invalid multipart boundary");
      return false;
    }
    strcpy(_boundary, boundary);
    snprintf(_opening, sizeof(_opening), "--%s", _boundary);
    snprintf(_delimiter, sizeof(_delimiter), "\r\n--%s", _boundary);
    _delimiterLength = strlen(_delimiter);
    _state = State::Preamble;
    return true;
  }

  void reject(const char* error) {
    reset();
    fail(error);
  }

  void rejectBusy() {
    reset();
    _busy = true;
    fail("BFT transfer is already active");
  }

  bool feed(const uint8_t* data, size_t size) {
    while (size-- && !_failed) feedByte(*data++);
    return !_failed;
  }

  bool idle() {
    return _sink.idle();
  }

  bool finalize() {
    if (_failed) return false;
    if (!_fileSeen) {
      fail("Multipart request has no file part");
      return false;
    }
    if (!_fileFinished) {
      fail("Multipart file part did not finish");
      return false;
    }
    if (!_complete) {
      fail("Multipart closing boundary is missing");
      return false;
    }
    if (_print) {
      esp3d_log("[OCTO] starting print");
      if (!BinaryFileTransferService::startPrint(_filename)) {
        _bftFailure = true;
        fail("Unable to start print");
        return false;
      }
    }
    return true;
  }

  void abort() {
    _sink.abort();
  }

  bool failed() const { return _failed; }
  bool busy() const { return _busy; }
  bool complete() const { return _complete; }
  const char* filename() const { return _filename; }
  uint32_t received() const { return _received; }
  const char* error() const {
    const char* bftError = _sink.error();
    if (!bftError && _bftFailure) bftError = BinaryFileTransferService::error();
    return bftError && *bftError ? bftError : _error;
  }

 private:
  enum class State : uint8_t { Preamble, Headers, Data, BoundaryTail, End };
  enum class Part : uint8_t { Ignore, File, Print, Select };

  void reset() {
    _state = State::Preamble;
    _part = Part::Ignore;
    _failed = _busy = _complete = _fileSeen = _fileFinished = false;
    _bftFailure = _print = _select = false;
    _error = "Malformed multipart request";
    _boundary[0] = _opening[0] = _delimiter[0] = '\0';
    _delimiterLength = _lineLength = _tailSize = _boundaryTailLength = 0;
    _outputLength = _fieldLength = _received = 0;
    _disposition[0] = _filename[0] = _fieldValue[0] = '\0';
  }

  void fail(const char* error) {
    if (!_failed) _error = error;
    _failed = true;
    abort();
  }

  void feedByte(uint8_t value) {
    switch (_state) {
      case State::Preamble:
      case State::Headers:
        lineByte(value);
        break;
      case State::Data:
        dataByte(value);
        break;
      case State::BoundaryTail:
        boundaryTailByte(value);
        break;
      case State::End:
        // RFC 2046 permits the final CRLF, and no payload follows it.
        if (value != '\r' && value != '\n') fail("Invalid multipart trailer");
        break;
    }
  }

  void lineByte(uint8_t value) {
    if (value == '\n') {
      if (_lineLength && _line[_lineLength - 1] == '\r') --_lineLength;
      _line[_lineLength] = '\0';
      lineComplete();
      _lineLength = 0;
      return;
    }
    if (_lineLength + 1 >= sizeof(_line)) {
      fail("Multipart headers are too large");
      return;
    }
    _line[_lineLength++] = static_cast<char>(value);
  }

  void lineComplete() {
    if (_state == State::Preamble) {
      if (strcmp(_line, _opening) != 0)
        fail("Malformed multipart preamble");
      else
        _state = State::Headers;
      return;
    }
    if (_lineLength == 0) {
      startPart();
      return;
    }
    static constexpr char kDisposition[] = "Content-Disposition:";
    if (strncasecmp(_line, kDisposition, strlen(kDisposition)) == 0) {
      const char* value = _line + strlen(kDisposition);
      while (*value == ' ' || *value == '\t') ++value;
      if (strlen(value) >= sizeof(_disposition)) {
        fail("Multipart disposition is too large");
        return;
      }
      strcpy(_disposition, value);
    }
  }

  static bool attribute(const char* input, const char* name, char* output,
                        size_t capacity) {
    const size_t nameLength = strlen(name);
    for (const char* p = input; *p;) {
      while (*p == ';' || *p == ' ' || *p == '\t') ++p;
      if (strncasecmp(p, name, nameLength) != 0) {
        p = strchr(p, ';');
        if (!p) return false;
        continue;
      }
      p += nameLength;
      while (*p == ' ' || *p == '\t') ++p;
      if (*p != '=') {
        p = strchr(p, ';');
        if (!p) return false;
        continue;
      }
      ++p;
      while (*p == ' ' || *p == '\t') ++p;
      const bool quoted = *p == '\"';
      if (quoted) ++p;
      size_t length = 0;
      while (*p && ((quoted && *p != '\"') || (!quoted && *p != ';'))) {
        if (length + 1 >= capacity) return false;
        output[length++] = *p++;
      }
      if (quoted && *p != '\"') return false;
      output[length] = '\0';
      return true;
    }
    return false;
  }

  void startPart() {
    char name[16] = {};
    char filename[kFilenameCapacity + 1] = {};
    _part = Part::Ignore;
    _fieldLength = 0;
    if (!attribute(_disposition, "name", name, sizeof(name))) {
      fail("Multipart part has no name");
      return;
    }
    if (strcmp(name, "file") == 0) {
      if (_fileSeen || !attribute(_disposition, "filename", filename,
                                  sizeof(filename)) ||
          !validFilename(filename)) {
        fail("Invalid upload filename");
        return;
      }
      _fileSeen = true;
      for (char* c = filename; *c; ++c) {
        if (*c == ',') *c = '_';
      }
      strcpy(_filename, filename);
      esp3d_log("[OCTO] filename=%s", _filename);
      if (!_sink.start(_filename)) {
        _busy = _sink.busy();
        _bftFailure = _sink.failed();
        fail("Unable to start BFT upload");
        return;
      }
      esp3d_log("[OCTO] upload started");
      _part = Part::File;
    } else if (strcmp(name, "print") == 0) {
      _part = Part::Print;
    } else if (strcmp(name, "select") == 0) {
      _part = Part::Select;
    }
    _disposition[0] = '\0';
    _state = State::Data;
  }

  void dataByte(uint8_t value) {
    if (_tailSize >= sizeof(_tail)) {
      fail("Multipart boundary is too long");
      return;
    }
    _tail[_tailSize++] = value;
    if (_tailSize > _delimiterLength) {
      emitByte(_tail[0]);
      memmove(_tail, _tail + 1, --_tailSize);
    }
    // A delimiter may become aligned only after releasing the oldest byte.
    // Check after the shift as well, otherwise its final byte is followed by
    // the boundary suffix before it is recognized.
    if (_tailSize == _delimiterLength &&
        memcmp(_tail, _delimiter, _delimiterLength) == 0) {
      _tailSize = 0;
      endPart();
    }
  }

  void emitByte(uint8_t value) {
    if (_part == Part::File) {
      _output[_outputLength++] = value;
      if (_outputLength == sizeof(_output)) flushOutput();
    } else if (_part == Part::Print || _part == Part::Select) {
      if (_fieldLength + 1 >= sizeof(_fieldValue)) {
        fail("Multipart option is too large");
        return;
      }
      _fieldValue[_fieldLength++] = static_cast<char>(value);
    }
  }

  void flushOutput() {
    if (_outputLength == 0 || _failed) return;
    if (!_sink.write(_output, _outputLength)) {
      _bftFailure = true;
      fail("BFT write failed");
      return;
    }
    _received += _outputLength;
    if ((_received & 0xffff) < _outputLength)
      esp3d_log("[OCTO] received=%lu", static_cast<unsigned long>(_received));
    _outputLength = 0;
  }

  void endPart() {
    if (_part == Part::File) {
      flushOutput();
      if (_failed) return;
      if (!_sink.finish()) {
        _bftFailure = true;
        fail("BFT finish failed");
        return;
      }
      _fileFinished = true;
      esp3d_log("[OCTO] upload finished bytes=%lu",
                static_cast<unsigned long>(_received));
    } else if (_part == Part::Print) {
      _fieldValue[_fieldLength] = '\0';
      _print = isTrue(_fieldValue);
      esp3d_log("[OCTO] print=%s", _print ? "true" : "false");
    } else if (_part == Part::Select) {
      _fieldValue[_fieldLength] = '\0';
      _select = isTrue(_fieldValue);
    }
    _part = Part::Ignore;
    _boundaryTailLength = 0;
    _state = State::BoundaryTail;
  }

  void boundaryTailByte(uint8_t value) {
    if (_boundaryTailLength == 0) {
      if (value != '-' && value != '\r') {
        fail("Malformed multipart boundary");
        return;
      }
      _boundaryTail[0] = value;
      _boundaryTailLength = 1;
      return;
    }
    if (_boundaryTail[0] == '-' && value == '-') {
      _complete = true;
      _state = State::End;
    } else if (_boundaryTail[0] == '\r' && value == '\n') {
      _disposition[0] = '\0';
      _state = State::Headers;
    } else {
      fail("Malformed multipart boundary");
    }
  }

  State _state = State::Preamble;
  Part _part = Part::Ignore;
  bool _failed = false;
  bool _busy = false;
  bool _complete = false;
  bool _fileSeen = false;
  bool _fileFinished = false;
  bool _bftFailure = false;
  bool _print = false;
  bool _select = false;
  const char* _error = "Malformed multipart request";
  char _boundary[kBoundaryCapacity] = {};
  char _opening[kBoundaryCapacity + 3] = {};
  char _delimiter[kDelimiterCapacity] = {};
  size_t _delimiterLength = 0;
  char _line[kHeaderLineCapacity] = {};
  size_t _lineLength = 0;
  char _disposition[kHeaderLineCapacity] = {};
  char _filename[kFilenameCapacity + 1] = {};
  uint8_t _tail[kDelimiterCapacity + 1] = {};
  size_t _tailSize = 0;
  char _boundaryTail[1] = {};
  size_t _boundaryTailLength = 0;
  uint8_t _output[kReadBufferSize] = {};
  size_t _outputLength = 0;
  char _fieldValue[kFieldValueCapacity] = {};
  size_t _fieldLength = 0;
  uint32_t _received = 0;
  BftUploadSink _sink;
};

String jsonError(const char* error) {
  String body = "{\"error\":\"";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(error);
       p && *p; ++p) {
    const char value = *p >= 0x20 && *p <= 0x7e && *p != '\"' && *p != '\\'
                           ? static_cast<char>(*p)
                           : '?';
    body += value;
  }
  body += "\"}";
  return body;
}

}  // namespace

#if defined(ARDUINO_ARCH_ESP8266)

#include <ESP8266WebServer.h>

ESP8266WebServer* octoServer = nullptr;
// ESP8266 has a 4 KiB continuation stack. MultipartUpload plus the two input
// buffers needs about 2.7 KiB, leaving too little stack for synchronous BFT
// calls and their UART/Wi-Fi internals. The web server processes one request
// at a time, so persistent storage is safe here; begin() resets it per request.
MultipartUpload esp8266Upload;
uint8_t esp8266UploadBuffer[kReadBufferSize] = {};
char esp8266UploadBoundary[kBoundaryCapacity] = {};

void sendJson(WiFiClient* client, uint16_t status, const String& body) {
  const char* reason = status == 200   ? "OK"
                       : status == 201 ? "Created"
                       : status == 400 ? "Bad Request"
                       : status == 408 ? "Request Timeout"
                       : status == 409 ? "Conflict"
                                       : "Internal Server Error";
  client->printf(
      "HTTP/1.1 %u %s\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %u\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-cache\r\n\r\n%s",
      status, reason, static_cast<unsigned>(body.length()), body.c_str());
}

ESP8266WebServer::ClientFuture handleUpload(const String& method,
                                            const String& url,
                                            WiFiClient* client) {
  const int query = url.indexOf('?');
  const String path = url.substring(0, query < 0 ? url.length() : query);
  if (method != "POST" || path != "/api/files/local")
    return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;

  String contentLength;
  String contentType;
  bool chunked = false;
  client->setTimeout(kHttpBodyTimeout);
  while (client->connected()) {
    String line = client->readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    const int separator = line.indexOf(':');
    if (separator <= 0) {
      sendJson(client, 400, jsonError("Malformed HTTP headers"));
      return ESP8266WebServer::CLIENT_IS_GIVEN;
    }
    const String name = line.substring(0, separator);
    String value = line.substring(separator + 1);
    value.trim();
    if (name.equalsIgnoreCase("Content-Length"))
      contentLength = value;
    else if (name.equalsIgnoreCase("Content-Type"))
      contentType = value;
    else if (name.equalsIgnoreCase("Transfer-Encoding") &&
             value.indexOf("chunked") >= 0)
      chunked = true;
  }

  uint32_t remaining = 0;
  char* const boundary = esp8266UploadBoundary;
  if (chunked || !parseContentLength(contentLength, &remaining) ||
      !parseBoundary(contentType, boundary)) {
    sendJson(client, 400, jsonError("Expected multipart/form-data request"));
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }
  if (BinaryFileTransferService::active()) {
    sendJson(client, 409, jsonError("BFT transfer is already active"));
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }

  esp3d_log("[OCTO] POST /api/files/local");
  MultipartUpload& upload = esp8266Upload;
  upload.begin(boundary);
  uint8_t* const buffer = esp8266UploadBuffer;
  uint32_t lastData = millis();
  const uint32_t started = lastData;
  bool timeout = false;
  while (!upload.failed() && remaining > 0) {
    if (!upload.idle() || millis() - started >= kHttpUploadTimeout) {
      timeout = true;
      break;
    }
    const int available = client->available();
    if (available > 0) {
      size_t wanted =
          remaining > kReadBufferSize ? kReadBufferSize : remaining;
      if (wanted > static_cast<size_t>(available)) wanted = available;
      const int received = client->read(buffer, wanted);
      if (received <= 0 || !upload.feed(buffer, static_cast<size_t>(received)))
        break;
      remaining -= received;
      lastData = millis();
    } else if (!client->connected() || millis() - lastData >= kHttpBodyTimeout) {
      timeout = true;
      break;
    }
    ESP3DHal::wait(1);
  }
  const bool success = !timeout && remaining == 0 && upload.finalize();
  if (!success) upload.abort();
  if (success) {
    String body = "{\"done\":true,\"files\":{\"local\":{\"name\":\"";
    body += upload.filename();
    body += "\",\"path\":\"";
    body += upload.filename();
    body += "\",\"origin\":\"local\"}}}";
    sendJson(client, 201, body);
  } else {
    const uint16_t status = upload.busy() ? 409 : timeout ? 408 : 400;
    esp3d_log_e("[OCTO] upload failed: %s", upload.error());
    sendJson(client, status, jsonError(upload.error()));
  }
  return ESP8266WebServer::CLIENT_IS_GIVEN;
}

void handleVersion() {
  esp3d_log("[OCTO] GET /api/version");
  octoServer->send(200, "application/json",
                   "{\"api\":\"0.1\",\"server\":\"1.11.0\","
                   "\"text\":\"OctoPrint 1.11.0\"}");
}

void installOctoPrintUpload(ESP8266WebServer* server) {
  octoServer = server;
  server->on("/api/version", HTTP_GET, handleVersion);
  server->addHook([](const String& method, const String& url,
                     WiFiClient* client,
                     ESP8266WebServer::ContentTypeFunction) {
    return handleUpload(method, url, client);
  });
}

#elif defined(ARDUINO_ARCH_ESP32)

#include <WebServer.h>

WebServer* octoServer = nullptr;
// WebServer parses multipart bodies itself and never invokes RequestHandler::raw
// for them. Use its streaming HTTPUpload notifications on ESP32 instead.
BftUploadSink octoUpload;
bool requestActive = false;
bool requestSuccessful = false;
bool requestFailed = false;
bool requestBusy = false;
uint32_t requestStartTime = 0;
String requestFilename;

bool requestPrintsAfterUpload() {
  return octoServer->hasArg("print") &&
         (octoServer->arg("print") == "1" ||
          octoServer->arg("print").equalsIgnoreCase("true"));
}

void failUpload() {
  requestFailed = true;
  requestActive = false;
  octoUpload.abort();
}

void handleUploadBody() {
  HTTPUpload& upload = octoServer->upload();
  if (upload.status == UPLOAD_FILE_START) {
    requestActive = requestSuccessful = requestFailed = false;
    requestBusy = false;
    requestFilename = upload.filename;
    requestFilename.replace(",", "_");
    requestStartTime = millis();
    esp3d_log("[OCTO] POST /api/files/local filename=%s",
              requestFilename.c_str());
    if (upload.name != "file" || !validFilename(requestFilename.c_str())) {
      requestFailed = true;
      return;
    }
    if (!octoUpload.start(requestFilename.c_str())) {
      requestBusy = octoUpload.busy();
      failUpload();
      return;
    }
    requestActive = true;
    esp3d_log("[OCTO] upload started");
    return;
  }
  if (upload.status == UPLOAD_FILE_WRITE) {
    if (requestActive &&
        (millis() - requestStartTime >= kHttpUploadTimeout ||
         !octoUpload.idle() ||
         !octoUpload.write(upload.buf, upload.currentSize)))
      failUpload();
    return;
  }
  if (upload.status == UPLOAD_FILE_END) {
    if (requestActive && millis() - requestStartTime < kHttpUploadTimeout &&
        octoUpload.finish()) {
      requestSuccessful = true;
      requestActive = false;
      esp3d_log("[OCTO] upload finished bytes=%lu",
                static_cast<unsigned long>(upload.totalSize));
    } else {
      failUpload();
    }
    return;
  }
  if (upload.status == UPLOAD_FILE_ABORTED) failUpload();
}

void sendUploadResult() {
  bool success = requestSuccessful && !requestFailed;
  if (success && requestPrintsAfterUpload()) {
    esp3d_log("[OCTO] starting print");
    success = BinaryFileTransferService::startPrint(requestFilename.c_str());
  }
  if (!success) octoUpload.abort();
  if (success) {
    String body = "{\"done\":true,\"files\":{\"local\":{\"name\":\"";
    body += requestFilename;
    body += "\",\"path\":\"";
    body += requestFilename;
    body += "\",\"origin\":\"local\"}}}";
    octoServer->send(201, "application/json", body);
  } else {
    const char* error = octoUpload.error();
    if (!error || !*error)
      error = requestActive ? "Upload timed out" : "Multipart upload failed";
    const uint16_t status = requestBusy ? 409 : 400;
    esp3d_log_e("[OCTO] upload failed: %s", error);
    octoServer->send(status, "application/json", jsonError(error));
  }
}

void handleVersion() {
  esp3d_log("[OCTO] GET /api/version");
  octoServer->send(200, "application/json",
                   "{\"api\":\"0.1\",\"server\":\"1.11.0\","
                   "\"text\":\"OctoPrint 1.11.0\"}");
}

void installOctoPrintUpload(WebServer* server) {
  octoServer = server;
  server->on("/api/version", HTTP_GET, handleVersion);
  server->on("/api/files/local", HTTP_POST, sendUploadResult,
             handleUploadBody);
}

#endif  // platform
#endif  // RAW_SERIAL && supported HTTP server
