#pragma once
#include <cstdint>

extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
struct TestSerial {
  template <typename... Args>
  void printf(const char *, Args...) {}
};
extern TestSerial Serial;
