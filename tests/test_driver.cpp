#include "ADS131M08.h"
#include <cassert>
#include <cmath>
#include <iostream>

static bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 1e-6f;
}

static SPIClass *resetBus = nullptr;
static bool resetLow = false;
static int resetPulses = 0;

static void emulateResetPin(int pin, int level) {
    if (pin != 4) return;
    if (level == LOW) resetLow = true;
    else if (resetLow) {
        resetLow = false;
        ++resetPulses;
        resetBus->registers.fill(0);
        resetBus->registers[0] = 0x2800;
        resetBus->response = 0;
    }
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

    // Clear both FIFO entries even when DRDY deasserts after the first frame.
    testAutoClearDrdy = true;
    const int beforeDrain = bus.transfers;
    const uint32_t beforeDrainTime = micros();
    adc.drainFIFO();
    assert(bus.transfers == beforeDrain + 2 && micros() == beforeDrainTime);
    testAutoClearDrdy = false;

    // No reset pin means no GPIO/SPI work and no change to verified gain.
    testDrdy = LOW;
    assert(adc.setGain(ADS131_GAIN_8X));
    const int beforeReset = bus.transfers;
    adc.syncReset();
    assert(bus.transfers == beforeReset && adc.currentGain() == ADS131_GAIN_8X);

    // New reset-pin forms coexist with the unchanged custom-SPI constructor.
    ADS131M08 gpioZero(-1, 10, 9, 11, 13, 12, 0);
    ADS131M08 explicitBus(-1, 10, 9, 11, 13, 12, -1, &bus);
    const int beforeExplicitBegin = bus.begins;
    assert(explicitBus.begin() && bus.begins == beforeExplicitBegin + 1);
    ADS131M08 withReset(-1, 10, 9, 11, 13, 12, 4, &bus);
    resetBus = &bus;
    testPinWrite = emulateResetPin;
    assert(withReset.begin());
    assert(withReset.setGain(ADS131_GAIN_32X));
    withReset.calibrate(3);
    assert(withReset.readData(data) && data.ch[0] == 0);
    withReset.syncReset();
    assert(resetPulses == 1 && withReset.currentGain() == ADS131_GAIN_1X);
    assert(near(withReset.rawToVoltage(4194304), 0.6f));
    assert(withReset.readData(data) && data.ch[0] == bus.channels[0]);
    testPinWrite = nullptr;
    std::cout << "Driver gain, readback, calibration, and frame tests passed\n";
}
