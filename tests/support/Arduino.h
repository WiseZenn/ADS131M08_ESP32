#pragma once
#include <cstdint>
#include <cstring>
#include <string>

constexpr int HIGH = 1, LOW = 0, INPUT = 0, OUTPUT = 1;
inline int testDrdy = LOW;
inline int testClockStarts = 0;
inline uint32_t testMicros = 0;
inline bool testAutoClearDrdy = false;
using String = std::string;
struct TestSerial {
    void begin(int) {}
    void println(const char *) {}
    template<typename... Args> void printf(const char *, Args...) {}
};
inline TestSerial Serial;
inline uint32_t micros() { return testMicros; }
inline uint32_t millis() { return testMicros / 1000; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return testDrdy; }
inline void delay(unsigned long ms) { testMicros += ms * 1000; }
inline void delayMicroseconds(unsigned int) {}
inline void yield() {}
inline bool ledcAttach(int, unsigned int, unsigned int) { ++testClockStarts; return true; }
inline bool ledcWrite(int, unsigned int) { return true; }
