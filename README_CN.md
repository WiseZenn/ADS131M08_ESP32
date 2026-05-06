# ADS131M08 Driver for ESP32

[![Arduino](https://img.shields.io/badge/Arduino-ESP32-blue.svg)](https://github.com/espressif/arduino-esp32)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-2.0.0-orange.svg)](library.properties)

[🇬🇧 English](README.md) | [🇨🇳 中文](README_CN.md)

专为 **ESP32/ESP32-S3** 微控制器优化的 **Texas Instruments ADS131M08** (8通道, 24位 Delta-Sigma ADC) 高性能 Arduino 库。

> **核心特性**: 本库利用 ESP32 的 **LEDC (PWM)** 外设直接生成所需的 8MHz 主时钟 (MCLK)，无需外部晶振。

## ✨ 特性

- **🚀 可编程增益放大器 (PGA)**: 支持 1x, 2x, 4x, 8x, 16x, 32x, 64x, 以及 128x 增益配置。
  - 允许与低阻抗传感器、生物电传感器直接配合使用。
  - 纯软件配置，无需更改硬件底层布局。
- **⚙️ 过采样率 (OSR) 调节**: 灵活配置从 128 至 16384，轻松平衡噪声与采样率（如在 BLE/WiFi 传输时可降低 OSR）。
- **🎛️ 零点偏移校准**: 内置动态零电平校准功能。
- **8通道同步采样**: 基于严格的 30 字节 SPI 数据帧读取所有 8 个通道的数据。
- **24位高分辨率**: 支持完整的 24 位数据解析（自动符号扩展为32位）。
- **内置时钟生成**: 使用 ESP32 硬件 PWM (LEDC) 生成 MCLK。
- **数据就绪中断**: 通过 DRDY 引脚实现高效的非阻塞数据读取。

## 🔌 接线与硬件

### 引脚映射 (以 ESP32-S3 为例)

| ADS131 引脚 | ESP32-S3 引脚 | 描述 |
| :--- | :--- | :--- |
| **AVDD/DVDD** | 3.3V | 电源 |
| **GND** | GND | 地 |
| **MCLK** | GPIO 1 | **时钟输入** (由 ESP32 LEDC 生成) |
| **SCLK** | GPIO 12 | SPI 时钟 |
| **MOSI (DIN)** | GPIO 11 | SPI 数据输入 |
| **MISO (DOUT)**| GPIO 13 | SPI 数据输出 |
| **CS** | GPIO 10 | 片选 |
| **DRDY** | GPIO 9 | 数据就绪中断 |

*注意：引脚可在软件构造函数中完全配置。*

## 🚀 使用方法

### 基础代码示例 (包含增益、OSR和校准功能)

\\cpp
#include <Arduino.h>
#include "ADS131M08.h"

// 定义 ESP32-S3 引脚
#define PIN_CLK  1  
#define PIN_DRDY 9  
#define PIN_CS   10 
#define PIN_MOSI 11 
#define PIN_SCLK 12 
#define PIN_MISO 13 

// 初始化驱动
ADS131M08 adc(PIN_CLK, PIN_CS, PIN_DRDY, PIN_MOSI, PIN_MISO, PIN_SCLK);
ADS131M08_Data adcData;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    if (!adc.begin()) {
        Serial.println("与 ADS131M08 通信失败！请检查接线。");
        while (1) { delay(100); }
    }
    Serial.println("ADS131M08 初始化成功！");

    // 1. 设置高增益以读取较小信号 (如称重传感器、生物医学传感器)
    //    库在 begin() 中自动处理寄存器解锁。
    if (!adc.setGain(ADS131_GAIN_64X)) {
        Serial.println("PGA 增益设置失败！请检查接线与电源。");
    }

    // 2. 设置 OSR 以调整输出数据率 (对低功耗蓝牙 BLE 应用至关重要)
    // 在 8MHz 时钟下 OSR = 16384，可使输出数据率设为 ~244Hz。
    adc.setOSR(ADS131_OSR_16384);
    
    // 3. 校准零点偏移
    Serial.println("正在校准偏置原点，请稍候...");
    adc.calibrate(50);
    Serial.println("校准完成！");
}

void loop() {
    if (adc.isDataReady()) {
        if (adc.readData(adcData)) {
            // 打印通道0的毫伏电压
            float ch0_mV = adc.rawToVoltage(adcData.ch[0]) * 1000.0f;
            
            Serial.print("CH0 (mV): ");
            Serial.print(ch0_mV, 4);
            
            Serial.print("\tCH0 Raw: ");
            Serial.println(adcData.ch[0]);
        }
    }
}
\
## ⚖️ 许可证

本项目采用 MIT 许可证。
