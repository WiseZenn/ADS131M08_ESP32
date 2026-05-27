# ADS131M08 ESP32 驱动库

[![Arduino](https://img.shields.io/badge/Arduino-ESP32-blue.svg)](https://github.com/espressif/arduino-esp32)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-2.2.0-orange.svg)](library.properties)

[🇬🇧 English](README.md) | [🇨🇳 中文](README_CN.md)

**Texas Instruments ADS131M08**（8通道、24位 Delta-Sigma ADC）高性能 Arduino 驱动库。

> **核心特性**: 利用 ESP32 的 **LEDC (PWM)** 外设直接生成 8 MHz 主时钟 (MCLK)，无需外部晶振。如使用外部时钟，传入 `clk_pin = -1` 即可跳过内部时钟生成。

## 特性

- **可编程增益放大器 (PGA)**: 支持 1x/2x/4x/8x/16x/32x/64x/128x 增益配置，写入后自动回读验证。
- **过采样率 (OSR)**: 128 至 16384 可配置，OSR 越高噪声越低、数据率越低。
- **零点偏移校准**: 内置动态零电平校准功能，校准前自动排空 FIFO 防止陈旧数据污染。
- **8通道同步采样**: 严格的 30 字节 SPI 数据帧，符合 TI 数据手册规范。
- **24位高分辨率**: 完整 24 位数据解析，自动符号扩展为 32 位。
- **跨平台 SPI**: 通过依赖注入使用 Arduino 全局 `SPI` 对象 — 兼容 ESP32/ESP32-S2/S3/C3 及其他 Arduino 平台。
- **外部时钟支持**: 传入 `clk_pin = -1` 即可使用外部时钟源。
- **自动 FIFO 管理**: `begin()`、`calibrate()`、`setGain()`、`setOSR()` 后自动调用 `drainFIFO()`。ADS131M08 有 2 级 FIFO — 暂停读取后不排空会返回陈旧数据。
- **SYNC/RESET 引脚支持**: 可选 `reset_pin` 参数，通过 `syncReset()` 实现硬件级 FIFO 清除和重新同步。
- **底层寄存器访问**: 提供 `writeRegister()` 和 `readRegister()` 进行完整硬件控制。

## 接线

### 引脚映射（以 ESP32-S3 为例）

| ADS131 引脚 | ESP32-S3 引脚 | 描述 |
| :--- | :--- | :--- |
| **AVDD/DVDD** | 3.3V | 电源 |
| **GND** | GND | 地 |
| **MCLK** | GPIO 1 | 时钟输入（LEDC 生成或外部时钟） |
| **SCLK** | GPIO 12 | SPI 时钟 |
| **MOSI (DIN)** | GPIO 11 | SPI 数据输入 |
| **MISO (DOUT)**| GPIO 13 | SPI 数据输出 |
| **CS** | GPIO 10 | 片选 |
| **DRDY** | GPIO 9 | 数据就绪 |
| **SYNC/RESET** | — | 同步/复位（可选，未连接传 `-1`） |

*引脚在构造函数中完全可配置。*

## 使用方法

### 基础示例

```cpp
#include <Arduino.h>
#include "ADS131M08.h"

// 定义引脚
#define PIN_CLK   1
#define PIN_DRDY  9
#define PIN_CS   10
#define PIN_MOSI 11
#define PIN_SCLK 12
#define PIN_MISO 13
#define PIN_RESET -1  // SYNC/RESET 引脚（可选，未连接传 -1）

// 默认使用全局 &SPI 对象（自动适配各 ESP32 变体）
ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK, PIN_RESET);
ADS131M08_Data adcData;

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (!adc.begin()) {
        Serial.println("与 ADS131M08 通信失败！请检查接线。");
        while (1) { delay(100); }
    }
    Serial.println("ADS131M08 初始化成功！");

    // 设置增益（适用于小信号，如称重传感器、生物传感器）
    if (!adc.setGain(ADS131_GAIN_64X)) {
        Serial.println("PGA 增益设置失败！请检查接线与电源。");
    }

    // 设置 OSR 以平衡噪声与采样率
    // 8 MHz 时钟下 OSR=16384 → ~244 SPS
    adc.setOSR(ADS131_OSR_16384);

    // 校准零点偏移
    Serial.println("正在校准，请稍候...");
    adc.calibrate(50);
    Serial.println("校准完成！");
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

### 外部时钟

如果板子有外部时钟源（如 8.192 MHz 晶振），传入 `clk_pin = -1` 跳过内部 LEDC 时钟生成：

```cpp
// clk_pin = -1：跳过 MCLK 生成，使用外部时钟
ADS131M08 adc(-1, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK, PIN_RESET);
```

### 自定义 SPI 总线

默认 `spiBus` 参数为 `&SPI`，Arduino Core 会根据板型自动创建正确的全局 SPI 实例。如需使用其他 SPI 总线，可显式传入：

```cpp
// 使用指定的 SPI 总线（reset_pin 在 spiBus 之前）
ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK, -1, &SPI1);
```

### SYNC/RESET 引脚（可选）

如接线包含 SYNC/RESET 引脚，可使用硬件级 FIFO 清除和重新同步：

```cpp
#define PIN_RESET 4  // ADS131M08 SYNC/RESET 引脚

ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK, PIN_RESET);

// 在代码中需要时，清除 FIFO 并重新同步：
adc.syncReset();      // 清除 FIFO，然后重新 UNLOCK 寄存器
adc.setGain(...);     // syncReset 后重新配置增益
adc.setOSR(...);      // syncReset 后重新配置 OSR
```

### FIFO 管理

ADS131M08 每通道有 2 级 FIFO。暂停读取（或 `begin()`/`calibrate()`/`setGain()`/`setOSR()` 之后），FIFO 中可能有陈旧数据。库会在上述方法中自动调用 `drainFIFO()`。也可以手动调用：

```cpp
// 暂停读取后排空 FIFO：
adc.drainFIFO();  // 读取 2 帧虚拟数据，清除陈旧数据
// 现在 readData() 返回实时数据
```

## 数据率参考

公式（数据手册公式 5）：

```
f_MOD  = f_CLKIN / 2
f_DATA = f_MOD / OSR
```

在 f_CLKIN = 8.192 MHz（f_MOD = 4.096 MHz）下：

| OSR | 数据率 | 适用场景 |
|:---:|:---:|:---|
| 128 | 32 kSPS | 高速采集 |
| 256 | 16 kSPS | 快速采样 |
| 512 | 8 kSPS | 通用 |
| 1024 | 4 kSPS | 默认值（复位值） |
| 2048 | 2 kSPS | 低噪声 |
| 4096 | 1 kSPS | 极低噪声 |
| 8192 | 500 SPS | 超低噪声 |
| 16384 | 250 SPS | BLE/WiFi 流传输 |

## SPI 时钟说明

默认 SPI 时钟为 **16 MHz**（数据手册规定 3.3V DVDD 下最高 25 MHz）。

每帧 SPI 数据为 10 字 × 24 位 = **240 个时钟周期**。在 16 MHz 下，一帧耗时 **15 μs**，在 32 kSPS（OSR=128）的 31.25 μs 窗口内绰绰有余。

如果使用杜邦线或面包板，走线尽量短，避免高频信号完整性问题。

## 更新日志

版本历史详见 [CHANGELOG.md](CHANGELOG.md)。

## 许可证

本项目采用 MIT 许可证。
