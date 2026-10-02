#include "servo_control.h"
#include "delay.h"

/*
 * MG90S / SG90 舵机驱动：PA6 = TIM3_CH1，硬件 PWM。
 *
 * 这一段用寄存器直接配置定时器，不依赖标准外设库的 TIM 封装，
 * 逻辑与已在实机上验证通过的独立测试工程完全一致：
 *     PSC = TIM3时钟/1MHz - 1   -> 1us 一个计数
 *     ARR = 20000 - 1           -> 20ms 周期 = 50Hz
 *     CCR1 = 1000 ~ 2000        -> 1.0 ~ 2.0ms 脉宽 -> 0 ~ 180 度
 * 已经用 ST-Link 读回寄存器确认过：PSC=71、ARR=19999、频率 50.00Hz。
 */

static uint16_t g_servo_angle = SERVO_CENTER_ANGLE;
/* 是否处于「不发脉冲」状态 */
static uint8_t g_servo_released = 0;

/* 角度 -> 脉宽（微秒），四舍五入到最近的微秒 */
static uint16_t angle_to_pulse(uint16_t angle)
{
	uint32_t pulse;

	if (SERVO_REVERSED != 0)
	{
		angle = (uint16_t)(180U - angle);
	}

	pulse = (uint32_t)SERVO_PULSE_MIN_US +
	        ((uint32_t)angle * (SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) + 90U) / 180U;

	return (uint16_t)pulse;
}

/*
 * TIM3 的计数时钟。
 * APB1 分频系数不为 1 时，挂在 APB1 上的定时器时钟 = PCLK1 x2，
 * 72MHz 系统下 PCLK1 = 36MHz，所以 TIM3 实际是 72MHz。
 * 这里不写死 72，是为了将来改主频也不会把 PWM 频率带偏。
 */
static uint32_t tim3_clock(void)
{
	RCC_ClocksTypeDef clocks;
	uint32_t clock;

	RCC_GetClocksFreq(&clocks);
	clock = clocks.PCLK1_Frequency;
	if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U)
	{
		clock *= 2U;
	}
	return clock;
}

void Servo_Init(void)
{
	GPIO_InitTypeDef gpio;
	uint32_t clock;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

	/*
	 * PA6 配置成复用推挽输出 50MHz。
	 * 这里刻意不调用 GPIO_PinRemapConfig()：TIM3 复位后默认就是不重映射，
	 * CH1 天然落在 PA6。而且标准的 GPIO_PinRemapConfig(DISABLE) 实现会顺手把
	 * AFIO_MAPR 的 SWJ_CFG 位写成 111，属于多余且有风险的副作用，能避就避。
	 */
	gpio.GPIO_Pin = GPIO_Pin_6;
	gpio.GPIO_Mode = GPIO_Mode_AF_PP;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &gpio);

	clock = tim3_clock();

	/* 计数频率 1MHz：1 个计数 = 1us */
	TIM3->PSC = (uint16_t)(clock / 1000000U - 1U);
	/* 20ms 一个周期 = 50Hz */
	TIM3->ARR = 20000U - 1U;
	/* 通道 1 设为 PWM 模式 1，并使能输出比较预装载 */
	TIM3->CCMR1 = (uint16_t)((TIM3->CCMR1 & 0xFF00U) |
	                         TIM_OCMode_PWM1 | TIM_OCPreload_Enable);
	/* 通道 1 输出使能，CC1P=0 表示高电平有效 */
	TIM3->CCER |= TIM_CCER_CC1E;
	/* 上电先给中位 1.5ms = 90 度，避免猛地甩到极限 */
	TIM3->CCR1 = angle_to_pulse(SERVO_CENTER_ANGLE);
	/* 产生一次更新事件，让 PSC/ARR 立刻载入影子寄存器 */
	TIM3->EGR = TIM_EGR_UG;
	/* 自动重装载预装载 + 启动计数器 */
	TIM3->CR1 |= (TIM_CR1_ARPE | TIM_CR1_CEN);

	g_servo_angle = SERVO_CENTER_ANGLE;
}

uint8_t Servo_SetAngle(uint16_t angle)
{
	if (angle < SERVO_MIN_ANGLE || angle > SERVO_MAX_ANGLE)
	{
		return 0;
	}

	TIM3->CCR1 = angle_to_pulse(angle);
	g_servo_angle = angle;
	g_servo_released = 0;
	return 1;
}

uint16_t Servo_GetAngle(void)
{
	return g_servo_angle;
}

/*
 * 停止发脉冲：CCR1 = 0，PWM 模式 1 下 PA6 恒低，一个脉冲都没有。
 *
 * 为什么不用「发 1500us」来停车：连续旋转舵机的停止点每一颗都不一样。
 * 这颗实测落在 1583~1667us 之间（约 1625us），根本不是标称的 1500us。
 * 固件猜不准这个值，硬写 1500us 只会让舵机在失联后慢慢爬。
 * 所以超时就干脆不发脉冲——舵机收不到脉冲就不会驱动电机，自然停下。
 * 标定好的停止点由上位机当作普通角度下发（Servo_SetAngle）。
 */
void Servo_Release(void)
{
	TIM3->CCR1 = 0U;
	g_servo_released = 1;
}

uint8_t Servo_IsReleased(void)
{
	return g_servo_released;
}

/* 在给定角度之间按步长平滑移动，每步之间等一会儿，避免舵机出现冲击 */
static void ramp_angle(int from, int to, int step, uint16_t step_ms)
{
	const int direction = (to >= from) ? 1 : -1;
	int angle = from;

	for (;;)
	{
		Servo_SetAngle((uint16_t)angle);
		if (angle == to)
		{
			break;
		}
		angle += direction * step;
		if ((direction > 0 && angle > to) || (direction < 0 && angle < to))
		{
			angle = to;
		}
		Delay_ms(step_ms);
	}
}

/*
 * 上电自检扫动：90 -> 30 -> 150 -> 90，约 1.2 秒。
 * 只在开机时跑一次，必须在 Delay_Init() 之后调用。
 */
void Servo_SelfTest(void)
{
	ramp_angle(SERVO_CENTER_ANGLE, SERVO_MIN_ANGLE, 5, 25);
	ramp_angle(SERVO_MIN_ANGLE, SERVO_MAX_ANGLE, 5, 25);
	ramp_angle(SERVO_MAX_ANGLE, SERVO_CENTER_ANGLE, 5, 25);
	g_servo_angle = SERVO_CENTER_ANGLE;
}
