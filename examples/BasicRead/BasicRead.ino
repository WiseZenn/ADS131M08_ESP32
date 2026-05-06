/**
 * @file Basic_BLE_Stream.ino
 * @brief ADS131M08 basic streaming example tailored for BLE applications.
 */

#include <Arduino.h>
#include "ADS131M08.h"

// Define your ESP32-S3 pins
#define PIN_CLK  1  
#define PIN_DRDY 9  
#define PIN_CS   10 
#define PIN_MOSI 11 
#define PIN_SCLK 12 
#define PIN_MISO 13 

ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK);
ADS131M08_Data adcData;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting ADS131M08 Initialization...");

    if (!adc.begin()) {
        Serial.println("Failed to communicate with ADS131M08! Check wiring.");
        while (1) { delay(100); }
    }
    Serial.println("ADS131M08 initialized successfully!");

    // 1. Set High Gain for small signals (e.g., Load cells, bio-sensors)
    //    The library handles register unlock automatically via begin().
    if (!adc.setGain(ADS131_GAIN_64X)) {
        Serial.println("PGA Gain set failed! Check wiring / power.");
    }

    // 2. Set OSR to reduce output data rate (Crucial for BLE applications!)
    // OSR = 16384 with 8MHz Clock -> Output Data Rate is ~244 Hz.
    // This allows the ESP32 plenty of time (~4ms) to process BLE tasks between samples.
    adc.setOSR(ADS131_OSR_16384);

    // 3. Calibrate offset (Zeroing)
    Serial.println("Calibrating offsets, please wait...");
    adc.calibrate(50);
    Serial.println("Calibration complete!");
}

void loop() {
    // Polling for DRDY
    if (adc.isDataReady()) {
        if (adc.readData(adcData)) {
            // Print Channel 0 Voltage in millivolts
            float ch0_mV = adc.rawToVoltage(adcData.ch[0]) * 1000.0f;
            
            Serial.print("CH0 (mV): ");
            Serial.print(ch0_mV, 4);
            
            Serial.print("\tCH0 Raw: ");
            Serial.println(adcData.ch[0]);

            // In a real application, you would pack `adcData` into a struct 
            // and send it over BLE here.
        }
    }

    // Process other non-blocking tasks
    // BLE.poll(), etc.
}