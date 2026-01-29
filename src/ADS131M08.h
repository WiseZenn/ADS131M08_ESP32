/**
 * @file ADS131M08.h
 * @brief ADS131M08 8-Channel Delta-Sigma ADC Driver for ESP32.
 * 
 * Provides an interface to communicate with the TI ADS131M08 ADC via SPI,
 * including clock generation using ESP32's LEDC peripheral.
 *
 * @author WiseZenn
 * @version 1.0.0
 * @license MIT
 */

#ifndef ADS131M08_H
#define ADS131M08_H

#include <Arduino.h>
#include <SPI.h>

/// Total number of channels on the ADS131M08
#define ADS131_NUM_CHANNELS 8

/// Frame size in bytes: 3 status bytes + 8 * 3 bytes channel data = 27 bytes
#define ADS131_FRAME_BYTES  27

/// Internal reference voltage (approx 1.2V)
#define ADS131_V_REF        1.2f

/// 24-bit resolution (2^23 positive range)
#define ADS131_RESOLUTION   8388608.0f

/**
 * @brief Structure to hold the data for all 8 channels.
 */
struct ADS131Data {
    int32_t ch[ADS131_NUM_CHANNELS]; ///< Array of signed 24-bit integer values (sign-extended to 32-bit)
};

/**
 * @brief Main driver class for ADS131M08.
 */
class ADS131M08 {
public:
    /**
     * @brief Constructor.
     * 
     * @param clk_pin  Pin for generating the ADC master clock (MCLK).
     * @param cs_pin   Chip Select pin.
     * @param drdy_pin Data Ready interrupt pin.
     * @param mosi_pin SPI MOSI pin.
     * @param miso_pin SPI MISO pin.
     * @param sclk_pin SPI Clock pin.
     */
    ADS131M08(int8_t clk_pin, int8_t cs_pin, int8_t drdy_pin, int8_t mosi_pin, int8_t miso_pin, int8_t sclk_pin);
    
    ~ADS131M08();

    /**
     * @brief Initializes the SPI bus, pins, and starts the MCLK signal.
     */
    void begin();

    /**
     * @brief Checks if new data is available.
     * @return true if DRDY pin is LOW.
     */
    bool isDataReady();

    /**
     * @brief Reads data from the ADC if available.
     * 
     * @param data Reference to an ADS131Data struct to store the results.
     * @return true if data was successfully read, false if DRDY was not active.
     */
    bool readData(ADS131Data &data);

    /**
     * @brief Converts a raw 24-bit signed value to Voltage (V).
     * @param raw Raw 24-bit integer.
     * @return Voltage in Volts.
     */
    static float rawToVoltage(int32_t raw);

    /**
     * @brief Converts a raw 24-bit signed value to Millivolts (mV).
     * @param raw Raw 24-bit integer.
     * @return Voltage in Millivolts.
     */
    static float rawToMillivolts(int32_t raw);

private:
    int8_t _pin_clk, _pin_cs, _pin_drdy, _pin_mosi, _pin_miso, _pin_sclk;
    SPIClass *_spi;
    uint8_t _rxBuf[ADS131_FRAME_BYTES];

    /**
     * @brief Configures LEDC to generate an 8MHz clock signal on _pin_clk.
     */
    void _startClock();

    /**
     * @brief Sign-extends a 24-bit integer to a 32-bit signed integer.
     * @param raw 24-bit unsigned/raw value.
     * @return 32-bit signed integer.
     */
    int32_t _signExtend24(uint32_t raw);
};

#endif
