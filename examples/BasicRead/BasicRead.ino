/**
 * @file BasicRead.ino
 * @brief Basic ADS131M08 example with PGA gain, OSR, and calibration.
 *
 * Data rate formula (Datasheet Equation 5):
 *   f_MOD  = f_CLKIN / 2
 *   f_DATA = f_MOD / OSR
 *
 * At 8 MHz CLKIN: OSR=128 -> 32 kSPS, OSR=256 -> 16 kSPS, OSR=1024 -> 4 kSPS
 */

#include <Arduino.h>
#include "ADS131M08.h"

// Define your pins
#define PIN_CLK   1   // Set to -1 if using external clock
#define PIN_DRDY  9
#define PIN_CS   10
#define PIN_MOSI 11
#define PIN_SCLK 12
#define PIN_MISO 13
#define PIN_RESET -1  // SYNC/RESET pin (optional, -1 = not connected)

// Uses default global &SPI — auto-correct for all ESP32 variants
ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK, PIN_RESET);
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

    // 1. Set PGA gain for small signals (e.g., load cells, bio-sensors)
    if (!adc.setGain(ADS131_GAIN_64X)) {
        Serial.println("PGA Gain set failed! Check wiring / power.");
    }

    // 2. Set OSR to control output data rate
    // OSR=16384 at 8 MHz CLKIN -> ~244 SPS (great for BLE/WiFi streaming)
    adc.setOSR(ADS131_OSR_16384);

    // 3. Calibrate zero offset
    Serial.println("Calibrating offsets, please wait...");
    adc.calibrate(50);
    Serial.println("Calibration complete!");
}

void loop() {
    if (adc.isDataReady()) {
        if (adc.readData(adcData)) {
            // Print Channel 0 voltage in millivolts
            float ch0_mV = adc.rawToVoltage(adcData.ch[0]) * 1000.0f;

            Serial.print("CH0 (mV): ");
            Serial.print(ch0_mV, 4);

            Serial.print("\tCH0 Raw: ");
            Serial.println(adcData.ch[0]);
        }
    }
}
