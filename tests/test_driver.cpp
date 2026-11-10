#include "ADS131M08.h"
#include <cassert>
#include <cmath>
#include <iostream>

static bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 1e-6f;
}

int main() {
    SPIClass bus;
    ADS131M08 adc(-1, 10, 9, 11, 13, 12, &bus);
    assert(adc.currentGain() == ADS131_GAIN_1X);
    assert(adc.begin());
    assert(bus.begins == 1 && SPI.begins == 0 && testClockStarts == 0);

    // Every gain updates both registers and the voltage conversion together.
    for (uint32_t code = 0; code < 8; ++code) {
        const auto gain = static_cast<ADS131_Gain_t>(code);
        assert(adc.setGain(gain));
        assert(adc.currentGain() == gain);
        assert(bus.registers[4] == code * 0x1111);
        assert(bus.registers[5] == code * 0x1111);
        assert(near(adc.rawToVoltage(4194304), 0.6f / (1u << code)));
    }

    // Invalid enum casts must not issue any SPI traffic or change the cache.
    const int transfers = bus.transfers;
    for (int code : {-1, 8, 256, 65536}) {
        assert(!adc.setGain(static_cast<ADS131_Gain_t>(code)));
    }
    assert(bus.transfers == transfers);
    assert(adc.currentGain() == ADS131_GAIN_128X);

    // A second-register readback fault must not publish the requested scale.
    bus.corruptReadAddress = ADS131_REG_GAIN2;
    assert(!adc.setGain(ADS131_GAIN_8X));
    assert(adc.currentGain() == ADS131_GAIN_128X);
    assert(near(adc.rawToVoltage(4194304), 0.6f / 128));
    bus.corruptReadAddress = -1;
    assert(adc.setGain(ADS131_GAIN_128X)); // Explicit verified restoration.

    // Preserve a calibration for the same gain; invalidate it on a change.
    bus.channels.fill(100);
    adc.calibrate(3);
    ADS131M08_Data data;
    assert(adc.readData(data) && data.ch[0] == 0);
    assert(adc.setGain(ADS131_GAIN_128X));
    assert(adc.readData(data) && data.ch[0] == 0);
    assert(adc.setGain(ADS131_GAIN_32X));
    assert(adc.readData(data) && data.ch[0] == 100);
    adc.calibrate(3);
    bus.ignoreReset = true;
    assert(!adc.begin()); // A dropped RESET must not publish an incorrect 1X scale.
    assert(adc.currentGain() == ADS131_GAIN_32X);
    bus.ignoreReset = false;
    assert(adc.begin());
    assert(adc.currentGain() == ADS131_GAIN_1X);
    assert(adc.readData(data) && data.ch[0] == 100);
    assert(near(adc.rawToVoltage(4194304), 0.6f));

    bus.channels = {-8388608, -1, 0, 1, 8388607, -100, 100, 256};
    assert(adc.readData(data));
    for (int i = 0; i < ADS131_NUM_CHANNELS; ++i) assert(data.ch[i] == bus.channels[i]);
    testDrdy = HIGH;
    assert(!adc.readData(data));
    std::cout << "Driver gain, readback, calibration, and frame tests passed\n";
}
