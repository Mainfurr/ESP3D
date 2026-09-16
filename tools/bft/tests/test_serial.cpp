#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
using uint = unsigned;
uint32_t ticks = 0;
uint32_t millis() { return ticks++; }
struct ESP3DHal {
  static void wait(uint32_t ms) { ticks += ms; }
};
void lockTx() {}
void unlockTx() {}
struct Hardware {
  size_t bytes = 0;
  size_t limit = 1000;
  std::function<void()> duringWrite;
  size_t availableForWrite() { return 128; }
  size_t write(const uint8_t*, size_t n) {
    if (duringWrite) duringWrite();
    n = n < limit ? n : limit;
    bytes += n;
    return n;
  }
} hardware;
Hardware* Serials[] = {&hardware};
struct ESP3DSerialService {
  bool _started = true, _exclusive = false, _writing = false;
  size_t _serialIndex = 0;
  bool claimExclusive();
  void releaseExclusive();
  size_t writeBytes(const uint8_t*, size_t);
  size_t writeExclusive(const uint8_t*, size_t);
  size_t writeGuarded(const uint8_t*, size_t, bool);
  size_t writeRaw(const uint8_t*, size_t);
};
// Exact production methods, extracted by run.py to isolate Arduino
// dependencies.
#include "serial_write_methods.inc"
int main() {
  ESP3DSerialService port;
  const uint8_t data[] = {'M', '1', '0', '5', '\n'};
  assert(port.writeExclusive(data, 5) == 0);
  assert(port.writeBytes(data, 5) == 5);
  assert(port.claimExclusive());
  assert(!port.claimExclusive());
  assert(port.writeBytes(data, 5) == 0);
  assert(hardware.bytes == 5);
  hardware.duringWrite = [&] {
    assert(port.writeBytes(data, 5) == 0);
    assert(port.writeExclusive(data, 5) == 0);
    assert(!port.claimExclusive());
  };
  assert(port.writeExclusive(data, 5) == 5);
  hardware.duringWrite = nullptr;
  port.releaseExclusive();
  assert(port.writeBytes(data, 5) == 5);
  hardware.duringWrite = [&] { assert(!port.claimExclusive()); };
  hardware.limit = 2;
  assert(port.writeBytes(data, 5) == 2);
  assert(!port._writing);
  hardware.duringWrite = nullptr;
  assert(port.claimExclusive());
  port.releaseExclusive();
  port._started = false;
  assert(!port.claimExclusive());
  std::cout << "PASS production UART ownership and in-flight write exclusion\n";
}
