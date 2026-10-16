#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>

#include "esp3d/src/include/esp3d_config.h"
#include "esp3d/src/modules/serial/serial_service.h"
#define private public
#include "Marlin/feature/binary_stream.h"
#include "esp3d/src/modules/bft/binary_file_transfer_service.h"
#undef private
using B = BinaryFileTransferService;
// Definitions from sample_code/binary_stream.cpp (the rest is platform
// includes).
char* SDFileTransferProtocol::Packet::Open::data = nullptr;
size_t SDFileTransferProtocol::data_waiting = 0,
       SDFileTransferProtocol::transfer_timeout = 0,
       SDFileTransferProtocol::idle_timeout = 0;
bool SDFileTransferProtocol::transfer_active = false,
     SDFileTransferProtocol::dummy_transfer = false,
     SDFileTransferProtocol::compression = false;
BinaryStream binaryStream[NUM_SERIAL];
SerialImpl SERIAL_IMPL;
Card card;
ESP3DSerialService esp3d_serial_service;
namespace {
uint32_t now = 0;
std::deque<uint8_t> rx;
std::string response, asciiInput;
std::string compressionCapability = "none";
std::vector<std::vector<uint8_t>> packets;
std::vector<std::string> commands;
std::function<bool(const std::string&)> drop;
std::function<void(const std::string&)> beforeResponse;
bool capability = true, noCapability = false, noSwitch = false,
     corruptNext = false;
bool fragment = false, pumping = false;
unsigned readGate = 0;
size_t writeLimit = 10000, writeBudget = 10000000;
uint32_t closeAt = 0, asciiAfterClose = 0;
unsigned skipPostCloseM115 = 0;
uint32_t delayCloseResultMs = 0, delayedAt = 0;
std::string delayedLine;
std::vector<uint8_t> written;
uint8_t commandMeta = 0;
char receiveBuffer[96] = {};
void enqueue(const std::string& s) {
  for (auto c : s) rx.push_back(c);
}
void pump() {
  if (pumping) return;
  pumping = true;
  if (!delayedLine.empty() && now >= delayedAt) {
    enqueue(delayedLine);
    delayedLine.clear();
  }
  if (card.flag.binary_mode)
    binaryStream[0].receive(receiveBuffer);
  else {
    while (!SERIAL_IMPL.input.empty()) {
      const char c = SERIAL_IMPL.read(0);
      if (c == '\n') {
        commands.push_back(asciiInput);
        if (asciiInput == "M115") {
          if (closeAt) asciiAfterClose = now;
          if (closeAt && skipPostCloseM115)
            --skipPostCloseM115;
          else {
            enqueue("FIRMWARE_NAME:Marlin test\n");
            if (!noCapability)
              enqueue(capability ? "Cap:BINARY_FILE_TRANSFER:1\n"
                                 : "Cap:BINARY_FILE_TRANSFER:0\n");
            enqueue("ok N0 P16 B3\n");
          }
        } else if (asciiInput == "M28 B1" && !noSwitch) {
          card.flag.binary_mode = true;
          enqueue("echo:Switching to Binary Protocol\nok\n");
        } else if (asciiInput == "M21") {
          enqueue("ok\n");
        } else if (asciiInput.rfind("M23 ", 0) == 0) {
          enqueue("File selected\nok\n");
        } else if (asciiInput == "M24") {
          enqueue("ok\n");
        }
        asciiInput.clear();
        if (card.flag.binary_mode) break;
      } else
        asciiInput += c;
    }
  }
  pumping = false;
}
void reset() {
  now = 0;
  rx.clear();
  response.clear();
  compressionCapability = "none";
  asciiInput.clear();
  packets.clear();
  commands.clear();
  drop = nullptr;
  beforeResponse = nullptr;
  writeLimit = 10000;
  writeBudget = 10000000;
  capability = true;
  noCapability = noSwitch = corruptNext = fragment = pumping = false;
  closeAt = asciiAfterClose = 0;
  skipPostCloseM115 = 0;
  delayCloseResultMs = delayedAt = 0;
  delayedLine.clear();
  written.clear();
  commandMeta = 0;
  readGate = 0;
  SERIAL_IMPL.input.clear();
  card = Card{};
  binaryStream[0] = BinaryStream{};
  SDFileTransferProtocol::transfer_active = false;
  SDFileTransferProtocol::data_waiting =
      SDFileTransferProtocol::transfer_timeout =
          SDFileTransferProtocol::idle_timeout = 0;
  SDFileTransferProtocol::dummy_transfer = SDFileTransferProtocol::compression =
      false;
  B::_active = false;
  B::_failed = false;
  B::_mode = B::Mode::Ascii;
  B::_file = B::File::None;
  esp3d_serial_service.exclusive = false;
}
size_t count(uint8_t meta) {
  return std::count_if(packets.begin(), packets.end(),
                       [=](const auto& p) { return p[3] == meta; });
}
void test(const char* name, const std::function<void()>& body) {
  reset();
  body();
  std::cout << "PASS " << name << std::endl;
}
}  // namespace
uint32_t millis() { return now++; }
void noInterrupts() {}
void interrupts() {}
void ESP3DHal::wait(uint32_t ms) {
  now += ms;
  pump();
}
void emit(const std::string& s) {
  for (char c : s) {
    if (c != '\n') {
      response += c;
      continue;
    }
    const std::string line = response.rfind("PFT:version:", 0) == 0
        ? "PFT:version:0.1.0:compression:" + compressionCapability : response;
    response.clear();
    if (beforeResponse) beforeResponse(line);
    if (commandMeta == 0x12 && line == "PFT:success" &&
        delayCloseResultMs) {
      delayedLine = line + "\n";
      delayedAt = now + delayCloseResultMs;
    } else if (!drop || !drop(line))
      enqueue(line + "\n");
    if (commandMeta == 2 && line.rfind("ok", 0) == 0) closeAt = now;
  }
}
bool ESP3DSerialService::claimExclusive() {
  if (exclusive) return false;
  exclusive = true;
  return true;
}
void ESP3DSerialService::releaseExclusive() { exclusive = false; }
void ESP3DSerialService::flush() {}
int ESP3DSerialService::available() {
  pump();
  // Yield between individual reads to cover fragmented lines.
  if (fragment) return (++readGate % 2) ? 0 : std::min(size_t(1), rx.size());
  return rx.size();
}
int ESP3DSerialService::read() {
  if (rx.empty()) return -1;
  int v = rx.front();
  rx.pop_front();
  return v;
}
size_t ESP3DSerialService::writeBytes(const uint8_t* data, size_t size) {
  return exclusive ? 0 : writeExclusive(data, size);
}
size_t ESP3DSerialService::writeExclusive(const uint8_t* data, size_t size) {
  assert(exclusive);
  size = std::min(std::min(size, writeLimit), writeBudget);
  writeBudget -= size;
  for (size_t i = 0; i < size; ++i) {
    written.push_back(data[i]);
    uint8_t b = data[i];
    if (corruptNext && written.size() == 7 && written[0] == 0xad) {
      b ^= 1;
      corruptNext = false;
    }
    SERIAL_IMPL.input.push_back(b);
  }
  if (written.size() >= 8 && written[0] == 0xad) {
    const size_t n = written[4] | (written[5] << 8);
    if (written.size() == (n ? n + 10 : 8)) {
      packets.push_back(written);
      commandMeta = written[3];
      written.clear();
    }
  } else if (!written.empty() && written[0] != 0xad && written.back() == '\n')
    written.clear();
  return size;
}
int main() {
  test("compressed stream: fragmented input, wraparound and final flush", [] {
    compressionCapability = "heatshrink,8,4";
    binaryStream[0].sync = 254;
    writeLimit = 3;
    std::vector<uint8_t> input;
    const std::string line = "G1 X123.45 Y67.89 E0.1234\n";
    for (unsigned i = 0; i < 200; ++i)
      input.insert(input.end(), line.begin(), line.end());
    assert(B::begin("C.GCO"));
    assert(packets[2][8] == 0 && packets[2][9] == 1);
    for (size_t i = 0; i < input.size();) {
      const size_t chunk = std::min(input.size() - i, i % 71 + 1);
      assert(B::write(input.data() + i, chunk));
      i += chunk;
    }
    assert(B::finish());
    assert(card.contents == input);
    size_t wireBytes = 0;
    for (const auto& p : packets)
      if (p[3] == 0x13) wireBytes += p[4] | (p[5] << 8);
    assert(wireBytes < input.size() / 2);
    std::cout << "Compression: " << input.size() << " -> " << wireBytes << '\n';
  });
  test("compressed empty, short and incompressible files; encoder resets", [] {
    compressionCapability = "heatshrink,8,4";
    for (size_t size : {0, 1, 15, 16, 255, 256, 257, 4096}) {
      std::vector<uint8_t> input(size);
      uint32_t random = 123456;
      for (auto& byte : input) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = random & 255;
      }
      assert(B::begin("C.GCO"));
      assert(B::write(input.data(), input.size()));
      assert(B::finish());
      assert(card.contents == input);
    }
  });
  test("compression capabilities fall back to raw", [] {
    for (const char* capability : {"none", "heatshrink,11,4", "heatshrink,8,3",
                                   "heatshrink,8,4junk", "unknown", ""}) {
      compressionCapability = capability;
      assert(B::begin("R.GCO"));
      assert(!SDFileTransferProtocol::compression);
      const uint8_t input[] = {0, 0xff, 3, 7};
      assert(B::write(input, sizeof(input)));
      assert(B::finish());
      assert(card.contents == std::vector<uint8_t>(input, input + sizeof(input)));
    }
  });
  test("compressed keepalive cannot change encoding midstream", [] {
    compressionCapability = "heatshrink,8,4";
    assert(B::begin("C.GCO"));
    compressionCapability = "none";
    const uint8_t input[] = {'A', 'B', 'C'};
    assert(B::write(input, sizeof(input)));
    now += 3100;
    assert(B::idle());
    assert(B::finish());
    assert(card.contents == std::vector<uint8_t>(input, input + sizeof(input)));
  });
  test("compressed packet retry preserves stream and abort resets encoder", [] {
    compressionCapability = "heatshrink,8,4";
    assert(B::begin("C.GCO"));
    std::vector<uint8_t> input(1024, 'A');
    corruptNext = true;
    assert(B::write(input.data(), input.size()));
    assert(B::finish());
    assert(card.contents == input);
    assert(packets[3] == packets[4]);
    assert(B::begin("D.GCO"));
    assert(B::write(input.data(), 17));
    assert(B::abort());
    assert(B::begin("E.GCO"));
    assert(B::write(input.data(), 3));
    assert(B::finish());
    assert(card.contents == std::vector<uint8_t>(3, 'A'));
  });
  test("compressed SD failure during final flush is reported", [] {
    compressionCapability = "heatshrink,8,4";
    assert(B::begin("C.GCO"));
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    card.writeError = true;
    assert(!B::finish());
    assert(B::abort());
  });
  test(
      "actual Marlin receiver: arbitrary bytes, wraparound, partial UART "
      "writes",
      [] {
        binaryStream[0].sync = 254;
        writeLimit = 3;
        assert(B::begin("A.GCO"));
        std::vector<uint8_t> input(512);
        for (size_t i = 0; i < input.size(); ++i) input[i] = i % 256;
        if (!B::write(input.data(), input.size())) {
          std::cerr << B::error() << " sync=" << unsigned(B::_sync)
                    << " peer=" << unsigned(binaryStream[0].sync)
                    << " time=" << now << " packets=" << packets.size()
                    << std::endl;
          std::abort();
        }
        assert(B::finish());
        assert(card.contents == input);
        assert(!B::active());
        if (asciiAfterClose - closeAt < 50) {
          std::cerr << "close=" << closeAt << " ascii=" << asciiAfterClose
                    << std::endl;
          std::abort();
        }
        assert(commands[0] == "M115" && commands[1] == "M28 B1" &&
               commands.back() == "M115");
        assert(packets[2][8] == 0 && packets[2][9] == 0);
      });
  test("empty file and repeat session without receiver reset", [] {
    assert(B::begin("A.GCO"));
    assert(B::finish());
    assert(card.contents.empty());
    assert(B::begin("B.GCO"));
    assert(B::finish());
    assert(card.opens == 2);
  });
  test("lost entry SYNC reply is retried without opening twice", [] {
    unsigned lost = 0;
    drop = [&](const std::string& s) {
      return s.rfind("ss", 0) == 0 && lost++ == 0;
    };
    assert(B::begin("A.GCO"));
    assert(count(1) == 2);
    assert(card.opens == 1);
    assert(B::finish());
  });
  test("missing entry SYNC identifies the failed phase", [] {
    drop = [](const std::string& s) { return s.rfind("ss", 0) == 0; };
    assert(!B::begin("A.GCO"));
    assert(count(1) == 3);
    assert(std::string(B::error()).find("during entry") != std::string::npos);
    assert(card.opens == 0);
  });
  test("M115 lost during mode switch is retried", [] {
    assert(B::begin("A.GCO"));
    skipPostCloseM115 = 1;
    assert(B::finish());
    assert(!B::active());
    assert(commands.back() == "M115");
    assert(std::count(commands.begin(), commands.end(), "M115") == 3);
  });
  test("confirmed binary close releases UART even if M115 is lost", [] {
    assert(B::begin("A.GCO"));
    skipPostCloseM115 = 2;
    assert(!B::finish());
    assert(!B::active());
    assert(!esp3d_serial_service.exclusive);
    assert(std::string(B::error()).find("M115") != std::string::npos);
  });
  test("print waits for separate mount, select, and start confirmations", [] {
    assert(B::startPrint("A.GCO"));
    assert(commands.size() == 3);
    assert(commands[0] == "M21");
    assert(commands[1] == "M23 A.GCO");
    assert(commands[2] == "M24");
    assert(!B::active());
    assert(!esp3d_serial_service.exclusive);
  });
  test("explicit missing capability and absent capability", [] {
    capability = false;
    assert(!B::begin("A.GCO"));
    assert(!B::active());
    assert(packets.empty());
    noCapability = true;
    assert(B::begin("A.GCO"));
    assert(B::finish());
  });
  test("unconfirmed M28 never receives SYNC or binary cleanup", [] {
    noSwitch = true;
    assert(!B::begin("A.GCO"));
    assert(packets.empty());
    assert(!B::abort());
    assert(B::active());
  });
  test("root filename and negotiated payload limit", [] {
    assert(!B::begin("dir/A.GCO"));
    assert(!B::begin("0:A.GCO"));
    assert(!B::begin(".."));
    assert(!B::begin(std::string(94, 'x').c_str()));
    assert(card.opens == 0);
    assert(B::abort());
  });
  test("rs expected sequence retries identical packet without double write",
       [] {
         assert(B::begin("A.GCO"));
         corruptNext = true;
         const uint8_t bytes[] = {'A', 'B'};
         assert(B::write(bytes, 2));
         assert(count(0x13) == 2);
         assert(card.writes == 1);
         assert(card.contents.size() == 2);
         assert(packets[3] == packets[4]);
         assert(B::finish());
       });
  test("lost ACK recovers duplicate once, no double SD write", [] {
    assert(B::begin("A.GCO"));
    bool lost = false;
    drop = [&](const std::string& s) {
      if (commandMeta == 0x13 && s.rfind("ok", 0) == 0 && !lost) {
        lost = true;
        return true;
      }
      return false;
    };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(card.writes == 1);
    assert(count(0x13) == 2);
    assert(B::finish());
  });
  test("repeated lost WRITE ACKs recover with SYNC", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 0x13 && s.rfind("ok", 0) == 0;
    };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(card.writes == 1);
    assert(count(0x13) == 3);
    assert(B::finish());
  });
  test("lost recovery SYNC reply is retried without repeating WRITE", [] {
    assert(B::begin("A.GCO"));
    unsigned syncReplies = 0;
    drop = [&](const std::string& s) {
      if (commandMeta == 0x13 && s.rfind("ok", 0) == 0) return true;
      return s.rfind("ss", 0) == 0 && syncReplies++ == 0;
    };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(card.writes == 1);
    assert(count(1) == 3);  // entry plus two recovery attempts
    assert(B::finish());
  });
  test("missing recovery SYNC identifies the failed phase", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return (commandMeta == 0x13 && s.rfind("ok", 0) == 0) ||
             s.rfind("ss", 0) == 0;
    };
    const uint8_t byte = 'A';
    assert(!B::write(&byte, 1));
    assert(std::string(B::error()).find("during packet recovery") !=
           std::string::npos);
  });
  test("late resend request does not abort SYNC recovery", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 0x13 && s.rfind("ok", 0) == 0;
    };
    beforeResponse = [](const std::string& s) {
      if (s.rfind("ss", 0) == 0) enqueue("rs3\n");
    };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(card.writes == 1);
    assert(B::finish());
  });
  test("lost ok255 recovers with SYNC without replaying data", [] {
    binaryStream[0].sync = 253;
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) { return s == "ok255"; };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(count(0x13) == 1);
    assert(card.writes == 1);
    assert(B::finish());
  });
  test("lost ACK with embedded token recovers with SYNC without replay", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 0x13 && s.rfind("ok", 0) == 0;
    };
    const uint8_t bytes[] = {0xad, 0xb5, 0, 1, 0, 0, 1, 3};
    assert(B::write(bytes, sizeof(bytes)));
    assert(count(0x13) == 1);
    assert(card.writes == 1);
    assert(B::finish());
  });
  test("SD ioerror is sticky and stops new writes", [] {
    assert(B::begin("A.GCO"));
    card.writeError = true;
    const uint8_t byte = 'A';
    assert(!B::write(&byte, 1));
    const auto writes = count(0x13);
    assert(!B::write(&byte, 1));
    assert(count(0x13) == writes);
    assert(!B::finish());
    assert(B::abort());
    assert(card.removes == 1);
    assert(std::string(B::error()) == "PFT:ioerror");
  });
  test("fourth response ioerror after diagnostics cannot become success", [] {
    assert(B::begin("A.GCO"));
    card.writeError = true;
    beforeResponse = [](const std::string& s) {
      if (commandMeta == 0x13 && s.rfind("ok", 0) == 0)
        enqueue("echo:a\necho:b\n");
    };
    const uint8_t byte = 'A';
    assert(!B::write(&byte, 1));
    assert(!B::finish());
    assert(B::abort());
  });
  test("finish QUERY barrier catches error arriving after WRITE ACK", [] {
    assert(B::begin("A.GCO"));
    beforeResponse = [](const std::string& s) {
      if (commandMeta == 0x10 && s.rfind("ok", 0) == 0)
        enqueue("PFT:ioerror\n");
    };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(count(0x13) == 1);
    assert(!B::finish());
    beforeResponse = nullptr;
    assert(B::abort());
  });
  test("WRITE barriers are periodic rather than per packet", [] {
    assert(B::begin("A.GCO"));
    std::vector<uint8_t> data(4096, 'A');
    assert(B::write(data.data(), data.size()));
    // At 96-byte Marlin payloads this is 43 WRITEs, but only one periodic
    // barrier in addition to the QUERY which opened the session.
    assert(count(0x10) == 2);
    assert(B::finish());
    assert(count(0x10) == 3);
  });
  test("lost QUERY result retries a new QUERY without repeating WRITE", [] {
    assert(B::begin("A.GCO"));
    unsigned lost = 0;
    drop = [&](const std::string& s) {
      return commandMeta == 0x10 &&
             s.rfind("PFT:version:", 0) == 0 && lost++ == 0;
    };
    std::vector<uint8_t> data(4096, 'A');
    assert(B::write(data.data(), data.size()));
    assert(count(0x10) == 3);  // entry, missing result, fresh QUERY
    assert(count(0x13) == 43);
    assert(card.contents == data);
    assert(B::finish());
  });
  test("two missing QUERY results still fail without replaying WRITE", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 0x10 && s.rfind("PFT:version:", 0) == 0;
    };
    std::vector<uint8_t> data(4096, 'A');
    assert(!B::write(data.data(), data.size()));
    assert(count(0x10) == 3);
    assert(count(0x13) == 43);
    assert(card.contents == data);
    assert(std::string(B::error()).find("packet 16") !=
           std::string::npos);
  });
  test("lost OPEN result keeps uncertainty and never ABORTs", [] {
    drop = [](const std::string& s) {
      return commandMeta == 0x11 && s == "PFT:success";
    };
    assert(!B::begin("A.GCO"));
    assert(B::abort());
    assert(count(0x14) == 0);
    assert(!card.opened && !B::active());
    assert(card.removes == 1);
  });
  test("lost CLOSE result never ABORTs potentially closed file", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 0x12 && s == "PFT:success";
    };
    assert(!B::finish());
    assert(B::abort());
    assert(count(0x14) == 0);
    assert(!B::active());
    assert(card.removes == 0);
  });
  test("slow SD close result is accepted after five seconds", [] {
    assert(B::begin("A.GCO"));
    delayCloseResultMs = 6000;
    assert(B::finish());
    assert(!B::active());
    assert(card.closes == 1);
  });
  test("lost ABORT result waits for safe idle cleanup", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 0x14 && s == "PFT:success";
    };
    assert(B::abort());
    assert(count(0x14) == 1);
    assert(count(2) == 1);
    assert(!B::active());
  });
  test("busy never closes/deletes foreign transfer", [] {
    SDFileTransferProtocol::transfer_active = true;
    SDFileTransferProtocol::transfer_timeout = 100000;
    assert(!B::begin("A.GCO"));
    assert(!B::abort());
    assert(card.removes == 0 && count(2) == 0);
  });
  test("idle keeps a slow HTTP producer alive", [] {
    assert(B::begin("A.GCO"));
    for (int i = 0; i < 15; ++i) {
      now += 1000;
      assert(B::idle());
    }
    assert(card.opened && count(0x10) >= 5);
    assert(B::finish());
  });
  test("expired ownership never ABORTs a stale file", [] {
    assert(B::begin("A.GCO"));
    now += 11000;
    assert(!B::idle());
    const auto removed = card.removes;
    assert(!B::abort());
    assert(count(0x14) == 0);
    assert(card.removes == removed);
  });
  test("RX overflow is fatal rather than silently losing an error", [] {
    assert(B::begin("A.GCO"));
    for (int i = 0; i < 1100; ++i) B::receiveByte('x');
    assert(!B::idle());
    assert(!B::abort());
    assert(B::active());
  });
  test("overlong diagnostic suffix is ignored before SYNC recovery", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 0x13 && s.rfind("ok", 0) == 0;
    };
    beforeResponse = [](const std::string& s) {
      if (commandMeta == 0x13 && s.rfind("ok", 0) == 0)
        enqueue(std::string(384, 'x') + s + "\n");
    };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(B::finish());
  });
  test("second begin preserves current transfer", [] {
    assert(B::begin("A.GCO"));
    assert(!B::begin("B.GCO"));
    assert(B::finish());
    assert(card.opens == 1);
  });
  test("fatal error never becomes success", [] {
    assert(B::begin("A.GCO"));
    enqueue("fe7\n");
    assert(!B::idle());
    assert(std::string(B::error()).find("fatal") != std::string::npos);
    assert(B::abort());
  });
  test("1024 packets accepted by the actual receiver and checked independently",
       [] {
         assert(B::begin("A.GCO"));
         std::vector<uint8_t> payload(96);
         auto checksum = [](const std::vector<uint8_t>& p, size_t end) {
           unsigned a = 0, b = 0;
           for (size_t j = 2; j < end; ++j) {
             a = (a + p[j]) % 255;
             b = (b + a) % 255;
           }
           return (b << 8) | a;
         };
         size_t bytes = 0;
         for (unsigned i = 0; i < 1024; ++i) {
           const size_t n = i % 97;
           for (size_t j = 0; j < n; ++j) payload[j] = (i + j) % 256;
           assert(B::sendPacket(1, 3, payload.data(), n));
           const auto p = packets.back();
           assert(p.size() == (n ? n + 10 : 8));
           assert(p[0] == 0xad && p[1] == 0xb5 && p[3] == 0x13);
           assert(unsigned(p[6] | (p[7] << 8)) == checksum(p, 6));
           if (n)
             assert(unsigned(p[p.size() - 2] | (p.back() << 8)) ==
                    checksum(p, p.size() - 2));
           assert(B::query());
           bytes += n;
         }
         assert(card.contents.size() == bytes);
         assert(B::finish());
       });
  test("byte-at-a-time response reads", [] {
    fragment = true;
    assert(B::begin("A.GCO"));
    assert(B::finish());
  });
  test("ACK duplicates are consumed once", [] {
    assert(B::begin("A.GCO"));
    beforeResponse = [](const std::string& s) {
      if (commandMeta == 0x13 && s.rfind("ok", 0) == 0)
        enqueue(s + "\r\n" + s + "\n");
    };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(B::finish());
    assert(card.writes == 1);
  });
  test("ACK retries fall back to SYNC after their bound", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 0x13 && s.rfind("ok", 0) == 0;
    };
    const uint8_t byte = 'A';
    assert(B::write(&byte, 1));
    assert(count(0x13) == 3);
    assert(card.writes == 1);
    assert(B::finish());
  });
  test("mismatched rs never renumbers data", [] {
    assert(B::begin("A.GCO"));
    beforeResponse = [](const std::string& s) {
      if (commandMeta == 0x13 && s.rfind("ok", 0) == 0) enqueue("rs77\n");
    };
    const uint8_t byte = 'A';
    assert(!B::write(&byte, 1));
    assert(count(0x13) == 1);
    beforeResponse = nullptr;
    assert(B::abort());
  });
  test("out-of-range SYNC rejected before OPEN", [] {
    beforeResponse = [](const std::string& s) {
      if (s.rfind("ss", 0) == 0) enqueue("ss256,96,0.1.0\n");
    };
    assert(!B::begin("A.GCO"));
    assert(card.opens == 0);
  });
  test("idle outside BFT does not steal printer output", [] {
    enqueue("ok\n");
    assert(!B::idle());
    assert(rx.size() == 3);
  });
  test("unsigned millis wraparound", [] {
    now = UINT32_MAX - 1000;
    assert(B::begin("A.GCO"));
    for (int i = 0; i < 10; ++i) {
      now += 1000;
      assert(B::idle());
    }
    assert(B::finish());
  });

  test("short frame times out before recovery SYNC", [] {
    assert(B::begin("A.GCO"));
    writeBudget = 5;
    const uint8_t byte = 'A';
    assert(!B::write(&byte, 1));
    assert(card.writes == 0);
    writeBudget = 10000000;
    written.clear();
    assert(B::abort());
    assert(card.removes == 1);
  });
  test("lost CONTROL/CLOSE ACK never emits binary retry into ASCII", [] {
    assert(B::begin("A.GCO"));
    drop = [](const std::string& s) {
      return commandMeta == 2 && s.rfind("ok", 0) == 0;
    };
    assert(!B::finish());
    assert(count(2) == 1);
    assert(!B::abort());
    assert(B::active());
    assert(commands.size() == 2);
  });
  test("PTF invalid type does not imply that the file is closed", [] {
    assert(B::begin("A.GCO"));
    enqueue("PTF:invalid\n");
    assert(!B::idle());
    assert(!B::abort());
    assert(count(2) == 0 && count(0x14) == 0);
  });
  test("PTF fail is recognized as an OPEN rejection", [] {
    B::_mode = B::Mode::Binary;
    B::_waiting = true;
    B::_expected = B::Reply::Success;
    std::strcpy(B::_line, "PTF:fail");
    B::lineReceived();
    assert(B::_reply == B::Reply::Fail);
    assert(std::strcmp(B::error(), "PTF:fail") == 0);
  });
  test("printer restart invalidates file ownership", [] {
    assert(B::begin("A.GCO"));
    enqueue("start\n");
    assert(!B::idle());
    assert(!B::abort());
    assert(count(0x14) == 0);
  });
  test("unsupported file version can leave without touching a file", [] {
    beforeResponse = [](const std::string& s) {
      if (s.rfind("PFT:version:", 0) == 0)
        enqueue("PFT:version:9.0.0:compression:none\n");
    };
    assert(!B::begin("A.GCO"));
    assert(card.opens == 0);
    assert(B::abort());
    assert(!B::active());
  });
  return 0;
}
