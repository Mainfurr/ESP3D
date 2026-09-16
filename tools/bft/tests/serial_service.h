#pragma once
#include <cstddef>
#include <cstdint>
struct ESP3DSerialService {
  bool exclusive = false;
  bool claimExclusive();
  void releaseExclusive();
  void flush();
  int available();
  int read();
  size_t writeExclusive(const uint8_t* data, size_t size);
  size_t writeBytes(const uint8_t* data, size_t size);
};
extern ESP3DSerialService esp3d_serial_service;
