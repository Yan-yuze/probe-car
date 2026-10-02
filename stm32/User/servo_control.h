#ifndef __SERVO_CONTROL_H
#define __SERVO_CONTROL_H

#include "stm32f10x.h"

/*
 * MG90S / SG90 舵机控制（PA6 = TIM3_CH1）
 * ---------------------------------------------------------------------------
 * 信号线固定接 PA6。TIM3_CH1 的默认复用功能就是 PA6，不做任何重映射。
 *
 * 舵机要的是 50Hz 方波，控制靠高电平宽度。
 *
 * 本车装的是 360° 连续旋转舵机，脉宽代表的是「转速」而不是角度：
 *     1500us        = 停止
 *     偏离 1500us    = 按偏差大小持续转动，大于/小于 1500us 决定转向
 * 所以上位机发的 30 / 90 / 150 实际含义是「一个方向 / 停 / 另一个方向」，
 * 而且必须按住才持续发，靠 SERVO_HOLD_TIMEOUT_MS 兜底停车。
 *
 * （如果以后换成普通位置舵机，1000/1500/2000us 就分别是 0/90/180 度。）
 *
 * TIM3 计数频率取 1MHz（1 个计数 = 1us），ARR = 20000-1 得到 20ms 周期，
 * CCR1 = 1000~2000 就是 1.0~2.0ms 的脉宽。硬件 PWM，CPU 只负责改 CCR1。
 * ---------------------------------------------------------------------------
 */

/* 角度范围。改这里时，ESP32 固件和网页 app.js 的限制必须同步改。 */
#define SERVO_MIN_ANGLE       30
#define SERVO_MAX_ANGLE      150
#define SERVO_CENTER_ANGLE    90

/* 保守标尺：0~180 度对应 1000~2000us。 */
#define SERVO_PULSE_MIN_US  1000
#define SERVO_PULSE_MAX_US  2000

/* 接线或安装方向相反时改成 1，左右就会对调。 */
#define SERVO_REVERSED          0

/*
 * 上电自检：开机时让舵机明显地转一下（约 1.2 秒）。
 * 用来一眼区分两类故障：自检会动 -> 硬件全好，问题在网页链路；
 * 自检不动 -> 硬件侧问题。不需要就改成 0。
 */
#define SERVO_BOOT_SELFTEST     1

/*
 * 按住才转的「保持超时」（毫秒）。
 *
 * 本车用的是 360° 连续旋转舵机：1500us 停，偏离 1500us 就按偏差大小持续转动。
 * 所以网页按住按钮时会每 180 ms 续发一次转速指令，松开就发 1500us 停。
 *
 * 有了这个超时，即使指令断流也能自动停下来：
 *   - 松开按钮后网页漏发停止指令
 *   - 关掉网页 / 浏览器崩溃
 *   - WiFi 掉线、ESP32 死机
 *   - 串口线断了
 * 只要超过这个时间没收到新的舵机指令，就调用 Servo_Release() 停止发脉冲。
 *
 * 注意：不能用「发 1500us」来停车。这颗舵机的真实停止点实测在 1625us 附近
 * （不是标称的 1500us），发 1500us 反而会让它慢慢转。
 * 真实停止点由网页标定后，当作普通角度下发。
 *
 * 如果以后换成普通角度舵机（能定位到某个角度），建议改成 0 关掉，
 * 否则舵机停在目标角度后会被释放、失去保持力矩。
 */
#define SERVO_HOLD_TIMEOUT_MS  700

void Servo_Init(void);
uint8_t Servo_SetAngle(uint16_t angle);
uint16_t Servo_GetAngle(void);
void Servo_Release(void);
uint8_t Servo_IsReleased(void);
void Servo_SelfTest(void);

#endif
