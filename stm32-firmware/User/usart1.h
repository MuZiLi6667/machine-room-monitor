#ifndef __USART1_H
#define __USART1_H

#include "stm32f10x.h"

/* USART1：PA9(TX) / PA10(RX)，115200-8-N-1
 * 经 USB转TTL 模块连接 PC 客户端（任务B） */
void usart1_init(uint32_t baudrate);
void uart1_send_bytes(const uint8_t *buf, uint16_t len);
void uart1_send_byte(uint8_t byte);

#endif
