/**
 * ADS131M08 BLE streaming with verified runtime PGA control.
 *
 * ADC/SPI, NVS, and transition handling run only in loop(). BLE callbacks
 * enqueue commands. See README.md for the wire protocol and host handshake.
 * Requires Arduino-ESP32 3.x and a BLE-capable ESP32 board.
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

constexpr int PIN_CLK = 1, PIN_CS = 10, PIN_DRDY = 9;
constexpr int PIN_MOSI = 11, PIN_MISO = 13, PIN_SCLK = 12;
constexpr ADS131_Gain_t DEFAULT_GAIN = ADS131_GAIN_32X;
constexpr ADS131_OSR_t ADC_OSR = ADS131_OSR_8192;
constexpr uint32_t MCLK_HZ = 8000000;
constexpr uint32_t SEND_INTERVAL_MS = 10;
constexpr uint32_t ADC_TIMEOUT_US = 100000;

// TI datasheet Table 8-3, in CLKIN periods. Round up to whole microseconds.
constexpr uint32_t SETTLE_CLOCKS[] = {856, 1112, 1624, 2648, 4696, 8792, 16984, 33368};
constexpr uint32_t SETTLE_US =
    (SETTLE_CLOCKS[ADC_OSR] * 1000000ULL + MCLK_HZ - 1) / MCLK_HZ;

constexpr char SERVICE_UUID[] = "12345678-1234-5678-1234-56789abcdef0";
constexpr char DATA_UUID[] = "abcdef01-1234-5678-1234-56789abcdef0";
constexpr char CONFIG_UUID[] = "abcdef02-1234-5678-1234-56789abcdef0";

struct __attribute__((packed)) DataPacket {
    uint32_t packetId;
    int32_t ch[ADS131_NUM_CHANNELS];
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

ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK);
Preferences preferences;
QueueHandle_t gainRequests;
BLECharacteristic *dataCharacteristic, *configCharacteristic;
BLEServer *server;
std::atomic<bool> connected{false}, newConnection{false}, advertiseAgain{false};
std::atomic<bool> refreshConfig{false};

enum class Phase { Settling, AwaitFresh, Ready, Fault };
Phase phase = Phase::Fault;
GainConfig config = {0, 0, 1, 0, 0.0f};
DataPacket packet = {};
bool nvsReady = false, saveRequested = false;
uint8_t savedGain = 0xFF;
uint32_t transitionStartedUs = 0, lastSendMs = 0, disconnectedMs = 0;
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

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        newConnection.store(true);
        connected.store(true);
    }
    void onDisconnect(BLEServer *) override {
        connected.store(false);
        xQueueReset(gainRequests);
        advertiseAgain.store(true);
    }
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

void setup() {
    Serial.begin(115200);
    gainRequests = xQueueCreate(1, sizeof(uint8_t));
    if (gainRequests == nullptr || !adc.begin() || !adc.setOSR(ADC_OSR)) {
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
    configCharacteristic = service->createCharacteristic(
        CONFIG_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
                         BLECharacteristic::PROPERTY_NOTIFY);
    configCharacteristic->addDescriptor(new BLE2902());
    configCharacteristic->setCallbacks(new ConfigCallbacks());

    startGainChange(savedGain <= 7 ? savedGain : static_cast<uint8_t>(DEFAULT_GAIN), false);
    service->start();
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    // Common to Bluedroid and NimBLE; the central chooses the actual interval.
    advertising->setMinPreferred(8);  // 10 ms, in 1.25 ms units
    advertising->setMaxPreferred(12); // 15 ms
    BLEDevice::startAdvertising();
    Serial.printf("Waiting for BLE client; gain settling guard=%lu us\n",
                  static_cast<unsigned long>(SETTLE_US));
}

void loop() {
    if (refreshConfig.exchange(false)) publishConfig(phase == Phase::Ready, false);
    if (newConnection.exchange(false)) packet.packetId = 0;
    if (advertiseAgain.exchange(false)) {
        disconnectedMs = millis();
        waitingToAdvertise = true;
    }
    if (waitingToAdvertise && millis() - disconnectedMs >= 500) {
        if (!connected.load()) server->startAdvertising();
        waitingToAdvertise = false;
    }

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
            memcpy(packet.ch, discard.ch, sizeof(packet.ch));
            dataCharacteristic->setValue(reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
            if (saveRequested && nvsReady && savedGain != adc.currentGain()) {
                if (preferences.putUChar("gain", static_cast<uint8_t>(adc.currentGain())) == 1) {
                    savedGain = static_cast<uint8_t>(adc.currentGain());
                } else {
                    Serial.println("PGA verified, but NVS save failed");
                }
            }
            phase = Phase::Ready;
            publishConfig(true, saveRequested);
        }
        return;
    }
    if (phase != Phase::Ready) return;

    ADS131M08_Data latest;
    bool fresh = false;
    for (int i = 0; i < 3 && adc.isDataReady(); ++i) {
        if (adc.readData(latest)) fresh = true;
    }
    const uint32_t now = millis();
    if (fresh && connected.load() && now - lastSendMs >= SEND_INTERVAL_MS) {
        lastSendMs = now;
        ++packet.packetId;
        memcpy(packet.ch, latest.ch, sizeof(packet.ch));
        dataCharacteristic->setValue(reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
        dataCharacteristic->notify();
    }
}
