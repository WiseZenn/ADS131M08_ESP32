#pragma once
#include <cassert>
#include <string>
#include <vector>

class BLECharacteristic;
class BLEServer;
struct BLECharacteristicCallbacks {
    virtual ~BLECharacteristicCallbacks() = default;
    virtual void onWrite(BLECharacteristic *) {}
};
struct BLEServerCallbacks {
    virtual ~BLEServerCallbacks() = default;
    virtual void onConnect(BLEServer *) {}
    virtual void onDisconnect(BLEServer *) {}
};
class BLECharacteristic {
public:
    enum { PROPERTY_READ = 1, PROPERTY_WRITE = 2, PROPERTY_NOTIFY = 4 };
    BLECharacteristicCallbacks *callbacks = nullptr;
    std::vector<uint8_t> value;
    int notifications = 0;
    void setValue(uint8_t *data, size_t size) { value.assign(data, data + size); }
    std::string getValue() const { return {value.begin(), value.end()}; }
    void notify() { ++notifications; }
    void addDescriptor(void *) {}
    void setCallbacks(BLECharacteristicCallbacks *cb) { callbacks = cb; }
    void write(const std::vector<uint8_t> &data) {
        value = data;
        callbacks->onWrite(this);
    }
};
class BLEService {
public:
    BLECharacteristic *createCharacteristic(const char *, int) { return new BLECharacteristic; }
    void start() {}
};
class BLEAdvertising {
public:
    void addServiceUUID(const char *) {}
    void setScanResponse(bool) {}
    void setMinPreferred(uint16_t) {}
    void setMaxPreferred(uint16_t) {}
};
class BLEServer {
public:
    BLEServerCallbacks *callbacks = nullptr;
    int advertisements = 0;
    void setCallbacks(BLEServerCallbacks *cb) { callbacks = cb; }
    BLEService *createService(const char *) { return new BLEService; }
    void startAdvertising() { ++advertisements; }
};
struct BLEDevice {
    static void init(const char *) {}
    static void setMTU(int) {}
    static BLEServer *createServer() { return new BLEServer; }
    static BLEAdvertising *getAdvertising() { static BLEAdvertising value; return &value; }
    static void startAdvertising() {}
};
