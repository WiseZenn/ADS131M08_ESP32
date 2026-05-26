# Changelog

All notable changes to this project will be documented in this file.

## [2.1.0] - 2026-05-27

### Changed
- **SPI Bus: Dependency Injection** — The library no longer creates `SPIClass` internally. It now accepts a `SPIClass *spiBus` parameter (default `&SPI`). The Arduino core auto-creates the correct global `SPI` object for each board (VSPI on standard ESP32, FSPI on ESP32-S3, etc.), making the library work out of the box on all ESP32 variants (ESP32/S2/S3/C3) without any ESP32-specific macros.
- **SPI Clock: 4 MHz → 16 MHz** — Increased default SPI clock from 4 MHz to 16 MHz. At 4 MHz, the 30-byte frame transfer (~68 us/frame) capped throughput at ~14,700 SPS, making it impossible to reach 16 kSPS at OSR=256. Now achieves 97-98% of theoretical rates across all OSR settings.
- **`_startMasterClock()`** — Passing `clk_pin = -1` now skips LEDC MCLK generation, for boards using an external clock source. Previously required commenting out library source code.

### Added
- **`keywords.txt`** — Added missing method keywords: `setGain`, `setOSR`, `calibrate`, `writeRegister`, `readRegister`.
- **`examples/IssueDiagnostic/`** — Diagnostic test suite with 5 tests: ID register, OSR write-readback, sampling rate measurement, DRDY polling jitter, and stability test.
- **`examples/IssueDiagnostic/spi_debug.ino`** — Minimal test comparing `SPIClass(FSPI)` vs `&SPI` for troubleshooting SPI bus issues.

### Fixed
- **Sampling rate bottleneck** — At 4 MHz SPI, OSR=256 could only achieve ~13 kSPS instead of 16 kSPS. Fixed by increasing SPI to 16 MHz. Verified: OSR=128 → 31,256 SPS (97.7%), OSR=256 → 15,673 SPS (98.0%), OSR=512 → 7,824 SPS (97.8%).
- **Cross-platform SPI compatibility** — Hardcoded `new SPIClass(FSPI)` failed on standard ESP32 (FSPI reserved for flash). Now uses `&SPI` which is automatically correct for all platforms.
- **LEDC MCLK: 1-bit → 2-bit resolution** — Previous version used 1-bit resolution which gave 8.000 MHz. Updated to 2-bit resolution for better duty cycle accuracy (8.000 MHz with divider=5).

### Removed
- `_ownsSpi` private member — No longer needed with dependency injection pattern.

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
