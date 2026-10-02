#pragma once

#include <Arduino.h>

/*
 * AHT10 温湿度传感器驱动（I2C，地址 0x38）。
 *
 * 顺带兼容 AHT20：两者地址和读数据格式完全一样，只有初始化命令不同
 * （AHT10 用 0xE1，AHT20 用 0xBE）。驱动会两个都试，谁把「已校准」标志位置起来
 * 就用谁——因为市面上标着 AHT10 的模块里装的经常其实是 AHT20。
 *
 * 读取用状态机实现，service() 不会阻塞主循环（AHT10 一次测量要 80 ms）。
 */
struct ClimateReading {
  bool present;          // I2C 上 0x38 有没有器件应答
  bool valid;            // 是否至少成功读到过一次数据
  bool busy;             // 正在测量中
  uint8_t address;       // 7 位地址
  const char *chip;      // "AHT10" / "AHT20" / ""
  float temperatureC;    // 摄氏度
  float humidityPct;     // 相对湿度 %
  unsigned long ageMs;   // 距上次成功读数过了多久
  uint32_t samples;      // 累计成功读数次数
  uint8_t errorCode;     // 0=正常，见 errorText()
};

class ClimateSensor {
 public:
  void begin();
  void service();

  const ClimateReading &reading() const { return reading_; }

  /* 给 /api/status 用的 JSON 片段 */
  String statusJson() const;

  /* 出错时给人看的说明 */
  const char *errorText() const;

  /* 扫描到的地址，例如 "0x38"；空则返回 "无" */
  String scanSummary() const;

 private:
  bool writeCommand(const uint8_t *data, size_t length);
  bool readBytes(uint8_t *buffer, size_t length);
  bool readStatus(uint8_t &status);
  bool initializeChip();
  void scanBus();
  void retryIfNeeded();

  ClimateReading reading_{};
  uint8_t scanAddresses_[8]{};
  uint8_t scanCount_ = 0;
  bool initialized_ = false;
  bool measuring_ = false;
  unsigned long measureStartedMs_ = 0;
  unsigned long lastAttemptMs_ = 0;
  unsigned long lastSuccessMs_ = 0;
};

extern ClimateSensor climateSensor;
