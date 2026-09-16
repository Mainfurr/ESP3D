#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
using millis_t = uint32_t;
using serial_index_t = int;
#define ENABLED(x) 0  // compression is deliberately outside these tests
#define TERN_(x, y)
#define HAS_MEDIA 0
#define NUM_SERIAL 1
#define F(x) x
#define C(x) x
#define ELAPSED(now, deadline) (int32_t((now) - (deadline)) >= 0)
#define PENDING(now, deadline) (!ELAPSED(now, deadline))
uint32_t millis();
void emit(const std::string& value);
inline std::string format(const char* v) { return v; }
inline std::string format(char v) { return std::string(1, v); }
template <class T>
std::string format(T v) {
  return std::to_string(v);
}
template <class... T>
void print(T... v) {
  (emit(format(v)), ...);
}
#define SERIAL_ECHO(...) print(__VA_ARGS__)
#define SERIAL_ECHOLN(...) \
  do {                     \
    print(__VA_ARGS__);    \
    emit("\n");            \
  } while (0)
#define SERIAL_ECHOLNPGM(...) SERIAL_ECHOLN(__VA_ARGS__)
#define SERIAL_ECHO_MSG(...) SERIAL_ECHOLN(__VA_ARGS__)
struct SerialImpl {
  std::deque<uint8_t> input;
  bool available(int) { return !input.empty(); }
  int read(int) {
    int b = input.front();
    input.pop_front();
    return b;
  }
};
extern SerialImpl SERIAL_IMPL;
struct Card {
  struct {
    bool binary_mode = false;
  } flag;
  int transfer_port_index = 0;
  char filename[121] = {};
  bool opened = false, exists = false, writeError = false;
  unsigned writes = 0, removes = 0, opens = 0, closes = 0;
  std::vector<uint8_t> contents;
  void mount() {}
  void release() {}
  void openFileWrite(char* name) {
    std::strncpy(filename, name, 120);
    opened = exists = true;
    contents.clear();
    ++opens;
  }
  bool isFileOpen() { return opened; }
  int write(const char* data, size_t size) {
    ++writes;
    if (writeError) return -1;
    contents.insert(contents.end(), data, data + size);
    return size;
  }
  void closefile() {
    opened = false;
    ++closes;
  }
  void removeFile(char*) {
    exists = false;
    contents.clear();
    ++removes;
  }
};
extern Card card;
