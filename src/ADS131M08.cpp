/**
 * @file ADS131M08.cpp
 * @brief Implementation of the ADS131M08 Library.
 */

#include "ADS131M08.h"

ADS131M08::ADS131M08(int8_t clk_pin, int8_t cs_pin, int8_t drdy_pin, int8_t mosi_pin, int8_t miso_pin, int8_t sclk_pin, int8_t reset_pin, SPIClass *spiBus)
    : _pin_clk(clk_pin), _pin_cs(cs_pin), _pin_drdy(drdy_pin),
      _pin_mosi(mosi_pin), _pin_miso(miso_pin), _pin_sclk(sclk_pin),
      _pin_reset(reset_pin),
      _spi(spiBus), _currentGain(ADS131_GAIN_1X),
      _spiSettings(16000000, MSBFIRST, SPI_MODE1) { // Mode 1 is mandatory; 16 MHz SPI (max 25 MHz per datasheet t_C(SC)=40ns @ DVDD 2.7-3.6V)
    memset(_offsets, 0, sizeof(_offsets));
}

ADS131M08::~ADS131M08() {
    _spi = nullptr; // We don't own the SPI object
}

void ADS131M08::_startMasterClock() {
    // Generate 8MHz MCLK using ESP32 LEDC with 1-bit resolution.
    // APB_CLK=80MHz, f_out = 80MHz / (2^1 × divider).
    // divider=5 gives exactly 8.000 MHz (80/10=8), no rounding error.
    // 2-bit resolution would need divider=2.5 (non-integer), giving ±20% error.
    ledcAttach(_pin_clk, 8000000, 1);
    ledcWrite(_pin_clk, 1); // 50% duty (1 out of 0..1)
}

bool ADS131M08::begin() {
    // 1. Start Master Clock (skip if using external clock, clk_pin = -1)
    if (_pin_clk >= 0) {
        _startMasterClock();
        delay(50); // Allow clock and internal 1.8V LDO to stabilize
    }

    // 2. Initialize GPIOs
    pinMode(_pin_cs, OUTPUT);
    digitalWrite(_pin_cs, HIGH);
    pinMode(_pin_drdy, INPUT); // Do NOT use pull-up, DRDY is actively driven by default
    if (_pin_reset >= 0) {
        pinMode(_pin_reset, OUTPUT);
        digitalWrite(_pin_reset, HIGH); // Idle high (active low)
    }

    // 3. Initialize SPI with user-provided pins
    _spi->begin(_pin_sclk, _pin_miso, _pin_mosi, _pin_cs);
    delay(10);

    // 4. Send Software Reset Command (0x0011)
    memset(_txBuf, 0, ADS131_FRAME_BYTES);
    _txBuf[0] = 0x00;
    _txBuf[1] = 0x11;
    _transferFrame();
    delay(50); // Wait for reset to complete

    // 4b. Send UNLOCK Command (0x0655) to enable register writes
    memset(_txBuf, 0, ADS131_FRAME_BYTES);
    _txBuf[0] = 0x06;
    _txBuf[1] = 0x55;
    _transferFrame();
    delayMicroseconds(50);

    // 5. Verify communication by reading the ID register
    uint16_t idReg = 0;
    if (!readRegister(ADS131_REG_ID, idReg)) {
        return false;
    }
    
    // Check if the fixed bits in the ID register match (Bits 15:12 should be 0010b = 0x2)
    if ((idReg >> 12) != 0x02) {
        return false;
    }

    // 6. Drain FIFO — ADC has been converting during steps 4-5, 2 stale samples accumulated
    drainFIFO();

    return true;
}

void ADS131M08::_transferFrame() {
    _spi->beginTransaction(_spiSettings);
    digitalWrite(_pin_cs, LOW);
    
    // Transfer exactly 30 bytes (10 words * 3 bytes)
    _spi->transferBytes(_txBuf, _rxBuf, ADS131_FRAME_BYTES);
    
    digitalWrite(_pin_cs, HIGH);
    _spi->endTransaction();
}

bool ADS131M08::writeRegister(uint8_t regAddr, uint16_t regData) {
    if (regAddr > 0x3F) return false;
    
    // WREG Command Format: 011a aaaa annn nnnn 
    // a = address, n = number of registers minus 1 (0 for 1 register)
    uint16_t cmd = 0x6000 | (regAddr << 7);
    
    memset(_txBuf, 0, ADS131_FRAME_BYTES);
    
    // Word 0: Command
    _txBuf[0] = (cmd >> 8) & 0xFF;
    _txBuf[1] = cmd & 0xFF;
    _txBuf[2] = 0x00;
    
    // Word 1: Register Data
    _txBuf[3] = (regData >> 8) & 0xFF;
    _txBuf[4] = regData & 0xFF;
    _txBuf[5] = 0x00;
    
    _transferFrame();
    delayMicroseconds(50); // Small delay to allow ADC to process
    return true;
}

bool ADS131M08::readRegister(uint8_t regAddr, uint16_t &regData) {
    if (regAddr > 0x3F) return false;
    
    // RREG Command Format: 101a aaaa annn nnnn 
    uint16_t cmd = 0xA000 | (regAddr << 7);
    
    memset(_txBuf, 0, ADS131_FRAME_BYTES);
    
    // First Frame: Send Read Command
    _txBuf[0] = (cmd >> 8) & 0xFF;
    _txBuf[1] = cmd & 0xFF;
    _txBuf[2] = 0x00;
    _transferFrame();
    delayMicroseconds(50);
    
    // Second Frame: Send NULL (all 0s) to clock out the response
    memset(_txBuf, 0, ADS131_FRAME_BYTES);
    _transferFrame();
    
    // The response is located in Word 0 of the second frame
    regData = (_rxBuf[0] << 8) | _rxBuf[1];
    return true;
}

bool ADS131M08::setGain(ADS131_Gain_t gain) {
    uint16_t g = (uint16_t)gain;
    uint16_t regData = (g << 12) | (g << 8) | (g << 4) | g;

    if (!writeRegister(ADS131_REG_GAIN1, regData)) return false;
    if (!writeRegister(ADS131_REG_GAIN2, regData)) return false;

    // Readback verification (pipeline: WREG response arrives in next frame)
    delayMicroseconds(100);
    uint16_t rb1 = 0, rb2 = 0;
    if (readRegister(ADS131_REG_GAIN1, rb1) &&
        readRegister(ADS131_REG_GAIN2, rb2)) {
        if (rb1 == regData && rb2 == regData) {
            _currentGain = gain;
            // Gain change resets digital filter — discard unsettled samples
            drainFIFO();
            return true;
        }
    }
    return false;
}

bool ADS131M08::setOSR(ADS131_OSR_t osr) {
    uint16_t clockReg = 0;
    if (!readRegister(ADS131_REG_CLOCK, clockReg)) return false;

    // Clear OSR bits [4:2] and set new OSR
    clockReg &= ~(0b111 << 2);
    clockReg |= (osr << 2);

    if (!writeRegister(ADS131_REG_CLOCK, clockReg)) return false;

    // OSR change resets digital filter — discard unsettled samples
    drainFIFO();
    return true;
}

bool ADS131M08::isDataReady() {
    return digitalRead(_pin_drdy) == LOW;
}

int32_t ADS131M08::_signExtend24(uint32_t raw) {
    // Extend 24-bit two's complement to 32-bit
    if (raw & 0x800000) {
        return (int32_t)(raw | 0xFF000000);
    }
    return (int32_t)raw;
}

bool ADS131M08::readData(ADS131M08_Data &data) {
    if (!isDataReady()) return false;

    // Send NULL command to read data
    memset(_txBuf, 0, ADS131_FRAME_BYTES);
    _transferFrame();

    // Parse channel data starting from Word 1 (byte 3)
    for (int i = 0; i < ADS131_NUM_CHANNELS; i++) {
        int base = 3 + (i * 3);
        uint32_t raw = ((uint32_t)_rxBuf[base] << 16) |
                       ((uint32_t)_rxBuf[base + 1] << 8) |
                       _rxBuf[base + 2];
        
        data.ch[i] = _signExtend24(raw) - _offsets[i];
    }
    return true;
}

void ADS131M08::calibrate(uint16_t samples) {
    long sums[ADS131_NUM_CHANNELS] = {0};
    memset(_offsets, 0, sizeof(_offsets));

    // Drain stale samples before calibration
    drainFIFO();

    for (uint16_t k = 0; k < samples; k++) {
        // Use yield() instead of delay(1) to prevent RTOS watchdog trigger 
        // and to avoid missing fast DRDY pulses.
        while (!isDataReady()) { yield(); } 
        
        ADS131M08_Data tempData;
        readData(tempData); 
        
        for (int i = 0; i < ADS131_NUM_CHANNELS; i++) {
            sums[i] += tempData.ch[i];
        }
    }

    for (int i = 0; i < ADS131_NUM_CHANNELS; i++) {
        _offsets[i] = sums[i] / samples;
    }
}

float ADS131M08::rawToVoltage(int32_t raw) {
    float gainMultiplier = 1.0f;
    switch (_currentGain) {
        case ADS131_GAIN_1X:   gainMultiplier = 1.0f;   break;
        case ADS131_GAIN_2X:   gainMultiplier = 2.0f;   break;
        case ADS131_GAIN_4X:   gainMultiplier = 4.0f;   break;
        case ADS131_GAIN_8X:   gainMultiplier = 8.0f;   break;
        case ADS131_GAIN_16X:  gainMultiplier = 16.0f;  break;
        case ADS131_GAIN_32X:  gainMultiplier = 32.0f;  break;
        case ADS131_GAIN_64X:  gainMultiplier = 64.0f;  break;
        case ADS131_GAIN_128X: gainMultiplier = 128.0f; break;
    }
    return ((float)raw / ADS131_RESOLUTION) * (ADS131_V_REF / gainMultiplier);
}

void ADS131M08::drainFIFO() {
    // Datasheet Section 8.5.1.9.1: "quickly read two data packets when data are
    // read for the first time or after a gap in reading data."
    ADS131M08_Data dummy;
    for (int i = 0; i < 2; i++) {
        // Wait for DRDY, but don't block forever
        unsigned long t0 = millis();
        while (!isDataReady()) {
            if (millis() - t0 > 100) return; // timeout safety
            yield();
        }
        readData(dummy);
    }
}

void ADS131M08::syncReset() {
    if (_pin_reset < 0) return;

    // Full hardware reset: hold low > 2048 t_CLKIN (at 8.192 MHz → > 250 us)
    // This clears FIFO, resets digital filters, AND resets all registers to defaults.
    // After reset, we must re-UNLOCK before any register writes.
    digitalWrite(_pin_reset, LOW);
    delayMicroseconds(300); // >2048 t_CLKIN at 8.192 MHz = 250us
    digitalWrite(_pin_reset, HIGH);

    // Wait for device to be ready (t_REGACQ = 5us after DRDY rising edge)
    delayMicroseconds(10);

    // Re-send UNLOCK command (reset/sync requires re-unlocking)
    memset(_txBuf, 0, ADS131_FRAME_BYTES);
    _txBuf[0] = 0x06;
    _txBuf[1] = 0x55;
    _transferFrame();
    delayMicroseconds(50);

    // Drain FIFO — first 2 samples after sync use fast-settling filter
    drainFIFO();
}