/* Marlin Binary File Transfer 0.1.0 client with static heatshrink compression. */
#include "../../include/esp3d_config.h"
#if COMMUNICATION_PROTOCOL == RAW_SERIAL
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../serial/serial_service.h"
#include "binary_file_transfer_service.h"
extern "C" {
#include "heatshrink/heatshrink_encoder.h"
}

namespace {
static_assert(!HEATSHRINK_DYNAMIC_ALLOC, "BFT encoder must use static memory");
static_assert(HEATSHRINK_STATIC_WINDOW_BITS == 8 &&
                  HEATSHRINK_STATIC_LOOKAHEAD_BITS == 4,
              "Update BFT capability matching when changing compression");
heatshrink_encoder encoder;
constexpr uint32_t kResponseTimeout = 5000;
constexpr uint32_t kAckTimeout = 1000;
constexpr uint32_t kKeepaliveInterval = 3000;
// Below Marlin's 10-second idle timeout. Never delete after ownership expires.
constexpr uint32_t kOwnershipTimeout = 8000;
constexpr uint8_t kMaxRetries = 2;
#if defined(ARDUINO_ARCH_ESP32)
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
void lockRx() { portENTER_CRITICAL(&rxMux); }
void unlockRx() { portEXIT_CRITICAL(&rxMux); }
#else
void lockRx() { noInterrupts(); }
void unlockRx() { interrupts(); }
#endif
bool startsWith(const char* value, const char* prefix) {
  return strncmp(value, prefix, strlen(prefix)) == 0;
}
bool number(const char*& p, unsigned long& value, unsigned long limit) {
  if (*p < '0' || *p > '9') return false;
  value = 0;
  do {
    const unsigned digit = *p++ - '0';
    if (value > (limit - digit) / 10) return false;
    value = value * 10 + digit;
  } while (*p >= '0' && *p <= '9');
  return value <= limit;
}
bool sequence(const char* p, uint8_t& value) {
  unsigned long n;
  if (!number(p, n, 255) || *p) return false;
  value = static_cast<uint8_t>(n);
  return true;
}
}  // namespace

bool BinaryFileTransferService::_active = false;
uint8_t BinaryFileTransferService::_rx[kRxCapacity] = {};
size_t BinaryFileTransferService::_head = 0,
       BinaryFileTransferService::_tail = 0;
bool BinaryFileTransferService::_overflow = false;
BinaryFileTransferService::Mode BinaryFileTransferService::_mode = Mode::Ascii;
BinaryFileTransferService::File BinaryFileTransferService::_file = File::None;
bool BinaryFileTransferService::_failed = false;
bool BinaryFileTransferService::_cleanup = false;
bool BinaryFileTransferService::_transportReady = false;
bool BinaryFileTransferService::_compression = false;
char BinaryFileTransferService::_error[128] = {};
char BinaryFileTransferService::_line[kLineCapacity] = {};
size_t BinaryFileTransferService::_lineSize = 0;
bool BinaryFileTransferService::_discardLine = false;
uint8_t BinaryFileTransferService::_sync = 0,
        BinaryFileTransferService::_pendingSync = 0;
uint16_t BinaryFileTransferService::_payloadSize = 0;
uint8_t BinaryFileTransferService::_packet[kPacketPayloadCapacity + 10] = {};
uint8_t BinaryFileTransferService::_encoderOutput[kPacketPayloadCapacity] = {};
bool BinaryFileTransferService::_waiting = false,
     BinaryFileTransferService::_acked = false;
bool BinaryFileTransferService::_resend = false,
     BinaryFileTransferService::_waitingSync = false;
bool BinaryFileTransferService::_gotSync = false;
BinaryFileTransferService::Reply BinaryFileTransferService::_expected =
    Reply::None;
BinaryFileTransferService::Reply BinaryFileTransferService::_reply =
    Reply::None;
BinaryFileTransferService::AsciiReply
    BinaryFileTransferService::_asciiExpected = AsciiReply::None;
bool BinaryFileTransferService::_asciiMarker = false,
     BinaryFileTransferService::_asciiOk = false;
bool BinaryFileTransferService::_unsupported = false;
uint32_t BinaryFileTransferService::_lastFileActivity = 0;
size_t BinaryFileTransferService::_writeBytesSinceBarrier = 0;

bool BinaryFileTransferService::active() {
  lockRx();
  const bool result = _active;
  unlockRx();
  return result;
}
void BinaryFileTransferService::fail(const char* message) {
  // Keep the original cause, even when cleanup subsequently fails.
  if (!_failed) {
    strncpy(_error, message, sizeof(_error) - 1);
    _error[sizeof(_error) - 1] = '\0';
  }
  _failed = true;
}
void BinaryFileTransferService::receiveByte(uint8_t value) {
  lockRx();
  if (_active) {
    const size_t next = (_head + 1) % kRxCapacity;
    if (next == _tail)
      _overflow = true;
    else {
      _rx[_head] = value;
      _head = next;
    }
  }
  unlockRx();
}
void BinaryFileTransferService::poll() {
  if (!active()) return;
  // Parse as we read, instead of first filling a small queue of whole lines.
  for (size_t count = 0; count < kRxCapacity * 2; ++count) {
    int value = -1;
    lockRx();
    const bool overflow = _overflow;
    if (_tail != _head) {
      value = _rx[_tail];
      _tail = (_tail + 1) % kRxCapacity;
    }
    unlockRx();
    if (overflow) {
      fail("BFT receive overflow; result unknown");
      _transportReady = false;
      _file = File::Unknown;
      _mode = Mode::Unknown;
      return;
    }
#if defined(ARDUINO_ARCH_ESP8266)
    if (value < 0 && esp3d_serial_service.available() > 0)
      value = esp3d_serial_service.read();
#endif
    if (value < 0) break;
    if (value == '\n') {
      if (!_discardLine && _lineSize) {
        if (_line[_lineSize - 1] == '\r') --_lineSize;
        _line[_lineSize] = '\0';
        lineReceived();
      }
      _lineSize = 0;
      _discardLine = false;
    } else if (!_discardLine) {
      if (value == 0 || _lineSize == sizeof(_line) - 1) {
        // Never accept the tail of a diagnostic as a fresh ACK.
        _discardLine = true;
        _line[_lineSize] = '\0';
        if (startsWith(_line, "PFT:") || startsWith(_line, "PTF:") ||
            startsWith(_line, "ok") || startsWith(_line, "ss") ||
            startsWith(_line, "rs") || startsWith(_line, "fe")) {
          fail("Malformed BFT response; result unknown");
          _transportReady = false;
        }
      } else
        _line[_lineSize++] = static_cast<char>(value);
    }
  }
}
void BinaryFileTransferService::lineReceived() {
  if (_asciiExpected != AsciiReply::None) {
    if (_asciiExpected == AsciiReply::Firmware &&
        startsWith(_line, "FIRMWARE_NAME:"))
      _asciiMarker = true;
    if (_asciiExpected == AsciiReply::Switch &&
        strcmp(_line, "echo:Switching to Binary Protocol") == 0)
      _asciiMarker = true;
    if (_asciiExpected == AsciiReply::FileSelected &&
        startsWith(_line, "File selected"))
      _asciiMarker = true;
    if (strcmp(_line, "Cap:BINARY_FILE_TRANSFER:0") == 0 ||
        strcmp(_line, "Cap:SD_WRITE:0") == 0)
      _unsupported = true;
    // Ignore leftover ACKs until the distinctive response to our command.
    if ((_asciiExpected == AsciiReply::Ok &&
         (strcmp(_line, "ok") == 0 || startsWith(_line, "ok "))) ||
        (_asciiMarker &&
         (strcmp(_line, "ok") == 0 || startsWith(_line, "ok "))))
      _asciiOk = true;
    return;
  }
  if (_mode != Mode::Binary && _mode != Mode::Leaving) return;
  if (strcmp(_line, "start") == 0) {
    fail("Printer restarted during BFT");
    _transportReady = false;
    _file = File::Unknown;
    _mode = Mode::Unknown;
    return;
  }
  uint8_t n;
  if (startsWith(_line, "ok") && sequence(_line + 2, n)) {
    if (_waiting && n == _pendingSync && !_acked) {
      _acked = true;
      _sync = static_cast<uint8_t>(n + 1);
      if ((_packet[3] >> 4) == 1) {
        if (_file == File::Open &&
            millis() - _lastFileActivity >= kOwnershipTimeout) {
          _file = File::Unknown;
          fail("BFT file ownership expired before acknowledgement");
        }
        _lastFileActivity = millis();
      }
    }
    // Late duplicate ACKs never advance the sequence again.
    return;
  }
  if (startsWith(_line, "rs") && sequence(_line + 2, n)) {
    if (_waiting && !_acked && n == _pendingSync)
      _resend = true;
    else {
      fail("BFT sequence mismatch; synchronization required");
      _transportReady = false;
    }
    return;
  }
  if (startsWith(_line, "fe") && sequence(_line + 2, n)) {
    _sync = 0;
    _transportReady = false;
    fail("Marlin BFT fatal transport error");
    return;
  }
  if (startsWith(_line, "ss")) {
    const char* p = _line + 2;
    unsigned long sync, size;
    if (!_waitingSync || !number(p, sync, 255) || *p++ != ',' ||
        !number(p, size, UINT16_MAX) || !size || *p++ != ',' ||
        strcmp(p, "0.1.0") != 0) {
      fail("Invalid or unsupported BFT SYNC response");
      _transportReady = false;
      return;
    }
    _sync = static_cast<uint8_t>(sync);
    _payloadSize =
        size > kPacketPayloadCapacity ? kPacketPayloadCapacity : size;
    _gotSync = true;
    return;
  }
  Reply result = Reply::None;
  if (strcmp(_line, "PFT:success") == 0)
    result = Reply::Success;
  else if (startsWith(_line, "PFT:version:")) {
    if (!startsWith(_line, "PFT:version:0.1.0:compression:")) {
      fail("Unsupported Marlin file transfer version");
      _transportReady = false;
      return;
    }
    // Negotiate only before OPEN. Keepalive QUERY must not change an active
    // stream's encoding, even if the peer advertises different capabilities.
    if (_waiting && _expected == Reply::Version && _file == File::None)
      _compression =
          strcmp(_line, "PFT:version:0.1.0:compression:heatshrink,8,4") == 0;
    result = Reply::Version;
  } else if (strcmp(_line, "PFT:fail") == 0)
    result = Reply::Fail;
  else if (strcmp(_line, "PFT:busy") == 0)
    result = Reply::Busy;
  else if (strcmp(_line, "PFT:invalid") == 0)
    result = Reply::Invalid;
  else if (strcmp(_line, "PTF:invalid") == 0) {
    // Marlin's typo means an unknown file command, not an inactive file.
    fail(_line);
    _transportReady = false;
    _file = File::Unknown;
    return;
  } else if (strcmp(_line, "PFT:ioerror") == 0)
    result = Reply::IoError;
  if (result == Reply::None) return;  // SD diagnostics / auto reports
  const bool error = result != Reply::Success && result != Reply::Version;
  if (error) fail(_line);
  // A PFT failure can follow any file command, including QUERY used as the
  // write barrier. Keep that response: otherwise a late SD error is reported
  // misleadingly as a missing result after its ACK was already received.
  if (_waiting && (result == _expected || error)) _reply = result;
  if (result == Reply::Invalid && _file == File::Open) _file = File::None;
}
uint16_t BinaryFileTransferService::fletcher(const uint8_t* data, size_t size) {
  uint16_t a = 0, b = 0;
  for (size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 255;
    b = (b + a) % 255;
  }
  return (b << 8) | a;
}
bool BinaryFileTransferService::writeAll(const uint8_t* data, size_t size) {
  const uint32_t start = millis();
  uint32_t progress = start;
  while (size) {
    const size_t sent = esp3d_serial_service.writeExclusive(data, size);
    data += sent;
    size -= sent;
    if (sent) progress = millis();
    if (!size) return true;
    if (millis() - progress >= 400 || millis() - start >= kResponseTimeout)
      break;
    ESP3DHal::wait(1);
  }
  _transportReady = false;
  fail("Unable to write complete BFT packet");
  return false;
}
bool BinaryFileTransferService::ascii(const char* command,
                                      AsciiReply expected) {
  poll();
  _asciiExpected = expected;
  _asciiMarker = _asciiOk = false;
  const bool written =
      writeAll(reinterpret_cast<const uint8_t*>(command), strlen(command));
  const uint32_t start = millis();
  while (written && millis() - start < kResponseTimeout) {
    poll();
    if (_asciiOk) break;
    ESP3DHal::wait(1);
  }
  _asciiExpected = AsciiReply::None;
  if (!_asciiOk || _mode == Mode::Unknown) {
    const char* step = "command";
    if (expected == AsciiReply::Firmware)
      step = "M115";
    else if (expected == AsciiReply::Switch)
      step = "M28 B1";
    else if (expected == AsciiReply::FileSelected)
      step = "M23";
    char error[64];
    snprintf(error, sizeof(error), "Marlin %s handshake timed out", step);
    fail(error);
    return false;
  }
  return true;
}
bool BinaryFileTransferService::synchronize() {
  if (_cleanup && !_transportReady) {
    // A short UART write may have left Marlin halfway through a frame.
    // Let its 500 ms inter-byte timeout finish before sending a new token.
    const uint32_t start = millis();
    while (millis() - start < 550) {
      poll();
      ESP3DHal::wait(1);
    }
  }
  poll();
  if (_mode != Mode::Binary) return false;
  const uint8_t packet[] = {0xad, 0xb5, 0, 1, 0, 0, 1, 3};
  _waitingSync = true;
  _gotSync = false;
  // Allow cleanup to recover numbering after rs/fe, never the file outcome.
  _transportReady = true;
  const bool written = writeAll(packet, sizeof(packet));
  const uint32_t start = millis();
  while (written && _transportReady && millis() - start < kResponseTimeout) {
    poll();
    if (_gotSync) break;
    ESP3DHal::wait(1);
  }
  _waitingSync = false;
  _transportReady = written && _gotSync && _transportReady;
  if (!_transportReady) fail("Marlin BFT SYNC failed");
  return _transportReady;
}
bool BinaryFileTransferService::sendPacket(uint8_t protocol, uint8_t type,
                                           const uint8_t* payload, size_t size,
                                           Reply expected) {
  poll();
  if ((!_cleanup && _failed) || !_transportReady || !active() ||
      size > _payloadSize || size > kPacketPayloadCapacity ||
      (size && !payload))
    return false;
  _packet[0] = 0xad;
  _packet[1] = 0xb5;
  _packet[2] = _sync;
  _packet[3] = (protocol << 4) | type;
  _packet[4] = size & 0xff;
  _packet[5] = size >> 8;
  uint16_t checksum = fletcher(_packet + 2, 4);
  _packet[6] = checksum & 0xff;
  _packet[7] = checksum >> 8;
  if (size) {
    memcpy(_packet + 8, payload, size);
    checksum = fletcher(_packet + 2, size + 6);
    _packet[8 + size] = checksum & 0xff;
    _packet[9 + size] = checksum >> 8;
  }
  const size_t packetSize = size ? size + 10 : 8;
  // The sample Marlin scans a duplicate's body for tokens. Retrying such a
  // body after a lost ACK could execute embedded packets. Fail conservatively.
  bool retrySafe = _sync != 255 && !(protocol == 0 && type == 2);
  for (size_t i = 8; i + 1 < packetSize; ++i)
    if (_packet[i] == 0xad && _packet[i + 1] == 0xb5) retrySafe = false;
  _pendingSync = _sync;
  _expected = expected;
  auto sendAndWait = [&]() {
    _reply = Reply::None;
    _acked = _resend = false;
    _waiting = true;
    uint8_t retries = 0;
    uint32_t sentAt = millis();
    bool sent = writeAll(_packet, packetSize);
    while (sent && _transportReady) {
      poll();
      if (!_transportReady) break;
      if (_acked && (expected == Reply::None || _reply != Reply::None)) break;
      const uint32_t elapsed = millis() - sentAt;
      if (_acked) {
        // Duplicate packets cannot recover a lost PFT result.
        if (elapsed >= kResponseTimeout) break;
      } else if (_resend || elapsed >= kAckTimeout) {
        // rs of the current sequence explicitly says the packet was not
        // accepted.
        if (retries == kMaxRetries || (!_resend && !retrySafe)) break;
        ++retries;
        _resend = false;
        sentAt = millis();
        sent = writeAll(_packet, packetSize);
      }
      ESP3DHal::wait(1);
    }
    _waiting = false;
    return sent;
  };
  bool sent = sendAndWait();
  if (!_acked) {
    // A SYNC response contains Marlin's current sequence number. It is a
    // safe recovery point after an ACK loss: nextSync proves the packet was
    // accepted, while pendingSync proves it was not and can be sent again.
    const uint8_t nextSync = static_cast<uint8_t>(_pendingSync + 1);
    if (expected != Reply::None && _reply == expected && synchronize() &&
        _sync == nextSync)
      return true;
    if (protocol == 1 && type == 3) {
      for (uint8_t recovery = 0;
           !_acked && recovery < kMaxRetries && !_failed; ++recovery) {
        if (!synchronize()) break;
        if (_sync == nextSync) return true;
        if (_sync != _pendingSync) break;
        sent = sendAndWait();
      }
    }
  }
  if (!_acked) {
    _transportReady = false;
    char error[96];
    snprintf(error, sizeof(error),
             "BFT acknowledgement missing for packet %u, sync %u; result "
             "unknown",
             static_cast<unsigned>((protocol << 4) | type),
             static_cast<unsigned>(_pendingSync));
    fail(error);
    return false;
  }
  if (expected != Reply::None && _reply == Reply::None) {
    fail("BFT file result missing; result unknown");
    return false;
  }
  return sent && _transportReady && (_reply == expected) &&
         (!_failed || _cleanup);
}
bool BinaryFileTransferService::query() {
  return sendPacket(1, 0, nullptr, 0, Reply::Version);
}
void BinaryFileTransferService::release() {
  lockRx();
  _active = false;
  unlockRx();
  esp3d_serial_service.releaseExclusive();
}
bool BinaryFileTransferService::begin(const char* filename) {
  // A second caller must never abort or reset another caller's transfer.
  if (active()) return false;
  _failed = _cleanup = false;
  _compression = false;
  _writeBytesSinceBarrier = 0;
  heatshrink_encoder_reset(&encoder);
  _error[0] = '\0';
  if (!filename || !*filename || strlen(filename) > kFilenameCapacity ||
      strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
    fail("Invalid BFT filename");
    return false;
  }
  for (const unsigned char* p =
           reinterpret_cast<const unsigned char*>(filename);
       *p; ++p) {
    if (*p < 0x20 || *p == 0x7f || strchr("/\\:*?\"<>|", *p)) {
      fail("BFT requires a filename in the SD root");
      return false;
    }
  }
  if (!esp3d_serial_service.claimExclusive()) {
    fail("Printer UART is busy");
    return false;
  }
  _mode = Mode::Ascii;
  _file = File::None;
  _transportReady = _waiting = _waitingSync = false;
  _asciiExpected = AsciiReply::None;
  _unsupported = _discardLine = false;
  _lineSize = 0;
  lockRx();
  _head = _tail = 0;
  _overflow = false;
  _active = true;
  unlockRx();
  esp3d_serial_service.flush();
  // M115's distinctive reply and final ok form a barrier after prior G-code.
  if (!ascii("M115\n", AsciiReply::Firmware) || _unsupported || _failed) {
    if (_unsupported) fail("Printer does not support BFT/SD writing");
    if (_mode == Mode::Ascii) release();
    return false;
  }
  _mode = Mode::Entering;
  if (!ascii("M28 B1\n", AsciiReply::Switch) || _failed) {
    _mode =
        Mode::Unknown;  // Do not send binary cleanup into unconfirmed ASCII.
    return false;
  }
  _mode = Mode::Binary;
  if (!synchronize() || !query()) return false;
  const size_t length = strlen(filename);
  if (length + 3 > _payloadSize) {
    fail("Filename exceeds Marlin BFT buffer");
    return false;
  }
  uint8_t payload[kFilenameCapacity + 3] = {};  // dummy=0
  payload[1] = _compression ? 1 : 0;
  memcpy(payload + 2, filename, length + 1);
  _file = File::Opening;
  const bool opened = sendPacket(1, 1, payload, length + 3, Reply::Success);
  if (_reply == Reply::Success)
    _file = File::Open;
  else if (_reply == Reply::Fail)
    _file = File::None;
  else if (_reply == Reply::Busy)
    _file = File::Foreign;
  else
    _file = File::Unknown;
  return opened;
}
bool BinaryFileTransferService::idle() {
  poll();
  if (_failed || !active() || _file != File::Open) return false;
  const uint32_t age = millis() - _lastFileActivity;
  if (age >= kOwnershipTimeout) {
    _file = File::Unknown;
    fail("BFT file ownership expired");
    return false;
  }
  return age < kKeepaliveInterval || query();
}
bool BinaryFileTransferService::write(const uint8_t* data, size_t size) {
  if (!idle() || (size && !data)) {
    fail("No writable BFT file");
    return false;
  }
  if (!_compression) return writePayload(data, size);
  while (size) {
    size_t consumed = 0;
    // heatshrink's sink API is non-const, but only copies the input bytes.
    if (heatshrink_encoder_sink(&encoder, const_cast<uint8_t*>(data), size,
                               &consumed) != HSER_SINK_OK) {
      fail("BFT compression input failed");
      return false;
    }
    data += consumed;
    size -= consumed;
    if (!drainEncoder()) return false;
  }
  return true;
}
bool BinaryFileTransferService::drainEncoder() {
  HSE_poll_res result;
  do {
    size_t produced = 0;
    result = heatshrink_encoder_poll(&encoder, _encoderOutput, _payloadSize,
                                    &produced);
    if (result < 0) {
      fail("BFT compression output failed");
      return false;
    }
    if (produced && !writePayload(_encoderOutput, produced)) return false;
  } while (result == HSER_POLL_MORE);
  return true;
}
bool BinaryFileTransferService::writePayload(const uint8_t* data, size_t size) {
  while (size) {
    const size_t chunk = size > _payloadSize ? _payloadSize : size;
    if (!sendPacket(1, 3, data, chunk)) return false;
    _writeBytesSinceBarrier += chunk;
    // ACK precedes the SD write, but poll() runs before and while waiting for
    // every following packet ACK, so asynchronous PFT errors still stop the
    // transfer immediately. QUERY is retained as a periodic strict dispatch
    // barrier and again in finish(), without paying its round trip per packet.
    if (_writeBytesSinceBarrier >= kWriteBarrierPayloadBytes) {
      if (!query()) return false;
      _writeBytesSinceBarrier = 0;
    }
    data += chunk;
    size -= chunk;
  }
  return true;
}
bool BinaryFileTransferService::leave() {
  if (_file != File::None || !_transportReady) return false;
  _mode = Mode::Leaving;
  if (!sendPacket(0, 2)) {
    _mode = Mode::Unknown;
    return false;
  }
  // receive() in the supplied Marlin can keep scanning for 20 ms after ACK.
  ESP3DHal::wait(50);
  if (!ascii("M115\n", AsciiReply::Firmware)) {
    _mode = Mode::Unknown;
    return false;
  }
  _mode = Mode::Ascii;
  release();
  return true;
}
bool BinaryFileTransferService::finish() {
  if (!idle()) return false;
  if (_compression) {
    HSE_finish_res result;
    do {
      result = heatshrink_encoder_finish(&encoder);
      if (result < 0) {
        fail("BFT compression finish failed");
        return false;
      }
      if (result == HSER_FINISH_MORE && !drainEncoder()) return false;
    } while (result != HSER_FINISH_DONE);
  }
  if (!query()) return false;
  _file = File::Closing;
  const bool closed = sendPacket(1, 2, nullptr, 0, Reply::Success);
  if (_reply == Reply::Success || _reply == Reply::Invalid)
    _file = File::None;
  else if (_reply == Reply::IoError)
    _file = File::Open;
  else
    _file = File::Unknown;
  return closed && leave();
}
bool BinaryFileTransferService::startPrint(const char* filename) {
  if (active() || !filename || !*filename) return false;
  char select[128];
  const int selectLength =
      snprintf(select, sizeof(select), "M23 %s\n", filename);
  if (selectLength < 0 || selectLength >= static_cast<int>(sizeof(select)))
    return false;
  if (!esp3d_serial_service.claimExclusive()) {
    fail("Printer UART is busy");
    return false;
  }
  _failed = false;
  _error[0] = '\0';
  _mode = Mode::Ascii;
  _file = File::None;
  _transportReady = _waiting = _waitingSync = false;
  _asciiExpected = AsciiReply::None;
  _lineSize = 0;
  _discardLine = false;
  lockRx();
  _head = _tail = 0;
  _overflow = false;
  _active = true;
  unlockRx();
  esp3d_serial_service.flush();
  // Keep the commands in separate request/response rounds. Otherwise the
  // `ok` for M21 can be mistaken for the M23 acknowledgement and M24 starts
  // the previously selected file (or no file at all).
  const bool mounted = ascii("M21\n", AsciiReply::Ok);
  // Marlin variants have different informational M23 messages, but all
  // terminate the command with `ok`.
  const bool selected = mounted && ascii(select, AsciiReply::Ok);
  const bool started = selected && ascii("M24\n", AsciiReply::Ok);
  release();
  return started;
}
bool BinaryFileTransferService::abort() {
  if (!active()) return true;
  poll();
  if (_mode == Mode::Ascii) {
    release();
    return true;
  }
  if (_mode != Mode::Binary || (_file != File::None && _file != File::Open))
    return false;
  // An earlier confirmed OPEN is not proof of ownership after Marlin's idle
  // timeout. Never issue its unguarded ABORT in that situation.
  if (_file == File::Open &&
      millis() - _lastFileActivity >= kOwnershipTimeout) {
    _file = File::Unknown;
    fail("BFT cleanup refused: file ownership expired");
    return false;
  }
  _cleanup = true;
  bool recovered = _transportReady || synchronize();
  if (recovered && _file == File::Open)
    recovered = query();  // drain older file results before cleanup
  if (recovered && _file == File::Open) {
    recovered = sendPacket(1, 4, nullptr, 0, Reply::Success);
    _file = recovered ? File::None : File::Unknown;
  }
  if (recovered) recovered = leave();
  _cleanup = false;
  return recovered;
}
#endif
