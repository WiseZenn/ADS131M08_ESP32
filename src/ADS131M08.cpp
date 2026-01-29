/**
 * @file ADS131M08.cpp
 * @brief ADS131M08 8-Channel Delta-Sigma ADC Driver for ESP32.
 * @author WiseZenn
 * @license MIT
 */

#include "ADS131M08.h"
#include "driver/ledc.h"

ADS131M08::ADS131M08(int8_t clk_pin, int8_t cs_pin, int8_t drdy_pin, int8_t mosi_pin, int8_t miso_pin, int8_t sclk_pin)
    : _pin_clk(clk_pin), _pin_cs(cs_pin), _pin_drdy(drdy_pin),
      _pin_mosi(mosi_pin), _pin_miso(miso_pin), _pin_sclk(sclk_pin), _spi(nullptr) {}

ADS131M08::~ADS131M08() {
    if (_spi) {
        _spi->end();
        delete _spi;
        _spi = nullptr;
    }
}

void ADS131M08::_startClock() {
    // Configure High-Speed (8MHz) Clock Output using LEDC
    // This provides the Master Clock (MCLK) required by the ADS131.
    // Note: This implementation uses ESP-IDF specific LEDC API suitable for ESP32/ESP32-S3.
    // Ensure compatibility with your Arduino ESP32 Core version.
    
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_2_BIT, // Lower resolution allows higher freq
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 8000000,          // 8 MHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num       = _pin_clk,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 2, // 50% duty cycle (2 out of 4)
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

void ADS131M08::begin() {
    // 1. Start External Clock
    _startClock();
    delay(100); // Wait for clock to stabilize

    // 2. Initialize Control Pins
    pinMode(_pin_cs, OUTPUT);
    digitalWrite(_pin_cs, HIGH); // Deselect

    pinMode(_pin_drdy, INPUT);

    // 3. Initialize SPI Bus
    // Note: Using FSPI for ESP32-S3. If using classic ESP32, this often maps to VSPI.
    // Adjust SPIClass instantiation if necessary for your board design.
    _spi = new SPIClass(FSPI);
    _spi->begin(_pin_sclk, _pin_miso, _pin_mosi, _pin_cs);
    delay(100);
}

bool ADS131M08::isDataReady() {
    // DRDY goes LOW when new data is ready
    return digitalRead(_pin_drdy) == LOW;
}

int32_t ADS131M08::_signExtend24(uint32_t raw) {
    // If the 24th bit (sign bit) is set, extend the sign to the upper 8 bytes
    if (raw & 0x800000) {
        return (int32_t)(raw | 0xFF000000);
    }
    return (int32_t)raw;
}

bool ADS131M08::readData(ADS131Data &data) {
    if (!isDataReady()) return false;

    // SPI Transaction (4MHz, MSB First, Mode 1)
    _spi->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE1));
    digitalWrite(_pin_cs, LOW);
    
    // Transfer entire frame of 27 bytes (null tx buffer means we send 0x00)
    _spi->transferBytes(nullptr, _rxBuf, ADS131_FRAME_BYTES);
    
    digitalWrite(_pin_cs, HIGH);
    _spi->endTransaction();

    // Parse Response
    // Frame: [Status (24bit)] [Ch0 (24bit)] [Ch1] ... [Ch7]
    // Indices: Status=0,1,2; Ch0=3,4,5; ...
    for (int i = 0; i < ADS131_NUM_CHANNELS; i++) {
        int base = 3 + (i * 3);
        uint32_t raw = ((uint32_t)_rxBuf[base] << 16) |
                       ((uint32_t)_rxBuf[base + 1] << 8) |
                       _rxBuf[base + 2];
        data.ch[i] = _signExtend24(raw);
    }
    return true;
}

float ADS131M08::rawToVoltage(int32_t raw) {
    // V = (Raw Code / 2^23) * Vref
    return (float)raw * (ADS131_V_REF / ADS131_RESOLUTION);
}

float ADS131M08::rawToMillivolts(int32_t raw) {
    return rawToVoltage(raw) * 1000.0f;
}
