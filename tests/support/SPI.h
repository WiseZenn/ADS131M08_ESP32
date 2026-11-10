#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

constexpr int MSBFIRST = 1, SPI_MODE1 = 1;
struct SPISettings {
    SPISettings(uint32_t, int, int) {}
};

// A small pipelined ADS131M08 model, used by the real driver implementation.
class SPIClass {
public:
    std::array<uint16_t, 64> registers{};
    std::array<int32_t, 8> channels{};
    int transfers = 0, begins = 0;
    int corruptReadAddress = -1;
    bool ignoreReset = false;
    uint16_t response = 0;

    void begin(int, int, int, int) { ++begins; }
    void beginTransaction(const SPISettings &) {}
    void endTransaction() {}
    void transferBytes(const uint8_t *tx, uint8_t *rx, size_t size) {
        assert(size == 30);
        ++transfers;
        if (testAutoClearDrdy) testDrdy = HIGH;
        std::memset(rx, 0, size);
        rx[0] = response >> 8;
        rx[1] = response & 0xFF;
        for (size_t i = 0; i < channels.size(); ++i) {
            const uint32_t raw = static_cast<uint32_t>(channels[i]);
            rx[3 + 3*i] = (raw >> 16) & 0xFF;
            rx[4 + 3*i] = (raw >> 8) & 0xFF;
            rx[5 + 3*i] = raw & 0xFF;
        }
        const uint16_t command = (tx[0] << 8) | tx[1];
        const int address = (command >> 7) & 0x3F;
        response = 0;
        if (command == 0x0011) {
            if (!ignoreReset) {
                registers.fill(0);
                registers[0] = 0x2800;
            }
        } else if ((command & 0xE000) == 0x6000) {
            registers[address] = (tx[3] << 8) | tx[4];
        } else if ((command & 0xE000) == 0xA000) {
            response = registers[address];
            if (address == corruptReadAddress) response ^= 1;
        }
    }
};
inline SPIClass SPI;
