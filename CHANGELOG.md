# Changelog

All notable changes to this project will be documented in this file.

## [2.0.0] - 2026-05-06

### Added
- **PGA Gain Control**: Full programmable gain amplifier (PGA) support via `setGain(ADS131_Gain_t gain)`.
  - 1x / 2x / 4x / 8x / 16x / 32x / 64x / 128x gain settings (all 8 channels simultaneously).
  - Writes to GAIN1 (0x04) and GAIN2 (0x05) registers with automatic readback verification.
- **UNLOCK sequence**: `begin()` now sends the `0x0655` UNLOCK command after RESET to enable register writes per TI datasheet.
- **`writeRegister()` / `readRegister()`**: Low-level SPI register access methods exposed as public API.
- **`setOSR()`**: Oversampling ratio configuration via CLOCK register (0x03) read-modify-write.
- **`calibrate()`**: Zero-offset calibration by averaging configurable number of samples.

### Fixed
- **MCLK clock generation**: Changed LEDC resolution from 8-bit to 2-bit (`ledcAttach(pin, 8MHz, 2)`). The previous 8-bit resolution was mathematically impossible at 8 MHz on ESP32 (required 2.048 GHz timer clock vs 80 MHz APB), causing actual output of ~312.5 kHz — far below the ADS131M08 minimum MCLK (~1 MHz). This caused all channel data to read as zero.
- **`setGain()` now returns `bool`**: Performs readback of GAIN1/GAIN2 registers after write and returns `false` on mismatch.

### Changed
- `begin()` returns `bool` — verifies ADC communication by reading the ID register (bits 15:12 must be `0x2`).
- `rawToVoltage()` now accounts for current gain setting via `_currentGain` multiplier.

## [1.0.0] - 2026-04-07

### Features
- Basic 8-channel ADC data acquisition with 30-byte SPI framing.
- ESP32 LEDC peripheral generates 8 MHz MCLK (no external crystal needed).
- Data Ready (DRDY) interrupt polling.
- Raw to voltage conversion utilities.
- Sign-extension from 24-bit to 32-bit integer.
