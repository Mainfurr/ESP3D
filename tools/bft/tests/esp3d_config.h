#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#define RAW_SERIAL 1
#define COMMUNICATION_PROTOCOL RAW_SERIAL
#define ARDUINO_ARCH_ESP8266 1
uint32_t millis();
void noInterrupts();
void interrupts();
struct ESP3DHal {
  static void wait(uint32_t ms);
};
