#ifndef __BH1750_H
#define __BH1750_H

#include "stm32f10x.h"

/* BH1750 光照传感器：SCL->PB6，SDA->PB7（I2C1 引脚），ADDR 接地（器件地址 0x23），3.3V 供电
 * 实现方式：软件模拟 I2C（时序直观稳定，避开 F1 硬件 I2C 的已知坑），引脚与 I2C1 一致 */

void BH1750_Init(void);

/* 读取光照强度（lux，整数）。连续高分辨率模式，内部自动等待 180ms 转换时间 */
uint16_t BH1750_ReadLux(void);

#endif
