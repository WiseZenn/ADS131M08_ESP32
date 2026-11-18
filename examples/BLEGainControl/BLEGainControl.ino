/**
 * ADS131M08 BLE streaming with verified runtime PGA control.
 *
 * One DRDY-driven task owns the ADC and gain transitions. loop() sends BLE
 * notifications from bounded buffers. See README.md for both stream formats.
 * Tested with Arduino-ESP32 3.3.11.
 */
#include <Arduino.h>
#include <ADS131M08.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/task.h>
#endif
#ifndef ADS131_BLE_USE_DMA
#define ADS131_BLE_USE_DMA 0
#endif
#ifndef ADS131_BLE_LIVE_INTERVAL_US
#define ADS131_BLE_LIVE_INTERVAL_US 10000
#endif

constexpr int PIN_CLK = 1, PIN_CS = 10, PIN_DRDY = 9;
constexpr int PIN_MOSI = 11, PIN_MISO = 13, PIN_SCLK = 12;
constexpr ADS131_Gain_t DEFAULT_GAIN = ADS131_GAIN_32X;
constexpr ADS131_OSR_t ADC_OSR = ADS131_OSR_8192;
constexpr uint32_t MCLK_HZ = 8000000;
constexpr uint32_t LIVE_INTERVAL_US = ADS131_BLE_LIVE_INTERVAL_US;
constexpr uint32_t SAMPLE_PERIOD_US = (2ULL * (128u << ADC_OSR) * 1000000) / MCLK_HZ;
constexpr uint32_t BATCH_WAIT_US = 12000;
constexpr uint32_t RETRY_US = 10000;
constexpr size_t CAPTURE_QUEUE_DEPTH = 128;
constexpr size_t MAX_BATCH = 6;
constexpr uint32_t ADC_TIMEOUT_US = 100000;

// TI datasheet Table 8-3, in CLKIN periods. Round up to whole microseconds.
constexpr uint32_t SETTLE_CLOCKS[] = {856, 1112, 1624, 2648, 4696, 8792, 16984, 33368};
constexpr uint32_t SETTLE_US =
    (SETTLE_CLOCKS[ADC_OSR] * 1000000ULL + MCLK_HZ - 1) / MCLK_HZ;

constexpr char SERVICE_UUID[] = "12345678-1234-5678-1234-56789abcdef0";
constexpr char DATA_UUID[] = "abcdef01-1234-5678-1234-56789abcdef0";
constexpr char CONFIG_UUID[] = "abcdef02-1234-5678-1234-56789abcdef0";
constexpr char CAPTURE_UUID[] = "abcdef03-1234-5678-1234-56789abcdef0";

struct __attribute__((packed)) DataPacket {
    uint32_t packetId;
    int32_t ch[ADS131_NUM_CHANNELS];
};

struct CaptureSample {
    uint32_t sequence;
    uint32_t readUs;
    int32_t ch[ADS131_NUM_CHANNELS];
};

struct QueuedSample {
    CaptureSample sample;
    uint32_t subscription; // Identifies the peer's current capture subscription.
};

struct __attribute__((packed)) GainConfig {
    uint8_t gainCode;
    uint8_t gainOk;
    uint8_t protocolVersion; // 1: completion notification follows a settled sample
    uint8_t flags;           // bit 0: this gain is saved in NVS
    float mvPerLsb;
};
static_assert(sizeof(DataPacket) == 36, "Data packet must remain 36 bytes");
static_assert(sizeof(GainConfig) == 8, "Gain config must remain 8 bytes");
static_assert(sizeof(CaptureSample) == 40, "Capture record must be 40 bytes");

ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK);
Preferences preferences;
QueueHandle_t gainRequests, latestSample, captureQueue;
SemaphoreHandle_t streamLock;
BLECharacteristic *dataCharacteristic, *configCharacteristic, *captureCharacteristic;
#if defined(ARDUINO_ARCH_ESP32)
TaskHandle_t adcTask;
#endif
BLEServer *server;
std::atomic<bool> connected{false}, newConnection{false}, advertiseAgain{false};
std::atomic<bool> refreshConfig{false}, streamReady{false};
// Odd values are subscribed. Increment on each state change so queued
// records from a prior subscription cannot leak into a later one.
std::atomic<uint32_t> captureSubscription{0};
std::atomic<uint32_t> samplesRead{0}, readErrors{0}, fifoResyncs{0}, queueDrops{0};
uint32_t sampleSequence = 0, captureAccepted = 0, notifyRetries = 0;
uint32_t lastReadUs = 0, lastStatsMs = 0;
bool fifoRecoveryPending = false;
CaptureSample pendingBatch[MAX_BATCH];
size_t pendingCount = 0;
uint32_t pendingSubscription = 0, lastCaptureAttemptUs = 0;
bool captureRetry = false, notifyAccepted = false;

enum class Phase { Settling, AwaitFresh, Ready, Fault };
Phase phase = Phase::Fault;
GainConfig config = {0, 0, 1, 0, 0.0f};
DataPacket packet = {};
bool nvsReady = false, saveRequested = false;
uint8_t savedGain = 0xFF;
uint32_t transitionStartedUs = 0, lastSendUs = 0, disconnectedMs = 0;
bool waitingToAdvertise = false;

void publishConfig(bool verified, bool notify) {
    config.gainCode = static_cast<uint8_t>(adc.currentGain());
    config.gainOk = verified ? 1 : 0;
    config.flags = (nvsReady && savedGain == config.gainCode) ? 1 : 0;
    config.mvPerLsb = (ADS131_V_REF * 1000.0f) /
        (ADS131_RESOLUTION * (1u << config.gainCode));
    configCharacteristic->setValue(reinterpret_cast<uint8_t *>(&config), sizeof(config));
    if (notify && connected.load()) configCharacteristic->notify();
}

void startGainChange(uint8_t code, bool persist) {
    streamReady.store(false);
    // Wait for the one in-progress notify call before touching PGA registers.
    xSemaphoreTake(streamLock, portMAX_DELAY);
    xQueueReset(latestSample);
    xQueueReset(captureQueue);
    pendingCount = 0;
    captureRetry = false;
    xSemaphoreGive(streamLock);
    publishConfig(false, false);
    const ADS131_Gain_t previous = adc.currentGain();
    if (!adc.setGain(static_cast<ADS131_Gain_t>(code))) {
        const bool restored = adc.setGain(previous);
        Serial.printf("PGA verification failed; rollback=%s. Streaming stopped.\n",
                      restored ? "verified" : "FAILED");
        phase = Phase::Fault;
        publishConfig(false, true);
        return;
    }
    saveRequested = persist;
    transitionStartedUs = micros();
    phase = Phase::Settling;
}

void setCaptureSubscribed(bool enabled) {
    const uint32_t state = captureSubscription.load();
    if (static_cast<bool>(state & 1u) != enabled) captureSubscription.store(state + 1);
}

class StreamCallbacks : public BLECharacteristicCallbacks {
    void onStatus(BLECharacteristic *, Status status, uint32_t) override {
        // Arduino-ESP32 reports submission status synchronously in notify().
        notifyAccepted = status == SUCCESS_NOTIFY;
    }
#if defined(CONFIG_NIMBLE_ENABLED)
    void onSubscribe(BLECharacteristic *characteristic, ble_gap_conn_desc *,
                     uint16_t subscription) override {
        if (characteristic == captureCharacteristic) setCaptureSubscribed(subscription & 1u);
    }
#endif
};

#if defined(CONFIG_BLUEDROID_ENABLED)
class CaptureDescriptorCallbacks : public BLEDescriptorCallbacks {
    void onWrite(BLEDescriptor *descriptor) override {
        setCaptureSubscribed(static_cast<BLE2902 *>(descriptor)->getNotifications());
    }
};
#endif

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        xQueueReset(latestSample);
        newConnection.store(true);
        connected.store(true);
    }
    void onDisconnect(BLEServer *) override {
        connected.store(false);
        setCaptureSubscribed(false);
        xQueueReset(gainRequests);
        advertiseAgain.store(true);
    }
#if defined(CONFIG_NIMBLE_ENABLED)
    void onConnect(BLEServer *peer, ble_gap_conn_desc *desc) override {
        peer->requestConnParams(desc->conn_handle, 6, 8, 0, 100); // 7.5–10 ms
        ble_gap_set_data_len(desc->conn_handle, 251, 2120); // Request DLE.
    }
#elif defined(CONFIG_BLUEDROID_ENABLED)
    void onConnect(BLEServer *peer, esp_ble_gatts_cb_param_t *param) override {
        peer->requestConnParams(param->connect.remote_bda, 6, 8, 0, 100);
        esp_ble_gap_set_pkt_data_len(param->connect.remote_bda, 251);
    }
#endif
};

class ConfigCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        const auto value = characteristic->getValue();
        if (value.length() != 1 || static_cast<uint8_t>(value[0]) > 7) {
            Serial.println("PGA command rejected: expected one gain code (0..7)");
            refreshConfig.store(true); // Restore the readable value in loop().
            return;
        }
        const uint8_t code = static_cast<uint8_t>(value[0]);
        // Latest pending request wins. Hosts must serialize requests and await
        // completion; this queue also avoids a cross-task volatile read/reset race.
        xQueueOverwrite(gainRequests, &code);
    }
};

void serviceAdc();

#if defined(ARDUINO_ARCH_ESP32)
void IRAM_ATTR onDataReady() {
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(adcTask, &wake);
    if (wake) portYIELD_FROM_ISR();
}

void acquisitionTask(void *) {
    const TickType_t timeout = pdMS_TO_TICKS(5);
    for (;;) {
        // The timeout services gain changes and recovers after missed edges.
        ulTaskNotifyTake(pdTRUE, timeout == 0 ? 1 : timeout);
        serviceAdc();
    }
}
#endif

void setup() {
    Serial.begin(115200);
    gainRequests = xQueueCreate(1, sizeof(uint8_t));
    latestSample = xQueueCreate(1, sizeof(CaptureSample));
    captureQueue = xQueueCreate(CAPTURE_QUEUE_DEPTH, sizeof(QueuedSample));
    streamLock = xSemaphoreCreateMutex();
#if ADS131_BLE_USE_DMA && defined(ARDUINO_ARCH_ESP32)
    const bool adcStarted = adc.beginDMA();
#else
    const bool adcStarted = adc.begin();
#endif
    if (!gainRequests || !latestSample || !captureQueue || !streamLock ||
        !adcStarted || !adc.setOSR(ADC_OSR)) {
        Serial.println("ADC/queue initialization failed");
        while (true) delay(100);
    }
    uint16_t clockReg = 0;
    if (!adc.readRegister(ADS131_REG_CLOCK, clockReg) ||
        ((clockReg >> 2) & 7) != ADC_OSR) {
        Serial.println("OSR readback failed");
        while (true) delay(100);
    }

    nvsReady = preferences.begin("ads131m08", false);
    if (nvsReady) savedGain = preferences.getUChar("gain", 0xFF);
    else Serial.println("NVS unavailable; gain changes will be volatile");

    BLEDevice::init("ESP32S3");
    BLEDevice::setMTU(256); // Central must also negotiate ATT MTU >= 39.
    server = BLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    BLEService *service = server->createService(SERVICE_UUID);
    dataCharacteristic = service->createCharacteristic(
        DATA_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    dataCharacteristic->addDescriptor(new BLE2902());
    dataCharacteristic->setCallbacks(new StreamCallbacks());
    captureCharacteristic = service->createCharacteristic(CAPTURE_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    captureCharacteristic->setCallbacks(new StreamCallbacks());
    auto *captureDescriptor = new BLE2902();
#if defined(CONFIG_BLUEDROID_ENABLED)
    captureDescriptor->setCallbacks(new CaptureDescriptorCallbacks());
#endif
    captureCharacteristic->addDescriptor(captureDescriptor);
    configCharacteristic = service->createCharacteristic(
        CONFIG_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
                         BLECharacteristic::PROPERTY_NOTIFY);
    configCharacteristic->addDescriptor(new BLE2902());
    configCharacteristic->setCallbacks(new ConfigCallbacks());

    service->start();
    startGainChange(savedGain <= 7 ? savedGain : static_cast<uint8_t>(DEFAULT_GAIN), false);
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    // Common to Bluedroid and NimBLE; the central chooses the actual interval.
    advertising->setMinPreferred(6);  // 7.5 ms, in 1.25 ms units
    advertising->setMaxPreferred(8);  // 10 ms
#if defined(ARDUINO_ARCH_ESP32)
    if (xTaskCreatePinnedToCore(acquisitionTask, "ads131-read", 4096, nullptr, 3,
                                &adcTask, xPortGetCoreID()) != pdPASS) {
        Serial.println("ADC task creation failed");
        while (true) delay(100);
    }
    attachInterrupt(digitalPinToInterrupt(PIN_DRDY), onDataReady, FALLING);
    xTaskNotifyGive(adcTask); // Service a DRDY already low during setup.
#endif
    BLEDevice::startAdvertising();
    Serial.printf("ADC period=%lu us, DMA=%d, gain settling guard=%lu us\n",
                  static_cast<unsigned long>(SAMPLE_PERIOD_US), ADS131_BLE_USE_DMA,
                  static_cast<unsigned long>(SETTLE_US));
}

void serviceAdc() {
    if (refreshConfig.exchange(false)) publishConfig(phase == Phase::Ready, false);

    uint8_t code;
    if ((phase == Phase::Ready || phase == Phase::Fault) &&
        xQueueReceive(gainRequests, &code, 0) == pdTRUE) {
        startGainChange(code, true);
    }

    if (phase == Phase::Settling || phase == Phase::AwaitFresh) {
        if (micros() - transitionStartedUs > ADC_TIMEOUT_US) {
            phase = Phase::Fault;
            Serial.println("No settled ADC sample within timeout; streaming stopped");
            publishConfig(false, true);
            return;
        }
        ADS131M08_Data discard;
        if (phase == Phase::Settling) {
            // Drain the two-entry ADC FIFO throughout settling. A high DRDY
            // after the guard gives a boundary before the next falling edge.
            for (int i = 0; i < 3 && adc.isDataReady(); ++i) adc.readData(discard);
            if (micros() - transitionStartedUs >= SETTLE_US && !adc.isDataReady()) {
                phase = Phase::AwaitFresh;
            }
        } else if (adc.readData(discard)) {
            // Only this new DRDY sample completes the transition, not the
            // register readback. Never deliver unsettled frames to the host.
            DataPacket settled = {};
            memcpy(settled.ch, discard.ch, sizeof(settled.ch));
            dataCharacteristic->setValue(reinterpret_cast<uint8_t *>(&settled), sizeof(settled));
            if (saveRequested && nvsReady && savedGain != adc.currentGain()) {
                if (preferences.putUChar("gain", static_cast<uint8_t>(adc.currentGain())) == 1) {
                    savedGain = static_cast<uint8_t>(adc.currentGain());
                } else {
                    Serial.println("PGA verified, but NVS save failed");
                }
            }
            phase = Phase::Ready;
            fifoRecoveryPending = false;
            lastReadUs = micros();
            publishConfig(true, saveRequested);
            streamReady.store(true);
        }
        return;
    }
    if (phase != Phase::Ready) return;

    if (!fifoRecoveryPending && !adc.isDataReady()) return;
    if (fifoRecoveryPending || micros() - lastReadUs >= 2 * SAMPLE_PERIOD_US) {
        // After a pause the ADC FIFO can contain two old samples. An edge
        // between the two reads can be too narrow to observe (TI §8.5.1.9.1).
        fifoRecoveryPending = !adc.drainFIFO();
        if (fifoRecoveryPending) {
            ++readErrors;
            return;
        }
        ++fifoResyncs;
        lastReadUs = micros();
        return;
    }

    for (int i = 0; i < 3 && adc.isDataReady(); ++i) {
        ADS131M08_Data data;
        if (!adc.readData(data)) {
            ++readErrors;
            break;
        }
        CaptureSample sample;
        sample.sequence = ++sampleSequence;
        sample.readUs = lastReadUs = micros();
        memcpy(sample.ch, data.ch, sizeof(sample.ch));
        ++samplesRead;
        if (!connected.load()) continue;
        xQueueOverwrite(latestSample, &sample);
        const uint32_t subscription = captureSubscription.load();
        if (subscription & 1u) {
            const QueuedSample entry{sample, subscription};
            if (xQueueSend(captureQueue, &entry, 0) != pdTRUE) ++queueDrops;
        }
    }
}

bool submit(BLECharacteristic *characteristic, uint8_t *value, size_t length) {
    notifyAccepted = false;
    characteristic->setValue(value, length);
    characteristic->notify();
    return notifyAccepted;
}

void sendCapture(uint32_t now, uint16_t mtu, uint32_t subscription) {
    if (mtu < sizeof(CaptureSample) + 3) return;
    const size_t fit = (mtu - 3) / sizeof(CaptureSample);
    const size_t capacity = fit < MAX_BATCH ? fit : MAX_BATCH;
    if (captureRetry && now - lastCaptureAttemptUs < RETRY_US) return;
    QueuedSample entry;
    while (pendingCount < capacity && xQueueReceive(captureQueue, &entry, 0) == pdTRUE) {
        if (entry.subscription == subscription) pendingBatch[pendingCount++] = entry.sample;
    }
    if (pendingCount == 0 ||
        (!captureRetry && pendingCount < capacity && micros() - pendingBatch[0].readUs < BATCH_WAIT_US)) return;
    if (captureSubscription.load() != subscription) return;
    lastCaptureAttemptUs = now;
    const size_t count = pendingCount < capacity ? pendingCount : capacity;
    if (submit(captureCharacteristic, reinterpret_cast<uint8_t *>(pendingBatch),
               count * sizeof(CaptureSample))) {
        captureAccepted += count;
        pendingCount -= count;
        memmove(pendingBatch, pendingBatch + count, pendingCount * sizeof(CaptureSample));
        captureRetry = false;
    } else {
        // Reference remains valid until the next attempt, gain change, or unsubscribe.
        ++notifyRetries;
        captureRetry = true;
    }
}

void serviceStream() {
    if (newConnection.exchange(false)) {
        packet.packetId = 0;
        lastSendUs = micros() - LIVE_INTERVAL_US;
    }
    if (advertiseAgain.exchange(false)) {
        disconnectedMs = millis();
        waitingToAdvertise = true;
    }
    if (waitingToAdvertise && millis() - disconnectedMs >= 500) {
        if (!connected.load()) server->startAdvertising();
        waitingToAdvertise = false;
    }


    if (!connected.load() || xSemaphoreTake(streamLock, 0) != pdTRUE) return;
    if (streamReady.load()) {
        const uint32_t now = micros();
        const uint16_t mtu = server->getPeerMTU(server->getConnId());
        const uint32_t subscription = captureSubscription.load();
        if (subscription != pendingSubscription) {
            pendingCount = 0;
            captureRetry = false;
            pendingSubscription = subscription;
            if (!(subscription & 1u)) xQueueReset(captureQueue);
        }
        if (subscription & 1u) {
            sendCapture(now, mtu, subscription);
        } else if (mtu >= sizeof(DataPacket) + 3 && now - lastSendUs >= LIVE_INTERVAL_US) {
            CaptureSample sample;
            if (xQueueReceive(latestSample, &sample, 0) == pdTRUE) {
                const uint32_t sendUs = micros();
                if (sendUs - sample.readUs <= LIVE_INTERVAL_US + 2 * SAMPLE_PERIOD_US) {
                    lastSendUs = sendUs;
                    ++packet.packetId;
                    memcpy(packet.ch, sample.ch, sizeof(packet.ch));
                    submit(dataCharacteristic, reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
                }
            }
        }
        if (millis() - lastStatsMs >= 2000) {
            lastStatsMs = millis();
            Serial.printf("stream mtu=%u adc_read=%lu adc_read_error=%lu fifo_resync=%lu "
                          "capture_queue_drop=%lu ble_retry=%lu capture_accepted=%lu queued=%u\n",
                          mtu, static_cast<unsigned long>(samplesRead.load()),
                          static_cast<unsigned long>(readErrors.load()),
                          static_cast<unsigned long>(fifoResyncs.load()),
                          static_cast<unsigned long>(queueDrops.load()),
                          static_cast<unsigned long>(notifyRetries),
                          static_cast<unsigned long>(captureAccepted),
                          static_cast<unsigned>(uxQueueMessagesWaiting(captureQueue) + pendingCount));
        }
    }
    xSemaphoreGive(streamLock);
}

void loop() {
#if !defined(ARDUINO_ARCH_ESP32)
    // Native regression tests run both owners on a deterministic fake clock.
    serviceAdc();
#endif
    serviceStream();
#if defined(ARDUINO_ARCH_ESP32)
    delay(1);
#endif
}
