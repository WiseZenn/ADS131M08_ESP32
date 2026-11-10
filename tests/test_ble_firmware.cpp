// Compile the real sketch against deterministic SPI, clock, GATT, and NVS fakes.
#include "../examples/BLEGainControl/BLEGainControl.ino"
#include <cassert>
#include <iostream>
#include <limits>

static void finishTransition() {
    assert(phase == Phase::Settling);
    testDrdy = LOW;
    loop(); // A pending/unsettled ADC frame must be discarded.
    testMicros += SETTLE_US;
    loop();
    assert(phase == Phase::AwaitFresh && config.gainOk == 0);
    testDrdy = LOW;
    loop(); // First new DRDY edge after the filter guard.
    assert(phase == Phase::Ready && config.gainOk == 1);
}

int main() {
    testAutoClearDrdy = true;
    setup();
    assert(SETTLE_US == 2123);
    finishTransition();
    assert(adc.currentGain() == DEFAULT_GAIN && testNvsWrites == 0);
    server->callbacks->onConnect(server);
    testDrdy = LOW;
    loop();
    assert(dataCharacteristic->notifications == 1);

    // No completion or data notification before both settling and fresh DRDY.
    configCharacteristic->write({3});
    loop();
    assert(phase == Phase::Settling && config.gainOk == 0);
    assert(dataCharacteristic->notifications == 1);
    assert(configCharacteristic->notifications == 0);
    finishTransition();
    assert(config.gainCode == 3 && config.flags == 1);
    assert(configCharacteristic->notifications == 1);
    assert(dataCharacteristic->notifications == 1);
    assert(testSavedGain == 3 && testNvsWrites == 1);
    configCharacteristic->write({3});
    loop();
    finishTransition();
    assert(testNvsWrites == 1); // No redundant flash write for the saved gain.

    // Malformed commands neither touch SPI nor leave a malformed read value.
    const int transfers = SPI.transfers;
    configCharacteristic->write({2, 3});
    loop();
    assert(SPI.transfers == transfers && configCharacteristic->value.size() == 8);
    configCharacteristic->write({8});
    loop();
    assert(SPI.transfers == transfers && config.gainCode == 3);

    // Readback/rollback uncertainty stops output until an explicit valid request.
    SPI.corruptReadAddress = ADS131_REG_GAIN2;
    configCharacteristic->write({6});
    loop();
    assert(phase == Phase::Fault && config.gainOk == 0);
    testMicros += 10000;
    testDrdy = LOW;
    loop();
    assert(dataCharacteristic->notifications == 1 && testNvsWrites == 1);
    SPI.corruptReadAddress = -1;
    configCharacteristic->write({2});
    loop();
    finishTransition();

    // NVS failure is visible without misreporting a valid hardware gain.
    testNvsWriteOk = false;
    configCharacteristic->write({4});
    loop();
    finishTransition();
    assert(config.gainCode == 4 && config.flags == 0 && testSavedGain == 2);
    testNvsWriteOk = true;

    configCharacteristic->write({5});
    loop();
    testDrdy = HIGH;
    testMicros += ADC_TIMEOUT_US + 1;
    loop();
    assert(phase == Phase::Fault && config.gainOk == 0);

    // Unsigned elapsed-time guards work across micros() wraparound.
    testMicros = std::numeric_limits<uint32_t>::max() - 1000;
    configCharacteristic->write({1});
    loop();
    finishTransition();
    assert(config.gainCode == 1);

    // Commands queued by a disconnected peer must not leak into a new session.
    configCharacteristic->write({7});
    server->callbacks->onDisconnect(server);
    loop();
    assert(!connected.load() && adc.currentGain() == ADS131_GAIN_2X);
    std::cout << "Firmware settling, failure, persistence, queue, and timing tests passed\n";
}
