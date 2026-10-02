#ifndef __CAR_CONTROL_H
#define __CAR_CONTROL_H

#include "stm32f10x.h"

/* 初始化 I2C1，确认驱动板在线并写入本车 JGB37-520 闭环参数。 */
uint8_t Car_Init(void);

/* 通过 0x06 寄存器设置 1~4 号电机目标速度，单位 mm/s。 */
uint8_t Car_ControlMotorRaw(int16_t motor1, int16_t motor2,
                            int16_t motor3, int16_t motor4);

/* 通过 PWM 寄存器清零全部输出。 */
uint8_t Car_StopAll(void);

/* 设置逻辑左右轮闭环速度；实车接线为 M1=右轮、M2=左轮。 */
uint8_t Car_SetWheelSpeed(int16_t left_speed, int16_t right_speed);

/* 两轮同时前进。 */
uint8_t Car_Forward(int16_t speed);

/* 两轮同时后退。 */
uint8_t Car_Backward(int16_t speed);

/* 原地左转：左轮后退，右轮前进。 */
uint8_t Car_TurnLeft(int16_t speed);

/* 原地右转：左轮前进，右轮后退。 */
uint8_t Car_TurnRight(int16_t speed);

#endif
