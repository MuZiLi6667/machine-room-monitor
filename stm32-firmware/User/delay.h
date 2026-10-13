#ifndef __DELAY_H
#define __DELAY_H

#include "stm32f10x.h"

/* 基于 SysTick 的微秒/毫秒延时（72MHz 主频）
 * 注意：本模块独占 SysTick，不要再把 SysTick 用作系统节拍 */
void delay_init(void);
void delay_us(uint32_t nus);   /* 单次最长约 230ms（24位计数器限制） */
void delay_ms(uint16_t nms);

#endif
