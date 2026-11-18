// Compile the real sketch against deterministic SPI, clock, GATT, and NVS fakes.
#include "../examples/BLEGainControl/BLEGainControl.ino"
#include <cassert>
#include <iostream>
#include <limits>
#include <new>

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
    // Every ADC sample survives the old 10 ms throttle in capture mode.
    server->callbacks->onConnect(server);
    setCaptureSubscribed(true);
    const auto beforeCapture = sampleSequence;
    for (int i = 1; i <= 5; ++i) {
        testMicros += SAMPLE_PERIOD_US;
        SPI.channels[0] = i;
        testDrdy = LOW;
        loop();
    }
    assert(pendingCount == 5 && captureCharacteristic->notifications == 0);

    // Congestion must retain the batch, not overwrite its older samples.
    captureCharacteristic->refuseNotify = true;
    testMicros += SAMPLE_PERIOD_US;
    SPI.channels[0] = 6;
    testDrdy = LOW;
    loop();
    assert(pendingCount == 6 && notifyRetries == 1);
    captureCharacteristic->refuseNotify = false;
    for (int i = 7; i <= 11; ++i) {
        testMicros += SAMPLE_PERIOD_US;
        SPI.channels[0] = i;
        testDrdy = LOW;
        loop();
    }
    assert(captureCharacteristic->notifications == 1 && captureAccepted == 6);
    assert(captureCharacteristic->value.size() == 240);
    for (int i = 0; i < 6; ++i) {
        CaptureSample sample;
        memcpy(&sample, captureCharacteristic->value.data() + i * sizeof(sample), sizeof(sample));
        assert(sample.sequence == beforeCapture + i + 1 && sample.ch[0] == i + 1);
    }

    // A smaller peer MTU reduces the batch size; no oversized notify.
    server->mtu = 83;
    serviceStream();
    assert(captureCharacteristic->value.size() == 80);

    // The FIFO is bounded and the number of newly dropped ADC records is visible.
    for (size_t i = 0; i < CAPTURE_QUEUE_DEPTH + 5; ++i) {
        testMicros += SAMPLE_PERIOD_US;
        testDrdy = LOW;
        serviceAdc();
    }
    assert(uxQueueMessagesWaiting(captureQueue) == CAPTURE_QUEUE_DEPTH);
    assert(queueDrops.load() > 0);
    configCharacteristic->write({4});
    serviceAdc();
    assert(uxQueueMessagesWaiting(captureQueue) == 0 && pendingCount == 0);
    assert(!streamReady.load());
    finishTransition();

    const auto errorsBefore = readErrors.load();
    const auto recoveriesBefore = fifoResyncs.load();
    const auto lastReadBefore = lastReadUs;
    adc.~ADS131M08();
    new (&adc) ADS131M08(-1, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK,
                        static_cast<SPIClass *>(nullptr));
    testMicros += 2 * SAMPLE_PERIOD_US;
    testDrdy = LOW;
    serviceAdc();
    assert(readErrors.load() == errorsBefore + 1);
    assert(fifoResyncs.load() == recoveriesBefore && lastReadUs == lastReadBefore);

    adc.~ADS131M08();
    new (&adc) ADS131M08(-1, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK);
    assert(adc.begin());
    const int beforeRetry = SPI.transfers;
    testDrdy = HIGH;
    serviceAdc();
    assert(SPI.transfers == beforeRetry + 2);
    assert(fifoResyncs.load() == recoveriesBefore + 1 && lastReadUs == micros());
    std::cout << "Firmware tests passed, including ordered ADC capture, retry, MTU and overflow\n";
}
