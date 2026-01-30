# ADS131M08 Driver for ESP32

[![Arduino](https://img.shields.io/badge/Arduino-ESP32-blue.svg)](https://github.com/espressif/arduino-esp32)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.0.0-orange.svg)](library.properties)

[🇺🇸 English](README.md) | [🇨🇳 中文](README_CN.md)

专为 **ESP32/ESP32-S3** 微控制器优化的 **Texas Instruments ADS131M08** (8通道, 24位 Delta-Sigma ADC) 高性能 Arduino 库。

> **核心特性**: 本库利用 ESP32 的 **LEDC (PWM)** 外设直接生成所需的 8.192MHz (或 8MHz) 主时钟 (MCLK)，无需外部晶振。

## ✨ 特性

- **8通道同步采样**: 高效读取所有8个通道的数据。
- **24位高分辨率**: 支持完整的24位数据解析。
- **内置时钟生成**: 使用 ESP32 硬件 PWM (LEDC) 生成 MCLK。
- **高速 SPI**: 针对快速数据读取进行了优化。
- **数据就绪中断**: 通过 DRDY 引脚实现高效的非阻塞数据读取。

## 🔌 接线与硬件

### 连接示意图

![接线图](extras/ADS131M08.png)

*详细原理图信息请参考 [ADS131M08_Schematic.pdf](extras/ADS131M08_Schematic.pdf)*

## 🔗 相关文档

- **官方数据手册与产品页**: [Texas Instruments ADS131M08](https://www.ti.com.cn/product/cn/ADS131M08)

### 引脚映射 (以 ESP32-S3 R8 为例)

| ADS131 引脚 | ESP32-S3 引脚 | 描述 |
| :--- | :--- | :--- |
| **AVDD/DVDD** | 3.3V | 电源 |
| **GND** | GND | 地 |
| **MCLK** | GPIO 4 | **时钟输入** (由 ESP32 LEDC 生成) |
| **SCLK** | GPIO 12 | SPI 时钟 |
| **MOSI (DIN)** | GPIO 11 | SPI 数据输入 |
| **MISO (DOUT)**| GPIO 13 | SPI 数据输出 |
| **CS** | GPIO 10 | 片选 |
| **DRDY** | GPIO 9 | 数据就绪中断 |

*注意：引脚可在软件构造函数中完全配置。*

## 📦 安装

1. 下载本仓库的 `.zip` 文件。
2. 在 Arduino IDE 中，点击 **Sketch (项目)** -> **Include Library (加载库)** -> **Add .ZIP Library... (添加 .ZIP 库)**。
3. 选择下载的文件。

## 🚀 使用方法

```cpp
#include <ADS131M08.h>

// 引脚定义 (基于 ESP32-S3 的示例)
#define CLK_PIN  4  // 由 ESP32 生成 MCLK
#define CS_PIN   10
#define DRDY_PIN 9
#define MOSI_PIN 11
#define MISO_PIN 13
#define SCLK_PIN 12

// 初始化驱动
ADS131M08 adc(CLK_PIN, CS_PIN, DRDY_PIN, MOSI_PIN, MISO_PIN, SCLK_PIN);

void setup() {
    Serial.begin(115200);
    
    // 启动 ADC 和时钟
    adc.begin();
    Serial.println("ADS131M08 Initialized");
}

void loop() {
    if (adc.isDataReady()) {
        ADS131Data data;
        if (adc.readData(data)) {
            // 打印通道 0 电压
            float voltage = ADS131M08::rawToVoltage(data.ch[0]);
            Serial.printf("CH0: %.4f V\n", voltage);
        }
    }
}
```

## ⚠️ 兼容性说明

本库使用 ESP-IDF `driver/ledc.h` API 进行时钟生成。
- **已测试平台**: ESP32-S3 (R8)
- **Arduino Core**: 推荐 v2.0.x (v3.0+ 可能需要更新 LEDC API)

## 📄 许可证

本项目采用 MIT 许可证 - 详情请参阅 [LICENSE](LICENSE) 文件。

---
**作者**: WiseZenn
