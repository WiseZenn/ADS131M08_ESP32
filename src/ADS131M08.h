/**
 * @file ADS131M08.h
 * @brief Professional Arduino/ESP32 library for the Texas Instruments ADS131M08 8-Channel 24-bit ADC.
 * @version 1.0.0
 * @author WiseZenn (Modified & Optimized)
 * @license MIT
 * 
 * This library enforces the strict 30-byte SPI frame structure required by the 
 * ADS131M08 and provides easy-to-use APIs for configuring Gain, OSR, and calibration.
 */

#ifndef ADS131M08_H
#define ADS131M08_H

#include <Arduino.h>
#include <SPI.h>

#define ADS131_NUM_CHANNELS 8
#define ADS131_FRAME_BYTES  30
#define ADS131_V_REF        1.2f
#define ADS131_RESOLUTION   8388608.0f // 2^23

// --- ADS131M08 Register Map ---
#define ADS131_REG_ID       0x00
#define ADS131_REG_STATUS   0x01
#define ADS131_REG_MODE     0x02
#define ADS131_REG_CLOCK    0x03
#define ADS131_REG_GAIN1    0x04
#define ADS131_REG_GAIN2    0x05

/**
 * @brief Programmable Gain Amplifier (PGA) options.
 */
typedef enum {
    ADS131_GAIN_1X   = 0,
    ADS131_GAIN_2X   = 1,
    ADS131_GAIN_4X   = 2,
    ADS131_GAIN_8X   = 3,
    ADS131_GAIN_16X  = 4,
    ADS131_GAIN_32X  = 5,
    ADS131_GAIN_64X  = 6,
    ADS131_GAIN_128X = 7
} ADS131_Gain_t;

/**
 * @brief Oversampling Ratio (OSR) options. Higher OSR means lower data rate but less noise.
 * Default is 1024. For Bluetooth/Wi-Fi streaming, 8192 or 16384 is highly recommended.
 */
typedef enum {
    ADS131_OSR_128   = 0,
    ADS131_OSR_256   = 1,
    ADS131_OSR_512   = 2,
    ADS131_OSR_1024  = 3, // Default
    ADS131_OSR_2048  = 4,
    ADS131_OSR_4096  = 5,
    ADS131_OSR_8192  = 6,
    ADS131_OSR_16384 = 7
} ADS131_OSR_t;

/**
 * @brief Structure to hold 8-channel ADC data
 */
struct ADS131M08_Data {
    int32_t ch[ADS131_NUM_CHANNELS];
};

class ADS131M08 {
public:
    /**
     * @brief Constructor
     * @param clk_pin  ESP32 pin for 8MHz MCLK. Pass -1 if using external clock source.
     * @param cs_pin   SPI Chip Select (CS) pin
     * @param drdy_pin Data Ready (DRDY) pin
     * @param mosi_pin SPI MOSI pin
     * @param miso_pin SPI MISO pin
     * @param sclk_pin SPI SCLK pin
     * @param spiBus   SPIClass pointer (default &SPI, the global Arduino SPI object).
     *                 The board package auto-creates &SPI with the correct bus for each platform
     *                 (VSPI on standard ESP32, FSPI on ESP32-S3, etc.).
     *                 Pass &SPI1, &SPI2, etc. to use a different bus if needed.
     */
    ADS131M08(int8_t clk_pin, int8_t cs_pin, int8_t drdy_pin, int8_t mosi_pin, int8_t miso_pin, int8_t sclk_pin, SPIClass *spiBus = &SPI);
    ~ADS131M08();

    /**
     * @brief Initializes the clock, SPI bus, and resets the ADC.
     * @return true if successful, false otherwise.
     */
    bool begin();

    /**
     * @brief Checks if DRDY pin is low (new data is available).
     */
    bool isDataReady();

    /**
     * @brief Reads a 30-byte frame and parses channel data.
     * @param data Reference to data structure to store results.
     * @return true if data was successfully read.
     */
    bool readData(ADS131M08_Data &data);

    /**
     * @brief Sets PGA Gain for all channels.
     */
    bool setGain(ADS131_Gain_t gain);

    /**
     * @brief Sets Oversampling Ratio (OSR) to control output data rate.
     */
    bool setOSR(ADS131_OSR_t osr);

    /**
     * @brief Performs zero-offset calibration. 
     * @param samples Number of samples to average.
     */
    void calibrate(uint16_t samples = 20);

    /**
     * @brief Converts raw ADC 32-bit (sign-extended) code to Voltage.
     */
    float rawToVoltage(int32_t raw);

    // --- Low-level Register Access ---
    bool writeRegister(uint8_t regAddr, uint16_t regData);
    bool readRegister(uint8_t regAddr, uint16_t &regData);

private:
    int8_t _pin_clk, _pin_cs, _pin_drdy, _pin_mosi, _pin_miso, _pin_sclk;
    SPIClass *_spi; // Pointer to user-provided SPI bus (dependency injection)
    SPISettings _spiSettings;
    
    uint8_t _txBuf[ADS131_FRAME_BYTES];
    uint8_t _rxBuf[ADS131_FRAME_BYTES];
    
    ADS131_Gain_t _currentGain;
    int32_t _offsets[ADS131_NUM_CHANNELS];

    void _startMasterClock();
    int32_t _signExtend24(uint32_t raw);
    void _transferFrame();
};

#endif // ADS131M08_H