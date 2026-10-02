#include "stm32f10x.h"
#include "car_control.h"
#include "delay.h"
#include "remote_control.h"
#include "servo_control.h"

/* 80 mm/s 提供比 50 mm/s 更连续的编码器反馈，同时保持低速。 */
#define DRIVE_SPEED_MM_S          80
#define TURN_SPEED_MM_S           80
#define MOTION_REFRESH_MS         10

/*
 * 松键或失联时，先沿上一动作的相反方向施加短脉冲，再撤销全部 PWM。
 * 若停车时出现反向顿挫，逐步缩短制动时间；若仍滑行则逐步增加。
 */
#define ACTIVE_BRAKE_SPEED_MM_S   60
#define ACTIVE_BRAKE_TIME_MS      40
#define COMMAND_TIMEOUT_MS      700
#define STOP_REFRESH_MS         1000

/* Blue Pill onboard LED, active low. On means a motion command is active. */
#define LED_PORT GPIOC
#define LED_PIN  GPIO_Pin_13

static void LED_Init(void)
{
	GPIO_InitTypeDef gpio;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
	gpio.GPIO_Pin = LED_PIN;
	gpio.GPIO_Mode = GPIO_Mode_Out_PP;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(LED_PORT, &gpio);
	GPIO_SetBits(LED_PORT, LED_PIN);
}

static void LED_SetMoving(uint8_t moving)
{
	if (moving != 0)
	{
		GPIO_ResetBits(LED_PORT, LED_PIN);
	}
	else
	{
		GPIO_SetBits(LED_PORT, LED_PIN);
	}
}

static uint8_t ApplyMotionCommand(char command)
{
	switch (command)
	{
		case 'F': return Car_Forward(DRIVE_SPEED_MM_S);
		case 'B': return Car_Backward(DRIVE_SPEED_MM_S);
		case 'L': return Car_TurnLeft(TURN_SPEED_MM_S);
		case 'R': return Car_TurnRight(TURN_SPEED_MM_S);
		case 'S': return Car_StopAll();
		default:  return 1;
	}
}

static uint8_t StopWithActiveBrake(char previous_command)
{
	uint8_t result;

	switch (previous_command)
	{
		case 'F': result = Car_Backward(ACTIVE_BRAKE_SPEED_MM_S); break;
		case 'B': result = Car_Forward(ACTIVE_BRAKE_SPEED_MM_S); break;
		case 'L': result = Car_TurnRight(ACTIVE_BRAKE_SPEED_MM_S); break;
		case 'R': result = Car_TurnLeft(ACTIVE_BRAKE_SPEED_MM_S); break;
		default: return Car_StopAll();
	}

	if (result != 0)
	{
		Car_StopAll();
		return result;
	}
	Delay_ms(ACTIVE_BRAKE_TIME_MS);
	return Car_StopAll();
}

int main(void)
{
	char command;
	char current_command = 'S';
	uint32_t last_motion_refresh_ms;
	uint32_t last_stop_refresh_ms;
	uint32_t last_motor_retry_ms;
	uint8_t motor_ready;

	Delay_Init();
	LED_Init();
	Servo_Init();

	/*
	 * 上电自检扫动：90 -> 30 -> 150 -> 90，约 1.2 秒。
	 * 放在 Car_Init()/RemoteControl_Init() 之前，既不会拖慢串口握手，
	 * 也让「舵机动没动」在开机瞬间就能看出来。
	 * 不需要就把 servo_control.h 里的 SERVO_BOOT_SELFTEST 改成 0。
	 */
#if SERVO_BOOT_SELFTEST
	Servo_SelfTest();
#endif

	/*
	 * 自检结束后先停止发脉冲，等网页把标定好的停止点发下来。
	 * 这颗舵机的真实停止点不在 1500us，固件猜不准，乱发只会让它慢慢转。
	 */
	Servo_Release();

	motor_ready = (Car_Init() == 0) ? 1 : 0;

	/* Motors are already stopped before the receive interrupt becomes active. */
	RemoteControl_Init();
	RemoteControl_SetMotorReady(motor_ready);
	LED_SetMoving(0);
	last_motion_refresh_ms = millis();
	last_stop_refresh_ms = millis();
	last_motor_retry_ms = millis();

	while (1)
	{
		RemoteControl_Service();

		/* A missing motor board must not block the independent SG90 link. */
		if (motor_ready == 0)
		{
			if (RemoteControl_GetCommand(&command) != 0)
			{
				RemoteControl_SendResult(command, command == 'S');
			}
			if ((millis() / 150U) % 2U == 0U)
			{
				GPIO_ResetBits(LED_PORT, LED_PIN);
			}
			else
			{
				GPIO_SetBits(LED_PORT, LED_PIN);
			}
			if ((uint32_t)(millis() - last_motor_retry_ms) >= 2000U)
			{
				motor_ready = (Car_Init() == 0) ? 1 : 0;
				last_motor_retry_ms = millis();
				/* Never replay motion received while initialization was in progress. */
				RemoteControl_GetCommand(&command);
				current_command = 'S';
				LED_SetMoving(0);
				RemoteControl_SetMotorReady(motor_ready);
			}
			continue;
		}

		if (RemoteControl_GetCommand(&command) != 0)
		{
			uint8_t result = 0;

			/* Heartbeats refresh the UART timeout; the block below refreshes I2C at 100 Hz. */
			if (command != current_command)
			{
				if (current_command != 'S')
				{
					result = StopWithActiveBrake(current_command);
				}
				if (result == 0 && command != 'S')
				{
					result = ApplyMotionCommand(command);
					last_motion_refresh_ms = millis();
				}
			}

			if (result == 0)
			{
				current_command = command;
				LED_SetMoving(command != 'S');
			}
			else
			{
				Car_StopAll();
				current_command = 'S';
				LED_SetMoving(0);
				motor_ready = 0;
				RemoteControl_SetMotorReady(0);
			}
			RemoteControl_SendResult(command, result == 0);
		}

		/* A broken UART cable or a crashed ESP32 can never leave the car moving. */
		if (current_command != 'S' &&
		    RemoteControl_IsTimedOut(COMMAND_TIMEOUT_MS) != 0)
		{
			StopWithActiveBrake(current_command);
			current_command = 'S';
			LED_SetMoving(0);
			RemoteControl_SendTimeout();
			last_stop_refresh_ms = millis();
		}

		/* Match the proven tracking project: refresh the closed-loop target at 100 Hz. */
		if (current_command != 'S' &&
		    (uint32_t)(millis() - last_motion_refresh_ms) >= MOTION_REFRESH_MS)
		{
			char failed_command = current_command;
			uint8_t result = ApplyMotionCommand(current_command);

			last_motion_refresh_ms = millis();
			if (result != 0)
			{
				Car_StopAll();
				current_command = 'S';
				LED_SetMoving(0);
				RemoteControl_SendResult(failed_command, 0);
				motor_ready = 0;
				RemoteControl_SetMotorReady(0);
				last_stop_refresh_ms = millis();
			}
		}

		/* Reassert zero speed periodically, preserving the original project's safety. */
		if (current_command == 'S' &&
		    (uint32_t)(millis() - last_stop_refresh_ms) >= STOP_REFRESH_MS)
		{
			Car_StopAll();
			last_stop_refresh_ms = millis();
		}
	}
}
