#include "remote_control.h"
#include "delay.h"
#include "servo_control.h"

#define STM32_FIRMWARE_VERSION "V80SG90"

static volatile char g_pending_command = 'S';
static volatile uint8_t g_command_pending = 0;
static volatile uint8_t g_version_request_pending = 0;
static volatile uint8_t g_link_seen = 0;
static volatile uint32_t g_last_receive_ms = 0;
static volatile uint8_t g_servo_pending = 0;
static volatile uint16_t g_pending_servo_angle = SERVO_CENTER_ANGLE;
static volatile uint8_t g_servo_query_pending = 0;
static uint8_t g_servo_receiving = 0;
static uint8_t g_servo_digits = 0;
static uint16_t g_servo_value = 0;
static uint32_t g_servo_frame_started_ms = 0;
static uint8_t g_motor_ready = 0;
/* 最近一次真正下发舵机指令的时间，用于按住才转的失联停车保护。 */
static volatile uint32_t g_last_servo_command_ms = 0;

static uint8_t is_valid_command(char command)
{
	return (command == 'F' || command == 'B' || command == 'L' ||
	        command == 'R' || command == 'S');
}

static void send_byte(char value)
{
	while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
	{
	}
	USART_SendData(USART2, (uint16_t)(uint8_t)value);
}

static void send_text(const char *text)
{
	while (*text != '\0')
	{
		send_byte(*text++);
	}
}

static void send_version(void)
{
	send_text(STM32_FIRMWARE_VERSION);
	send_byte('\n');
}

static void send_servo_angle(void)
{
	uint16_t angle = Servo_GetAngle();
	send_byte('P');
	send_byte((char)('0' + angle / 100U));
	send_byte((char)('0' + (angle / 10U) % 10U));
	send_byte((char)('0' + angle % 10U));
	send_byte('\n');
}

static void send_motor_ready(void)
{
	send_text(g_motor_ready != 0 ? "M1\n" : "M0\n");
}

void RemoteControl_SetMotorReady(uint8_t ready)
{
	g_motor_ready = ready;
	send_motor_ready();
}

void RemoteControl_Init(void)
{
	GPIO_InitTypeDef gpio;
	USART_InitTypeDef usart;
	NVIC_InitTypeDef nvic;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

	/* PA2 = USART2_TX, connected to ESP32 GPIO18 when acknowledgements are used. */
	gpio.GPIO_Pin = GPIO_Pin_2;
	gpio.GPIO_Mode = GPIO_Mode_AF_PP;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &gpio);

	/* PA3 = USART2_RX, connected to ESP32 GPIO17. */
	gpio.GPIO_Pin = GPIO_Pin_3;
	/* Pull up to the UART idle level so a disconnected cable cannot float. */
	gpio.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_Init(GPIOA, &gpio);

	USART_StructInit(&usart);
	usart.USART_BaudRate = 115200;
	usart.USART_WordLength = USART_WordLength_8b;
	usart.USART_StopBits = USART_StopBits_1;
	usart.USART_Parity = USART_Parity_No;
	usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
	USART_Init(USART2, &usart);

	nvic.NVIC_IRQChannel = USART2_IRQn;
	nvic.NVIC_IRQChannelPreemptionPriority = 1;
	nvic.NVIC_IRQChannelSubPriority = 0;
	nvic.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&nvic);

	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
	USART_Cmd(USART2, ENABLE);

	/* Let the ESP32 know that the STM32 control firmware has booted. */
	send_text("R\n");
	/* Also broadcast once; ESP32 can request it again with V. */
	send_version();
	send_servo_angle();
	g_last_servo_command_ms = millis();
}

void RemoteControl_Service(void)
{
	uint8_t version_requested;
	uint8_t servo_pending;
	uint8_t servo_requested;
	uint16_t servo_angle;

	__disable_irq();
	version_requested = g_version_request_pending;
	g_version_request_pending = 0;
	servo_pending = g_servo_pending;
	servo_angle = g_pending_servo_angle;
	g_servo_pending = 0;
	servo_requested = g_servo_query_pending;
	g_servo_query_pending = 0;
	__enable_irq();

	if (version_requested != 0)
	{
		send_version();
		send_motor_ready();
	}
	/* Hardware PWM keeps running; changing CCR never blocks motor control. */
	if (servo_pending != 0)
	{
		Servo_SetAngle(servo_angle);
		g_last_servo_command_ms = millis();
	}

	/*
	 * 按住才转的兜底停车。
	 * 连续旋转舵机的真实停止点每一颗都不一样（这颗实测约 1625us，不是 1500us），
	 * 固件猜不准，所以超时不发固定脉宽，而是直接停止发脉冲：
	 * 舵机收不到脉冲就不会驱动电机，自然停下。
	 * 松开按钮、关掉网页、WiFi 掉线、ESP32 死机、串口断开，全都能兜住。
	 */
#if SERVO_HOLD_TIMEOUT_MS > 0
	if (!Servo_IsReleased() &&
	    (uint32_t)(millis() - g_last_servo_command_ms) > (uint32_t)SERVO_HOLD_TIMEOUT_MS)
	{
		Servo_Release();
		g_last_servo_command_ms = millis();
		/* 借用已有的回执标志，把状态同步给 ESP32 和网页。 */
		servo_requested = 1;
	}
#endif

	if (servo_pending != 0 || servo_requested != 0 || version_requested != 0)
	{
		send_servo_angle();
	}
}

uint8_t RemoteControl_GetCommand(char *command)
{
	uint8_t available = 0;

	__disable_irq();
	if (g_command_pending != 0)
	{
		*command = g_pending_command;
		g_command_pending = 0;
		available = 1;
	}
	__enable_irq();

	return available;
}

uint8_t RemoteControl_IsTimedOut(uint32_t timeout_ms)
{
	if (g_link_seen == 0)
	{
		return 0;
	}
	return ((uint32_t)(millis() - g_last_receive_ms) > timeout_ms) ? 1 : 0;
}

void RemoteControl_SendResult(char command, uint8_t success)
{
	send_byte(success != 0 ? 'A' : 'E');
	send_byte(command);
	send_byte('\n');
}

void RemoteControl_SendTimeout(void)
{
	send_byte('T');
	send_byte('\n');
}

void USART2_IRQHandler(void)
{
	if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
	{
		char command = (char)(USART_ReceiveData(USART2) & 0xFF);
		if (g_servo_receiving != 0 &&
		    (uint32_t)(millis() - g_servo_frame_started_ms) > 100U)
		{
			g_servo_receiving = 0;
		}
		/* STOP takes priority, including during an incomplete servo frame. */
		if (command == 'S')
		{
			g_servo_receiving = 0;
			g_pending_command = command;
			g_command_pending = 1;
			g_link_seen = 1;
			g_last_receive_ms = millis();
		}
		else if (g_servo_receiving != 0)
		{
			if (command == '\n' || command == '\r')
			{
				if (g_servo_digits == 3U &&
				    g_servo_value >= SERVO_MIN_ANGLE && g_servo_value <= SERVO_MAX_ANGLE)
				{
					g_pending_servo_angle = g_servo_value;
					g_servo_pending = 1;
				}
				g_servo_receiving = 0;
			}
			else if (command >= '0' && command <= '9' && g_servo_digits < 3U)
			{
				g_servo_value = (uint16_t)(g_servo_value * 10U + (uint16_t)(command - '0'));
				g_servo_digits++;
			}
			else
			{
				/* Ignore a malformed frame until its delimiter. */
				g_servo_digits = 4U;
			}
		}
		else if (command == 'P')
		{
			g_servo_receiving = 1;
			g_servo_digits = 0;
			g_servo_value = 0;
			g_servo_frame_started_ms = millis();
		}
		else if (command == 'Q')
		{
			g_servo_query_pending = 1;
		}
		else if (command == 'V')
		{
			g_version_request_pending = 1;
		}
		else if (is_valid_command(command) != 0)
		{
			g_pending_command = command;
			g_command_pending = 1;
			g_link_seen = 1;
			g_last_receive_ms = millis();
		}
	}
}
