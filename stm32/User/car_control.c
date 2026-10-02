#include "car_control.h"
#include "delay.h"

/* 电机驱动板通信参数。 */
#define MOTOR_I2C                 I2C1
#define MOTOR_ADDR_7BIT           0x26
#define MOTOR_TYPE_REG            0x01
#define MOTOR_DEADZONE_REG        0x02
#define MOTOR_PULSE_LINE_REG      0x03
#define MOTOR_REDUCTION_REG       0x04
#define MOTOR_WHEEL_DIA_REG       0x05
#define MOTOR_SPEED_REG           0x06
#define MOTOR_PWM_REG             0x07
#define I2C_TIMEOUT_MS            50
#define MOTOR_SPEED_LIMIT         1000

/* 循迹工程已在本车验证的 JGB37-520 闭环参数。 */
#define MOTOR_TYPE_520            1U
#define MOTOR_DEADZONE_520        1500U
#define MOTOR_PULSE_LINE_520      11U
#define MOTOR_REDUCTION_520       30U
#define MOTOR_WHEEL_DIA_520       68.0f

/* 8 路循迹工程已经在同一台实车上验证过的端口和方向。 */
#define RIGHT_WHEEL_MOTOR_INDEX   1
#define LEFT_WHEEL_MOTOR_INDEX    2
#define RIGHT_WHEEL_FORWARD_SIGN -1
#define LEFT_WHEEL_FORWARD_SIGN   1

static int16_t clamp_speed(int16_t value)
{
	if (value > MOTOR_SPEED_LIMIT)
	{
		return MOTOR_SPEED_LIMIT;
	}
	if (value < -MOTOR_SPEED_LIMIT)
	{
		return -MOTOR_SPEED_LIMIT;
	}
	return value;
}

static void put_i16_be(uint8_t *dst, int16_t value)
{
	uint16_t raw = (uint16_t)value;
	dst[0] = (uint8_t)(raw >> 8);
	dst[1] = (uint8_t)(raw & 0xFF);
}

static uint8_t wait_flag(uint32_t flag, FlagStatus status)
{
	uint32_t start = millis();
	while (I2C_GetFlagStatus(MOTOR_I2C, flag) != status)
	{
		if ((uint32_t)(millis() - start) > I2C_TIMEOUT_MS)
		{
			return 1;
		}
	}
	return 0;
}

static uint8_t wait_event(uint32_t event)
{
	uint32_t start = millis();
	while (I2C_CheckEvent(MOTOR_I2C, event) != SUCCESS)
	{
		if (I2C_GetFlagStatus(MOTOR_I2C, I2C_FLAG_AF) == SET)
		{
			I2C_ClearFlag(MOTOR_I2C, I2C_FLAG_AF);
			return 1;
		}
		if ((uint32_t)(millis() - start) > I2C_TIMEOUT_MS)
		{
			return 1;
		}
	}
	return 0;
}

static uint8_t motor_write_reg(uint8_t reg,
                               const uint8_t *data,
                               uint8_t length)
{
	uint8_t i;

	if (wait_flag(I2C_FLAG_BUSY, RESET) != 0)
	{
		return 1;
	}

	I2C_GenerateSTART(MOTOR_I2C, ENABLE);
	if (wait_event(I2C_EVENT_MASTER_MODE_SELECT) != 0)
	{
		return 2;
	}

	I2C_Send7bitAddress(MOTOR_I2C, MOTOR_ADDR_7BIT << 1,
	                    I2C_Direction_Transmitter);
	if (wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) != 0)
	{
		I2C_GenerateSTOP(MOTOR_I2C, ENABLE);
		return 3;
	}

	I2C_SendData(MOTOR_I2C, reg);
	if (wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != 0)
	{
		I2C_GenerateSTOP(MOTOR_I2C, ENABLE);
		return 4;
	}

	for (i = 0; i < length; i++)
	{
		I2C_SendData(MOTOR_I2C, data[i]);
		if (wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != 0)
		{
			I2C_GenerateSTOP(MOTOR_I2C, ENABLE);
			return 5;
		}
	}

	I2C_GenerateSTOP(MOTOR_I2C, ENABLE);
	return 0;
}

static uint8_t motor_write_u8(uint8_t reg, uint8_t value)
{
	return motor_write_reg(reg, &value, 1U);
}

static uint8_t motor_write_u16(uint8_t reg, uint16_t value)
{
	uint8_t data[2];

	data[0] = (uint8_t)(value >> 8);
	data[1] = (uint8_t)(value & 0xFFU);
	return motor_write_reg(reg, data, sizeof(data));
}

static uint8_t motor_write_float(uint8_t reg, float value)
{
	union
	{
		float number;
		uint8_t bytes[4];
	} raw;

	raw.number = value;
	return motor_write_reg(reg, raw.bytes, sizeof(raw.bytes));
}

static uint8_t Motor_Configure520(void)
{
	if (motor_write_u8(MOTOR_TYPE_REG, MOTOR_TYPE_520) != 0U)
	{
		return 1U;
	}
	Delay_ms(100);
	if (motor_write_u16(MOTOR_REDUCTION_REG, MOTOR_REDUCTION_520) != 0U)
	{
		return 2U;
	}
	Delay_ms(100);
	if (motor_write_u16(MOTOR_PULSE_LINE_REG, MOTOR_PULSE_LINE_520) != 0U)
	{
		return 3U;
	}
	Delay_ms(100);
	if (motor_write_float(MOTOR_WHEEL_DIA_REG, MOTOR_WHEEL_DIA_520) != 0U)
	{
		return 4U;
	}
	Delay_ms(100);
	if (motor_write_u16(MOTOR_DEADZONE_REG, MOTOR_DEADZONE_520) != 0U)
	{
		return 5U;
	}
	Delay_ms(100);
	return 0U;
}

static uint8_t motor_board_ready(void)
{
	uint8_t ready = 0;

	if (wait_flag(I2C_FLAG_BUSY, RESET) != 0)
	{
		return 0;
	}

	I2C_GenerateSTART(MOTOR_I2C, ENABLE);
	if (wait_event(I2C_EVENT_MASTER_MODE_SELECT) == 0)
	{
		I2C_Send7bitAddress(MOTOR_I2C, MOTOR_ADDR_7BIT << 1,
		                    I2C_Direction_Transmitter);
		if (wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) == 0)
		{
			ready = 1;
		}
	}
	I2C_GenerateSTOP(MOTOR_I2C, ENABLE);
	return ready;
}

static void Motor_I2C_Init(void)
{
	GPIO_InitTypeDef gpio;
	I2C_InitTypeDef i2c;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);

	/* PB6=SCL、PB7=SDA；I2C 必须采用复用开漏输出。 */
	gpio.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
	gpio.GPIO_Mode = GPIO_Mode_AF_OD;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB, &gpio);

	I2C_DeInit(MOTOR_I2C);
	I2C_StructInit(&i2c);
	i2c.I2C_Mode = I2C_Mode_I2C;
	i2c.I2C_DutyCycle = I2C_DutyCycle_2;
	i2c.I2C_OwnAddress1 = 0x00;
	i2c.I2C_Ack = I2C_Ack_Enable;
	i2c.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
	i2c.I2C_ClockSpeed = 100000;
	I2C_Init(MOTOR_I2C, &i2c);
	I2C_Cmd(MOTOR_I2C, ENABLE);
}

uint8_t Car_Init(void)
{
	Motor_I2C_Init();
	Delay_ms(100);
	if (motor_board_ready() == 0)
	{
		return 1;
	}
	if (Motor_Configure520() != 0U)
	{
		return 2;
	}
	return Car_StopAll();
}

uint8_t Car_ControlMotorRaw(int16_t motor1, int16_t motor2,
                            int16_t motor3, int16_t motor4)
{
	uint8_t data[8];

	put_i16_be(&data[0], motor1);
	put_i16_be(&data[2], motor2);
	put_i16_be(&data[4], motor3);
	put_i16_be(&data[6], motor4);
	return motor_write_reg(MOTOR_SPEED_REG, data, sizeof(data));
}

uint8_t Car_StopAll(void)
{
	uint8_t data[8] = {0};

	return motor_write_reg(MOTOR_PWM_REG, data, sizeof(data));
}

uint8_t Car_SetWheelSpeed(int16_t left_speed, int16_t right_speed)
{
	int16_t motor[4] = {0, 0, 0, 0};

	left_speed = clamp_speed(left_speed);
	right_speed = clamp_speed(right_speed);
	motor[RIGHT_WHEEL_MOTOR_INDEX - 1] =
		(int16_t)(RIGHT_WHEEL_FORWARD_SIGN * right_speed);
	motor[LEFT_WHEEL_MOTOR_INDEX - 1] =
		(int16_t)(LEFT_WHEEL_FORWARD_SIGN * left_speed);

	return Car_ControlMotorRaw(motor[0], motor[1], motor[2], motor[3]);
}

uint8_t Car_Forward(int16_t speed)
{
	return Car_SetWheelSpeed(speed, speed);
}

uint8_t Car_Backward(int16_t speed)
{
	return Car_SetWheelSpeed((int16_t)-speed, (int16_t)-speed);
}

uint8_t Car_TurnLeft(int16_t speed)
{
	return Car_SetWheelSpeed((int16_t)-speed, speed);
}

uint8_t Car_TurnRight(int16_t speed)
{
	return Car_SetWheelSpeed(speed, (int16_t)-speed);
}
