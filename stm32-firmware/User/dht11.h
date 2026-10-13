#ifndef __DHT11_H
#define __DHT11_H

#include "stm32f10x.h"

/* DHT11 温湿度传感器：DATA -> PB5，VCC 3.3V，DATA 需 4.7k~10k 上拉（模块自带可省）
 * 两次读取间隔必须 >= 1s（主循环 2s 周期天然满足） */

void DHT11_Init(void);

/* 读取温湿度
 * 返回：0 成功；非0 失败（超时/校验错）
 * 失败时 *temp / *humi 不被修改（由调用方沿用上次有效值） */
uint8_t DHT11_Read(float *temp, float *humi);

#endif
