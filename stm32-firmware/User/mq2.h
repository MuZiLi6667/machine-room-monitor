#ifndef __MQ2_H
#define __MQ2_H

#include "stm32f10x.h"

/* MQ-2 烟雾传感器：AO 经分压后 -> PA0（ADC1 通道0，12位）
 * !! 注意：MQ-2 模块 5V 供电，AO 输出最高可达 5V，超过 STM32 ADC 上限 3.3V，
 *    必须外接分压电阻（如 AO--10k--+--20k--GND，中点接 PA0，电压≈2/3×AO）
 * 模块上电后需预热 2~3 分钟读数才稳定 */

void MQ2_Init(void);

/* 读取 ADC 原始值（0~4095），8 次采样取中值滤波 */
uint16_t MQ2_ReadRaw(void);

#endif
