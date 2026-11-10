#pragma once
#include <cstddef>
#include <cstdint>
inline bool testNvsAvailable = true, testNvsWriteOk = true;
inline uint8_t testSavedGain = 0xFF;
inline int testNvsWrites = 0;
struct Preferences {
    bool begin(const char *, bool) { return testNvsAvailable; }
    uint8_t getUChar(const char *, uint8_t fallback) {
        return testSavedGain == 0xFF ? fallback : testSavedGain;
    }
    size_t putUChar(const char *, uint8_t value) {
        ++testNvsWrites;
        if (!testNvsWriteOk) return 0;
        testSavedGain = value;
        return 1;
    }
};
