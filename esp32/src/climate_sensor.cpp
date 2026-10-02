#include "climate_sensor.h"

#include <Wire.h>

#include "project_config.h"

ClimateSensor climateSensor;

namespace {

/* AHT10 初始化 + 校准 */
constexpr uint8_t kInitAht10[3] = {0xE1, 0x08, 0x00};
/* AHT20 初始化 + 校准（地址和读数据格式与 AHT10 相同，只有这条命令不同） */
constexpr uint8_t kInitAht20[3] = {0xBE, 0x08, 0x00};
/* 触发一次温湿度测量 */
constexpr uint8_t kMeasure[3] = {0xAC, 0x33, 0x00};

constexpr uint8_t kStatusBusy = 0x80;        /* bit7 = 忙 */
constexpr uint8_t kStatusCalibrated = 0x08;  /* bit3 = 已校准 */

/* 20 位原始值满量程 = 2^20，湿度和温度都用这个分母 */
constexpr float kRawFullScale = 1048576.0f;

/* 传感器没应答时，隔多久重新初始化 + 重新扫描一次 */
constexpr unsigned long kRetryIntervalMs = 5000;

String formatAddress(uint8_t address) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "0x%02X", address);
  return String(buffer);
}

}  // namespace

void ClimateSensor::begin() {
  Wire.begin(CLIMATE_I2C_SDA_PIN, CLIMATE_I2C_SCL_PIN, CLIMATE_I2C_FREQ_HZ);
  /* 总线被拉死时不要让 requestFrom 卡太久 */
  Wire.setTimeOut(50);

  /* AHT10 上电后需要一点稳定时间（数据手册要求 > 20 ms） */
  delay(60);

  scanBus();
  Serial.printf("[CLIMATE] I2C SDA=GPIO%d SCL=GPIO%d @%luHz, scan: %s\n",
                CLIMATE_I2C_SDA_PIN, CLIMATE_I2C_SCL_PIN,
                static_cast<unsigned long>(CLIMATE_I2C_FREQ_HZ),
                scanSummary().c_str());

  initialized_ = initializeChip();
  lastAttemptMs_ = millis();
  lastSuccessMs_ = millis();

  if (initialized_) {
    Serial.printf("[CLIMATE] %s ready at %s\n", reading_.chip,
                  formatAddress(reading_.address).c_str());
  } else {
    Serial.printf("[CLIMATE] not ready: %s\n", errorText());
  }
}

void ClimateSensor::scanBus() {
  scanCount_ = 0;
  for (uint8_t address = 0x08; address < 0x78 && scanCount_ < 8; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      scanAddresses_[scanCount_++] = address;
    }
  }
}

bool ClimateSensor::writeCommand(const uint8_t *data, size_t length) {
  Wire.beginTransmission(CLIMATE_I2C_ADDRESS);
  Wire.write(data, length);
  return Wire.endTransmission() == 0;
}

bool ClimateSensor::readBytes(uint8_t *buffer, size_t length) {
  if (Wire.requestFrom(static_cast<int>(CLIMATE_I2C_ADDRESS),
                       static_cast<int>(length)) != static_cast<int>(length)) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    buffer[index] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool ClimateSensor::readStatus(uint8_t &status) {
  return readBytes(&status, 1);
}

bool ClimateSensor::initializeChip() {
  uint8_t status = 0;

  reading_.present = false;
  reading_.chip = "";
  reading_.address = CLIMATE_I2C_ADDRESS;

  /* 先看 0x38 上到底有没有器件应答 */
  Wire.beginTransmission(CLIMATE_I2C_ADDRESS);
  if (Wire.endTransmission() != 0) {
    reading_.errorCode = 1;
    return false;
  }
  reading_.present = true;

  /* AHT10 走 0xE1；如果模块其实是 AHT20，就得用 0xBE。两个都试一遍。 */
  const uint8_t *commands[2] = {kInitAht10, kInitAht20};
  const char *names[2] = {"AHT10", "AHT20"};

  for (int index = 0; index < 2; ++index) {
    if (!writeCommand(commands[index], 3)) {
      continue;
    }
    delay(20);
    if (!readStatus(status)) {
      continue;
    }
    if ((status & kStatusCalibrated) != 0) {
      reading_.chip = names[index];
      reading_.errorCode = 0;
      return true;
    }
  }

  /* 再宽限一次，有些批次校准比较慢 */
  delay(60);
  if (readStatus(status) && (status & kStatusCalibrated) != 0) {
    reading_.chip = "AHT10/AHT20";
    reading_.errorCode = 0;
    return true;
  }

  reading_.errorCode = 2;
  return false;
}

void ClimateSensor::retryIfNeeded() {
  const unsigned long now = millis();
  if (now - lastAttemptMs_ < kRetryIntervalMs) {
    return;
  }
  lastAttemptMs_ = now;
  scanBus();
  if (initializeChip()) {
    Serial.printf("[CLIMATE] %s detected at %s\n", reading_.chip,
                  formatAddress(reading_.address).c_str());
  }
}

void ClimateSensor::service() {
  const unsigned long now = millis();

  /* 每次进来都刷新一下「数据有多旧」，方便网页显示 */
  reading_.ageMs = reading_.valid ? (now - lastSuccessMs_) : 0;

  if (!initialized_ || !reading_.present) {
    retryIfNeeded();
    return;
  }

  if (!measuring_) {
    /* 已经有数据且还没到下次读取时间，就什么都别做 */
    if (reading_.valid && (now - lastAttemptMs_) < CLIMATE_READ_INTERVAL_MS) {
      return;
    }
    if (!writeCommand(kMeasure, 3)) {
      /* 写不进去说明器件掉了，下一轮重新初始化 */
      reading_.present = false;
      reading_.errorCode = 1;
      lastAttemptMs_ = now;
      return;
    }
    measuring_ = true;
    reading_.busy = true;
    measureStartedMs_ = now;
    return;
  }

  /* 正在等测量完成 */
  if (now - measureStartedMs_ < CLIMATE_MEASURE_WAIT_MS) {
    return;
  }
  measuring_ = false;
  reading_.busy = false;
  lastAttemptMs_ = now;

  uint8_t data[6] = {0};
  if (!readBytes(data, 6)) {
    reading_.errorCode = 3;
    return;
  }
  if ((data[0] & kStatusBusy) != 0) {
    reading_.errorCode = 4;
    return;
  }
  if ((data[0] & kStatusCalibrated) == 0) {
    initialized_ = false;
    reading_.errorCode = 2;
    return;
  }

  /* AHT10 数据格式：状态 1 字节 + 湿度 20 位 + 温度 20 位，共 6 字节 */
  const uint32_t rawHumidity =
      (static_cast<uint32_t>(data[1]) << 12) |
      (static_cast<uint32_t>(data[2]) << 4) |
      (static_cast<uint32_t>(data[3]) >> 4);
  const uint32_t rawTemperature =
      ((static_cast<uint32_t>(data[3]) & 0x0F) << 16) |
      (static_cast<uint32_t>(data[4]) << 8) |
      static_cast<uint32_t>(data[5]);

  reading_.humidityPct = static_cast<float>(rawHumidity) * 100.0f / kRawFullScale;
  reading_.temperatureC =
      static_cast<float>(rawTemperature) * 200.0f / kRawFullScale - 50.0f;

  /* 合理性检查：AHT10 有效量程是 0~100%RH、-40~80℃ */
  if (reading_.humidityPct < 0.0f || reading_.humidityPct > 100.0f ||
      reading_.temperatureC < -40.0f || reading_.temperatureC > 80.0f) {
    reading_.errorCode = 4;
    return;
  }

  reading_.valid = true;
  reading_.errorCode = 0;
  reading_.ageMs = 0;
  lastSuccessMs_ = now;
  ++reading_.samples;
}

const char *ClimateSensor::errorText() const {
  switch (reading_.errorCode) {
    case 0:
      return "正常";
    case 1:
      return "0x38 无应答：检查 SDA/SCL 是否接反、VCC/GND、以及和开发板共地";
    case 2:
      return "初始化失败：器件没进入已校准状态，可能是供电不稳或型号不符";
    case 3:
      return "读取数据失败：I2C 通信中断";
    case 4:
      return "数据无效：器件仍在测量或读数超出量程";
    default:
      return "未知错误";
  }
}

String ClimateSensor::scanSummary() const {
  if (scanCount_ == 0) {
    return String("无");
  }
  String result;
  for (uint8_t index = 0; index < scanCount_; ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += formatAddress(scanAddresses_[index]);
  }
  return result;
}

String ClimateSensor::statusJson() const {
  String json;
  json.reserve(320);
  json += F("{\"ok\":");
  json += reading_.valid ? F("true") : F("false");
  json += F(",\"present\":");
  json += reading_.present ? F("true") : F("false");
  json += F(",\"chip\":\"");
  json += reading_.chip == nullptr ? "" : reading_.chip;
  json += F("\",\"address\":\"");
  json += formatAddress(reading_.address);
  json += F("\",\"temperature_c\":");
  json += reading_.valid ? String(reading_.temperatureC, 2) : String(F("null"));
  json += F(",\"humidity_pct\":");
  json += reading_.valid ? String(reading_.humidityPct, 2) : String(F("null"));
  json += F(",\"age_ms\":");
  json += reading_.valid ? String(reading_.ageMs) : String(F("null"));
  json += F(",\"samples\":");
  json += reading_.samples;
  json += F(",\"error\":\"");
  json += errorText();
  json += F("\",\"i2c_scan\":\"");
  json += scanSummary();
  json += F("\"}");
  return json;
}
