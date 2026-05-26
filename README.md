# ADS131M08 Driver for ESP32

[![Arduino](https://img.shields.io/badge/Arduino-ESP32-blue.svg)](https://github.com/espressif/arduino-esp32)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-2.1.0-orange.svg)](library.properties)

[🇬🇧 English](README.md) | [🇨🇳 中文](README_CN.md)

High-performance Arduino library for the **Texas Instruments ADS131M08** (8-Channel, 24-bit Delta-Sigma ADC).

> **Key Feature**: This library generates the required 8 MHz master clock (MCLK) directly using the ESP32's **LEDC (PWM)** peripheral, eliminating the need for an external crystal oscillator. For boards with an external clock, pass `clk_pin = -1` to skip internal clock generation.

## Features

- **Programmable Gain Amplifier (PGA)**: 1x, 2x, 4x, 8x, 16x, 32x, 64x, and 128x gain configurations with automatic readback verification.
- **Oversampling Ratio (OSR)**: Configurable from 128 to 16384. Higher OSR = lower noise, lower data rate.
- **Zero Offset Calibration**: Built-in function to dynamically calibrate output at 0V.
- **8-Channel Simultaneous Sampling**: Strict 30-byte SPI frame structure per TI datasheet.
- **24-bit High Resolution**: Full 24-bit data parsing with sign-extension to 32-bit.
- **Cross-Platform SPI**: Uses Arduino's global `SPI` object via dependency injection — works on ESP32, ESP32-S2, ESP32-S3, ESP32-C3, and other Arduino platforms.
- **External Clock Support**: Pass `clk_pin = -1` to use an external clock source instead of LEDC-generated MCLK.
- **Low-level Register Access**: `writeRegister()` and `readRegister()` for full hardware control.

## Wiring

### Pin Mapping (ESP32-S3 Example)

| ADS131 Pin | ESP32-S3 Pin | Description |
| :--- | :--- | :--- |
| **AVDD/DVDD** | 3.3V | Power Supply |
| **GND** | GND | Ground |
| **MCLK** | GPIO 1 | Clock Input (from ESP32 LEDC or external) |
| **SCLK** | GPIO 12 | SPI Clock |
| **MOSI (DIN)** | GPIO 11 | SPI Data In |
| **MISO (DOUT)**| GPIO 13 | SPI Data Out |
| **CS** | GPIO 10 | Chip Select |
| **DRDY** | GPIO 9 | Data Ready |

*Pins are fully configurable in the constructor.*

## Usage

### Basic Example

```cpp
#include <Arduino.h>
#include "ADS131M08.h"

// Define your pins
#define PIN_CLK  1
#define PIN_DRDY 9
#define PIN_CS   10
#define PIN_MOSI 11
#define PIN_SCLK 12
#define PIN_MISO 13

// Uses the default global &SPI object (auto-correct for each ESP32 variant)
ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK);
ADS131M08_Data adcData;

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (!adc.begin()) {
        Serial.println("Failed to communicate with ADS131M08! Check wiring.");
        while (1) { delay(100); }
    }
    Serial.println("ADS131M08 initialized successfully!");

    // Set gain for small signals (e.g., load cells, bio-sensors)
    if (!adc.setGain(ADS131_GAIN_64X)) {
        Serial.println("PGA Gain set failed! Check wiring / power.");
    }

    // Set OSR to balance noise and sample rate
    // OSR=16384 at 8 MHz CLKIN -> ~244 SPS
    adc.setOSR(ADS131_OSR_16384);

    // Calibrate zero offset
    Serial.println("Calibrating offsets, please wait...");
    adc.calibrate(50);
    Serial.println("Calibration complete!");
}

void loop() {
    if (adc.isDataReady()) {
        if (adc.readData(adcData)) {
            float ch0_mV = adc.rawToVoltage(adcData.ch[0]) * 1000.0f;
            Serial.print("CH0 (mV): ");
            Serial.print(ch0_mV, 4);
            Serial.print("\tCH0 Raw: ");
            Serial.println(adcData.ch[0]);
        }
    }
}
```

### External Clock

If your board has an external clock source (e.g., 8.192 MHz crystal), pass `clk_pin = -1` to skip the internal LEDC clock generation:

```cpp
// clk_pin = -1: skip MCLK generation, use external clock
ADS131M08 adc(-1, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK);
```

### Custom SPI Bus

The default `spiBus` parameter is `&SPI`, which the Arduino core auto-creates for each board. To use a different SPI bus, pass it explicitly:

```cpp
// Use a specific SPI bus
ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK, &SPI1);
```

## Data Rate Reference

Formula (Datasheet Equation 5):

```
f_MOD  = f_CLKIN / 2
f_DATA = f_MOD / OSR
```

At f_CLKIN = 8.192 MHz (f_MOD = 4.096 MHz):

| OSR | Data Rate | Use Case |
|:---:|:---:|:---|
| 128 | 32 kSPS | High-speed acquisition |
| 256 | 16 kSPS | Fast sampling |
| 512 | 8 kSPS | General purpose |
| 1024 | 4 kSPS | Default (reset value) |
| 2048 | 2 kSPS | Low noise |
| 4096 | 1 kSPS | Very low noise |
| 8192 | 500 SPS | Ultra-low noise |
| 16384 | 250 SPS | BLE/WiFi streaming |

## SPI Clock Considerations

The default SPI clock is **16 MHz** (max 25 MHz per datasheet at 3.3V DVDD).

Each SPI frame is 10 words × 24 bits = **240 clock cycles**. At 16 MHz, one frame takes **15 us**, well within the 31.25 us window of 32 kSPS (OSR=128).

If using jumper wires or breadboard, keep wires short to avoid signal integrity issues at high SPI frequencies.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for version history.

## License

This project is licensed under the MIT License.
