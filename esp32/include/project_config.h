#pragma once

/*
 * ============================================================================
 *  凭据（Credentials）
 *
 *  本文件会随代码一起公开，所以下面的密码都是占位符。
 *  烧录前请把这几项改成你自己的值，否则 ESP32 只会开热点、连不上你的路由器：
 *
 *      CAR_AP_PASSWORD   小车自己热点的密码，自己设一个（至少 8 位）
 *      WIFI_SSID         要连的手机热点 / 路由器名称，留空 "" 表示只开热点
 *      WIFI_PASSWORD     上面那个网络的密码
 * ============================================================================
 */

/*
 * 小车自己会开一个热点，不依赖路由器也能先测通。
 * 手机或电脑连上后打开 http://192.168.4.1/
 */
static constexpr char CAR_AP_SSID[] = "CarControl-ESP32";
static constexpr char CAR_AP_PASSWORD[] = "CHANGE_ME_ap_password";

/*
 * 可选：连接路由器 / 手机热点。留空 WIFI_SSID 就是「只开热点」模式。
 * 这两个值会被编译进固件，请用实验用的网络，不要填重要账号。
 */
static constexpr char WIFI_SSID[] = "";
static constexpr char WIFI_PASSWORD[] = "";

/* Dedicated UART1 pins on the YD-ESP32-S3 board. Do not use the silk-screen TX/RX. */
static constexpr int CAR_UART_TX_PIN = 17;
static constexpr int CAR_UART_RX_PIN = 18;
static constexpr unsigned long CAR_UART_BAUD = 115200;

/* Browser sends a command every 180 ms while a control is held. */
static constexpr unsigned long ESP32_COMMAND_TIMEOUT_MS = 500;

/*
 * AHT10 温湿度传感器（I2C）。
 *
 * 为什么选 GPIO8 / GPIO9：这两个脚在 ESP32-S3 上确定是空闲的普通 IO——
 *   不是 strapping 脚（0 / 45 / 46）
 *   不是原生 USB（19 / 20）
 *   不是串口 0（43 / 44，板载 USB 转串口占着）
 *   不是 SPI Flash（26~32）
 *   不是八线 PSRAM 占用的（33~37，本板是 N16R8 + qio_opi，这几个脚一定不能用）
 *   不是板载 RGB 灯（48）
 *   也不是本工程已占用的 STM32 串口（17 / 18）
 *
 * 如果和你板子上的丝印对不上，改下面两行即可，不用改驱动代码。
 * SDA 和 SCL 千万别接反，也别漏了共地。
 */
static constexpr int CLIMATE_I2C_SDA_PIN = 8;
static constexpr int CLIMATE_I2C_SCL_PIN = 9;
static constexpr uint32_t CLIMATE_I2C_FREQ_HZ = 100000;

/* AHT10 的 7 位地址固定是 0x38（AHT20 也是）。 */
static constexpr uint8_t CLIMATE_I2C_ADDRESS = 0x38;

/* 多久读一次。AHT10 一次测量约 80 ms，这里用状态机做，不阻塞主循环。 */
static constexpr unsigned long CLIMATE_READ_INTERVAL_MS = 2000;

/* 触发测量后等多久去取数据。数据手册要求 80 ms 以内完成，留点余量。 */
static constexpr unsigned long CLIMATE_MEASURE_WAIT_MS = 90;

