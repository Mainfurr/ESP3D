/* Marlin Binary File Transfer 0.1.0 client (uncompressed files). */
#ifndef _BINARY_FILE_TRANSFER_SERVICE_H
#define _BINARY_FILE_TRANSFER_SERVICE_H
#include "../../include/esp3d_config.h"

#if COMMUNICATION_PROTOCOL == RAW_SERIAL
class BinaryFileTransferService {
 public:
  static bool begin(const char* filename);
  static bool write(const uint8_t* data, size_t size);
  // Call while waiting for the next input chunk. Services UART and keepalive.
  static bool idle();
  static bool finish();
  // False means recovery is unconfirmed: the UART remains reserved.
  static bool abort();
  static void receiveByte(uint8_t value);
  static bool active();
  static const char* error() { return _error; }

 private:
  enum class Mode : uint8_t { Ascii, Entering, Binary, Leaving, Unknown };
  enum class File : uint8_t { None, Opening, Open, Closing, Unknown, Foreign };
  enum class Reply : uint8_t {
    None,
    Success,
    Version,
    Fail,
    Busy,
    Invalid,
    IoError
  };
  enum class AsciiReply : uint8_t { None, Firmware, Switch };
  static constexpr size_t kPacketPayloadCapacity = 128;
  static constexpr size_t kFilenameCapacity = 120;
  static constexpr size_t kLineCapacity = 384;
  static constexpr size_t kRxCapacity = 1024;

  static void poll();
  static void lineReceived();
  static void fail(const char* message);
  static void release();
  static bool writeAll(const uint8_t* data, size_t size);
  static bool ascii(const char* command, AsciiReply expected);
  static bool synchronize();
  static bool query();
  static bool sendPacket(uint8_t protocol, uint8_t type,
                         const uint8_t* payload = nullptr, size_t size = 0,
                         Reply expected = Reply::None);
  static bool leave();
  static uint16_t fletcher(const uint8_t* data, size_t size);

  // RX callback touches only these fields, under the RX critical section.
  static bool _active;
  static uint8_t _rx[kRxCapacity];
  static size_t _head, _tail;
  static bool _overflow;
  // All following state belongs to the synchronous upload caller.
  static Mode _mode;
  static File _file;
  static bool _failed, _cleanup, _transportReady;
  static char _error[128], _line[kLineCapacity];
  static size_t _lineSize;
  static bool _discardLine;
  static uint8_t _sync, _pendingSync;
  static uint16_t _payloadSize;
  static uint8_t _packet[kPacketPayloadCapacity + 10];
  static bool _waiting, _acked, _resend, _waitingSync, _gotSync;
  static Reply _expected, _reply;
  static AsciiReply _asciiExpected;
  static bool _asciiMarker, _asciiOk, _unsupported;
  static uint32_t _lastFileActivity;
};
#endif
#endif
