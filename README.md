# 探测小车：CyberCAM 实时画面 + 小车与 SG90 舵机网页控制

本目录提供在电脑上运行的网页服务，不需要地瓜派。Python 3 标准库即可运行，无需安装第三方包。

```text
CyberCAM 192.168.137.67 ──电脑热点/Wi-Fi──> 电脑网页（MJPEG 视频）
电脑网页 ──电脑热点/Wi-Fi──> ESP32 ──UART──> STM32 ──> 电机驱动板
                                                └──PA6 PWM──> SG90
```

CyberCAM 上运行本目录的 `cybercam/cybercam_stream.py`，电脑网页从 `http://192.168.137.67:8081/stream.mjpg` 取视频。这一路是摄像头原始画面，不包含旧项目的颜色形状识别框。控制按钮调用原 ESP32 固件的 `POST /api/command`。按住按钮每 180 ms 发送一次方向命令，松开立即发送 `STOP`。原固件还有 ESP32 500 ms 与 STM32 700 ms 两级失联停车保护。

## 项目结构

```text
.
├── app.py                        电脑上的网页服务（Python 3 标准库，无第三方依赖）
├── web/                          浏览器前端
│   ├── index.html
│   ├── app.js                    按住才转、舵机方向与转速、温湿度渲染都在这
│   └── style.css
├── esp32/                        ESP32-S3 固件（PlatformIO / Arduino）
│   ├── platformio.ini
│   ├── include/project_config.h  引脚与凭据（密码为占位符，烧录前请自己填）
│   └── src/
│       ├── main.cpp              WiFi、HTTP / WebSocket、UART 桥、状态 JSON
│       ├── climate_sensor.h/.cpp AHT10 温湿度驱动（兼容 AHT20）
│       └── web_page.h            ESP32 自带的一个简易控制页
├── stm32/                        STM32F103 固件（Keil MDK 工程）
│   ├── Project.uvprojx
│   ├── User/                     自己写的代码
│   │   ├── main.c                主循环：电机、舵机、串口、失联保护
│   │   ├── car_control.c/.h      电机驱动板 I2C（PB6/PB7）
│   │   ├── servo_control.c/.h    舵机 PWM（PA6 = TIM3_CH1）
│   │   ├── remote_control.c/.h   USART2 协议（PA2/PA3，115200 8N1）
│   │   └── delay.c/.h            SysTick 毫秒延时
│   ├── Library/                  ST 标准外设库
│   ├── Start/                    启动文件、CMSIS、system_stm32f10x.c
│   └── firmware/                 编译好的 HEX
├── cybercam/                     跑在 CyberCAM 摄像头板上的 MJPEG 服务
├── tests/test_app.py             网页服务的接口测试
├── verification/                 验证截图
├── build-stm32.ps1               编译 / 烧录 STM32
└── build-esp32.ps1               编译 / 烧录 ESP32
```

> **脚本运行环境**：`build-stm32.ps1` 和 `build-esp32.ps1` 是 UTF-8 无 BOM 的 PowerShell 脚本。
> 在 **PowerShell 7（`pwsh`）** 下可直接运行；Windows PowerShell 5.1 会把里面的中文注释按 GBK 解读而报语法错。
> 用 5.1 的话可以先给文件加 BOM，或者直接用 `pwsh -File .\build-stm32.ps1` 执行。

> **实现原理与答辩准备**：每个模块具体是怎么实现的（时钟树、PWM 标定、串口帧解析、I2C 容错、AHT10 数据格式等），
> 以及 17 道答辩模拟题及答案，见 [`实现原理与答辩准备.md`](实现原理与答辩准备.md)。

## 1. CyberCAM 图像服务（已部署）

已经将 `cybercam/cybercam_stream.py` 上传到板上的 `/home/pi/probe_car_cybercam/`，安装并启用了 `probe-car-camera.service`。它独占摄像头，输出 320×240、8 FPS、JPEG 质量 50 的 MJPEG 视频，不需要地瓜派或 USB 数据线。电脑已经实测收到有效 JPEG 帧。

今后重启 CyberCAM 时服务会自动启动。若要排查或手动重启，在 CyberCAM 的终端执行：

```bash
systemctl status probe-car-camera.service --no-pager
sudo systemctl restart probe-car-camera.service
curl http://127.0.0.1:8081/health
```

电脑端验证：

```powershell
curl.exe http://192.168.137.67:8081/health
```

正常应看到 `"ok":true`、不断增加的 `frames` 和较小的 `age_ms`。若 CyberCAM 更换热点后 IP 变化，启动网页时用 `--video-url` 指定新地址。旧文档中的地瓜派 `10.10.10.228` 地址不适用于当前硬件。

## 2. ESP32 固件和电脑热点

`esp32/` 和 `build-esp32.ps1` 是从原工程复制出来的独立副本，原工程未改动。热点名称和密码已经填写在本目录的 `esp32/include/project_config.h`，串口配置为 COM7。固件已于 2026-09-28 编译并成功烧录到 COM7。

**电脑移动热点必须保持 2.4 GHz。** 初次烧录时，热点为 5 GHz（5765 MHz），ESP32-S3 因此连接超时。2026-09-28 已将 Windows 移动热点切换为 2.4 GHz，并实测 ESP32 获得 `192.168.137.88`，CyberCAM 仍在 `192.168.137.67`。热点名称和密码不需要写进网页代码。

需要重新烧录时，在本目录运行：

```powershell
.\build-esp32.ps1 -Upload
```

ESP32 启动后，在 115200 波特率的 COM7 串口日志中找到 `station IP`。2026-09-30 重新分配为 `192.168.137.127`，网页默认配置已经更新；若再次变化，以下地址与启动参数都要相应更新。

```powershell
curl.exe http://192.168.137.127/api/status
```

如果 ESP32 尚未接入电脑热点，而电脑只有一个 Wi-Fi 连接，就无法同时访问 CyberCAM 和 ESP32 自建热点 `192.168.4.1`。必须让 ESP32 成功连上电脑热点，或者给电脑增加第二个网络接口。

## 3. 启动网页

在 PowerShell 中执行：

```powershell
cd 'D:\桌面图标\大学课程相关\大四\大四上\探测小车'
py -3 .\app.py
```

浏览器打开 `http://127.0.0.1:8765/`。ESP32 和 CyberCAM 地址已分别默认设为 `192.168.137.127`、`192.168.137.67`；若设备 IP 改变，可用 `--car-url http://新IP` 或 `--video-url http://新IP:8081/stream.mjpg` 覆盖。按 `Ctrl+C` 停止网页服务。

只有网页显示“ESP32 已连接”“STM32 在线”且电机驱动板就绪时，小车方向按钮才会启用。舵机按钮只要求 STM32 舵机新固件在线，可在电机板未就绪时单独使用。首次试车请架空驱动轮，并避免同时打开多个控制网页。

若网页显示“STM32 未连接”，先确认 STM32 已通电并烧录本目录 `stm32/firmware/probe_car_stm32_sg90.hex`。原代码使用 USART2、115200 8N1：ESP32 GPIO17（TX）接 STM32 PA3（RX），ESP32 GPIO18（RX）接 STM32 PA2（TX），两板 GND 必须共地。`GET /api/status` 的 `stm32_online` 为 `true`、`stm32_version` 显示 `V80SG90` 才表示新固件双向串口已通。电机板未通电或 PB6/PB7 的 I2C 无应答时，STM32 闪灯、回报 `motor_ready:false` 并每 2 秒重试初始化；舵机与串口仍可使用。重试成功后以停车状态恢复，不重放旧行驶指令。接线前请断电，并核对实际开发板引脚标注及 3.3 V 电平。

## 4. 舵机（2026-09-30 新增，2026-10-02 改为「按住才转」）

**重要：本车装的是 360° 连续旋转舵机，不是位置舵机。**

判据：早先网页发 `angle=100`，也就是 1556 µs，只比中位 1500 µs 偏了 56 µs，舵机却一直转不停。位置舵机在这种情况下只会转约 10° 然后停住；会持续转，说明它把「偏离 1500 µs 的程度」当成转速了。

所以对这类舵机来说「角度」是没有意义的，脉宽的真实含义是：

| 脉宽 | 含义 |
| --- | --- |
| 1500 µs | 停止 |
| 大于 1500 µs | 朝一个方向持续转，偏得越多越快 |
| 小于 1500 µs | 朝另一个方向持续转 |

网页舵机面板因此改成三个按钮：**◀ 左转 / ■ 停止 / 右转 ▶**，**按住才转，松开立即停止**，用法和方向键一致。按住期间网页每 180 ms 续发一次指令；松开、窗口失焦、切到后台、关闭页面都会立刻补发停止指令。

两级失联保护，网页漏发也不会失控：

| 环节 | 保护 |
| --- | --- |
| 网页 | 松开 / 失焦 / 页面隐藏 / 关闭 → 立即发停止点 |
| STM32 | `SERVO_HOLD_TIMEOUT_MS = 700`：超过 700 ms 没收到新的舵机指令就 **`Servo_Release()` 停止发脉冲** |

浏览器崩溃、WiFi 掉线、ESP32 死机、串口断开，舵机都会在 0.7 秒内停下来。

> **为什么超时是「停止发脉冲」而不是「发 1500 µs」**：连续旋转舵机的真实停止点每一颗都不一样，
> 固件写死 1500 µs 有可能反而让它慢慢爬。停止发脉冲（CCR1 = 0，PA6 恒低）时舵机收不到脉冲、
> 不驱动电机，一定能停。代价是停下来后没有保持力矩（对外力没有抵抗力）。

调参（改 `web/app.js` 顶部即可，改完刷新网页，不需要重烧固件）：

- **左右转共用一个基准，只差正负号**：

  ```js
  const SERVO_STOP_ANGLE = 88;   // 这颗舵机的真实停止点，约 1489 µs（不是标称的 1500）
  const SERVO_SPAN = 15;         // 转速偏移，±83 µs，约为满速的一半
  // 左转 = 停止点 - 偏移（73 -> 1406 µs）    右转 = 停止点 + 偏移（103 -> 1572 µs）
  ```

  **这两个值必须对称分布在舵机真实停止点两侧，左右转速才会一样快。**
  2026-10-02 实测：按标称 1500 µs（角度 90）时右转略快，说明真实停止点低于 1500；
  把 `SERVO_STOP_ANGLE` 从 90 下调到 88 后，两侧偏移都是 83 µs，转速一致。

  以后若再遇到左右快慢不一致：**右转快就把这个数调小，左转快就调大，一次调 1~2**
  （1 个角度约等于 5.6 µs）。

  **不要改成「左转一个值、右转另一个值」的两个独立常量。** 2026-10-02 出现过一次
  `SERVO_RIGHT_ANGLE` 被改成和左转相同的 75，两个按钮发出同一个脉宽，结果左右同向。
- **转速**：改 `SERVO_SPAN`，越小越慢。15 是半速；想更慢用 10，想恢复满速用 30。
- **左右反了**：把 `currentServoAngle()` 里的 `- SERVO_SPAN` 和 `+ SERVO_SPAN` 对调；
  或在 `servo_control.h` 把 `SERVO_REVERSED` 改成 1，从硬件层反转。

接线：

| SG90 导线 | 连接位置 |
| --- | --- |
| 橙色/黄色信号线 | STM32 PA6（TIM3_CH1） |
| 红色电源线 | 独立、稳定的 5 V 电源正极 |
| 棕色/黑色地线 | 电源负极、STM32 GND 共地 |

不要从 STM32 的 GPIO 或 3.3 V 输出给 SG90 供电。舵机供电说明参考 [TowerPro SG90 官方规格](https://towerpro.com.tw/product/sg90-7/)。PA6 定时器映射参考 [STM32F103 数据手册](https://www.st.com/resource/en/datasheet/cd00161566.pdf)。

`stm32/User/servo_control.c` 用寄存器直接配置 TIM3 硬件 PWM：1 µs 计数、20 ms 周期（50 Hz）。`servo_control.h` 集中配置脉宽标尺、`SERVO_REVERSED`、上电自检开关 `SERVO_BOOT_SELFTEST`，以及失联保护 `SERVO_HOLD_TIMEOUT_MS`。

**如果以后换成普通位置舵机（能定位到某个角度）**，必须把 `SERVO_HOLD_TIMEOUT_MS` 改成 0 关掉失联保护，否则舵机转到目标角度后会被强行拉回中位。

**上电自检（2026-10-02 新增）**：开机时舵机会自动正反各转一下，约 1.2 秒，然后停下。这是故意加的诊断动作，用来一眼区分两类故障：

- 自检时舵机会转 → 舵机、PA6 接线、供电、PWM 全部正常，问题在网页／ESP32 链路。
- 自检时舵机不动 → 是硬件侧问题，跟网页无关。

不需要这个动作，就把 `servo_control.h` 里的 `SERVO_BOOT_SELFTEST` 改成 0 重新编译。


本目录已加入完整的 STM32 Keil 工程，不需要修改原项目。重新编译或烧录：

```powershell
.\build-stm32.ps1
.\build-stm32.ps1 -Upload
.\build-esp32.ps1 -Upload
```

STM32 上传脚本在写入前备份 128 KB Flash 到 `stm32/firmware/backups/`，然后下载并校验新 HEX。也可用 Keil 打开 `stm32/Project.uvprojx`。

舵机链路为：网页 `POST /api/servo` → ESP32 `angle=90` → USART2 `P090\n` → STM32 设置 PWM → 回执 `P090\n`。`Q\n` 只查询舵机目标，`M0\n` / `M1\n` 表示电机板未就绪 / 就绪。舵机命令、状态查询和角度回执不会刷新小车行驶超时计时器；舵机有自己的 700 ms 保持超时，超过就停止发脉冲，松开按钮或失去连接后都会停。两种控制各自使用命令序号，舵机请求不会使停车指令被忽略；迟到的网页状态也不会覆盖刚提交的角度。旧版 `V80S` STM32 固件仍能控制电机，但网页会禁用舵机按钮并提示更新。

## 5. AHT10 温湿度传感器（2026-10-02 新增）

用 I2C 接在 ESP32 上，读数显示在网页的「环境温湿度」面板里。

### 接线

| AHT10 模块 | ESP32-S3（YD-ESP32-S3） | 说明 |
| --- | --- | --- |
| VCC / VDD | **3V3** | AHT10 是 3.3 V 器件，**别接 5V**，除非模块上明确印了支持 5V |
| GND | GND | 必须和开发板共地 |
| SDA | **GPIO8** | 数据线 |
| SCL | **GPIO9** | 时钟线 |

SDA / SCL **不要接反**。模块上一般已经带了 4.7 kΩ 上拉电阻，不用再加；只有裸芯片才需要外接。

为什么选 GPIO8 / GPIO9：在 ESP32-S3 上这两个脚是确定空闲的普通 IO——不是 strapping 脚（0 / 45 / 46）、不是原生 USB（19 / 20）、不是串口 0（43 / 44）、不是 SPI Flash（26~32）、不是本板八线 PSRAM 占用的（33~37，本板是 N16R8 + qio_opi，这几个脚碰不得）、也不是板载 RGB 灯（48），并且避开了本工程已占用的 STM32 串口 17 / 18。

**如果和板上丝印对不上**，改 `esp32/include/project_config.h` 里的 `CLIMATE_I2C_SDA_PIN` / `CLIMATE_I2C_SCL_PIN` 即可，驱动代码不用动。改完重新烧录 ESP32。

### 网页显示

网页「手动驾驶」卡片下方新增「环境温湿度」面板，显示温度和湿度，并标注芯片型号、I2C 地址、数据新鲜度和累计采样次数。

没接传感器时会显示「未检测到」，并列出**当前 I2C 总线上扫到的所有地址**——这是排查接线最快的手段：

- 扫到 `无` → SDA/SCL 接错、没供电、或者没共地
- 扫到别的地址（比如 `0x76`）→ 接的不是 AHT10，或者总线上挂了别的器件
- 扫到 `0x38` 但仍显示未检测到 → 器件在，但初始化没过，检查供电是否稳定

### 代码

- `esp32/src/climate_sensor.h` / `.cpp`：AHT10 驱动。用状态机实现，`service()` 不阻塞主循环（AHT10 一次测量要 80 ms）。
- 同时兼容 **AHT20**：两者地址和读数据格式一样，只有初始化命令不同（AHT10 用 `0xE1`，AHT20 用 `0xBE`）。驱动两个都试，谁把「已校准」标志位置起来就用谁——市面上标着 AHT10 的模块里装的经常其实是 AHT20。
- 读数每 2 秒刷新一次，出现在 `/api/status` 的 `climate` 字段里：

```json
"climate": {
  "ok": true, "present": true, "chip": "AHT10", "address": "0x38",
  "temperature_c": 25.34, "humidity_pct": 60.12,
  "age_ms": 812, "samples": 37, "error": "正常", "i2c_scan": "0x38"
}
```

- 传感器没接或读不到时 `ok` 为 `false`，由 `error` 和 `i2c_scan` 给出原因；**不影响小车和舵机**。
- 引脚、读取周期等都在 `esp32/include/project_config.h` 里配置。

### 烧录

```powershell
.\build-esp32.ps1 -Upload
```

### 实测

2026-10-02：PlatformIO 编译成功（Flash 12.4%、RAM 14.8%），COM7 烧录 811136 字节并校验通过。启动日志确认 `I2C SDA=GPIO8 SCL=GPIO9 @100000Hz`，扫描结果为空（当时传感器还没接上）；`/api/status` 的 `climate` 字段正确返回 `ok:false`、`present:false`、`i2c_scan:"无"`，并经 `app.py` 中转后在网页侧读到正确的中文提示。接入传感器后的实际读数待测。

## 文件与验证

- `app.py`：本机网页服务，转发 CyberCAM 的 MJPEG、ESP32 状态与控制命令。
- `cybercam/`：已部署到 CyberCAM 的图像服务程序及 systemd 服务文件。
- `esp32/`：已烧录的 ESP32 固件独立副本；热点参数在 `esp32/include/project_config.h`，AHT10 的 I2C 引脚也在这里。
- `build-esp32.ps1`：在当前目录编译或烧录 ESP32。
- `stm32/`：包含 SG90 PWM 驱动和新串口协议的 STM32 独立工程。
- `build-stm32.ps1`：调用 Keil 编译，通过 ST-Link 备份、烧录和校验 STM32。
- `web/`：响应式网页和按住/松开控制逻辑。
- `tests/test_app.py`：使用模拟设备验证接口与过期命令保护。

测试命令：`py -3 -m unittest discover -s tests -v`。CyberCAM 到网页的视频、ESP32 编译和 COM7 烧录、ESP32 与 CyberCAM 同时连接电脑热点均已实测。STM32 接线后，ESP32 和网页均读到 `stm32_online:true`、`stm32_version:V80S`；经网页代理发送 `STOP` 也已成功。用户反馈已完成实车控制测试，一切正常；助手未远程触发运动指令。

2026-09-30 舵机更新：ESP32 编译成功并通过 COM7 烧录；STM32 为 0 错误、0 警告，通过 ST-Link 下载并回读校验成功，原固件已备份。6 项本机接口测试通过，包括舵机越界拒绝、旧角度命令丢弃、舵机与停车序号互不干扰。网页实际点击左转、右转、回中和操作滑条，STM32 分别回执 80°、100°、90°、99°；读回 TIM3 CCR1 确认 80° 为 1444 µs、100° 为 1556 µs，定时器 PSC=71、ARR=19999，对应 50 Hz PWM。验证结束后已回中 90°，小车始终为 STOP。机械实际转角未测量。

本次验收期间电机驱动板曾报告 `motor_ready:false`，舵机仍正常响应；初始化自动重试后已恢复 `motor_ready:true`，小车方向按钮也已恢复可用。CyberCAM 的原地址 `192.168.137.67:8081` 本次未响应，未改动板上的图像服务；摄像头上电并接入热点后，若 IP 改变，用 `--video-url` 指定实际地址。

2026-10-02 舵机改为按住才转：确认车上装的是 360° 连续旋转舵机（发 1556 µs 就会持续转动，位置舵机只会转 10° 后停住），原「点一下改 10°」的位置式控制不适用，已整体替换。

- `stm32/User/servo_control.c` 重写为寄存器直配 TIM3_CH1，去掉了会顺带改写 `AFIO_MAPR` 的 `SWJ_CFG` 位的 `GPIO_PinRemapConfig` 调用；对外接口未变。
- `remote_control.c` 增加舵机保持超时（`SERVO_HOLD_TIMEOUT_MS = 700`），超时自动回到 1500 µs 并回执 `P090`。
- 网页舵机面板改成「◀ 左转 / ■ 停止 / 右转 ▶」按住才转，移除角度滑条；增加 `pointerup` / `pointercancel` / `blur` / `visibilitychange` / `pagehide` 补发停止。
- 新增上电自检（`SERVO_BOOT_SELFTEST`），开机正反各转一下。

实测（Keil 0 Error / 0 Warning，Code=5932 字节；ST-Link 下载并回读校验通过）：按住右转时 TIM3 `CCR1 = 1667 µs`、左转 `1333 µs`、停止 `1500 µs`，与期望完全一致；**模拟网页掉线突然停发指令后，2 秒内 `CCR1` 自动回到 1500 µs，失联保护生效**。ESP32 侧读到 `stm32_online:true`、`servo_supported:true`。本次 ESP32 分到的地址是 `192.168.137.85`，不是文档里的 `192.168.137.127`。
