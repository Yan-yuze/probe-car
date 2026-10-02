#ifndef __REMOTE_CONTROL_H
#define __REMOTE_CONTROL_H

#include "stm32f10x.h"

/*
 * ESP32-S3 <-> STM32F103C8 control link:
 *   USART2, 115200 baud, 8 data bits, no parity, 1 stop bit.
 *   PA3 receives F/B/L/R/S from ESP32 GPIO17.
 *   PA2 returns acknowledgements and the V-prefixed firmware version to ESP32 GPIO18.
 *   Pddd\n sets SG90 position (30..150); Q queries it; reply is Pddd\n.
 *   Servo packets and version queries never refresh the motor watchdog.
 */
void RemoteControl_Init(void);

/* Replies to a pending ESP32 version query outside the USART interrupt. */
void RemoteControl_Service(void);
void RemoteControl_SetMotorReady(uint8_t ready);

/* Returns 1 once for each newly received valid command. */
uint8_t RemoteControl_GetCommand(char *command);

/* True after at least one command was seen and no valid byte arrived in time. */
uint8_t RemoteControl_IsTimedOut(uint32_t timeout_ms);

/* Result frame: A<command>\n on success, E<command>\n on failure. */
void RemoteControl_SendResult(char command, uint8_t success);

/* Timeout notification used by the ESP32 status display. */
void RemoteControl_SendTimeout(void);

#endif
