#ifndef __FRAME_H
#define __FRAME_H

#include "stm32f10x.h"

/* ================= 串口帧协议（与 docs/接口规范.md §2 逐字节一致） =================
 * 帧头(2B): 0xAA 0x55
 * 帧类型(1B): 0x01 = 传感器数据帧
 * 数据长度(1B): N = 1 + 3*n
 * 数据域(N B): 传感器个数n(1B) + n × ( 传感器编号(1B) + 数值(2B 大端有符号int16) )
 * 校验和(1B): 从帧类型字节到数据域末字节逐字节累加，取低8位
 *
 * 数值缩放规则（依接口规范测试向量 V1/V2/V3）：
 *   温度/湿度 = 真实值 × 10（int16，支持负温度补码）
 *   烟雾/光照 = 原始整数值（不乘10）
 * ================================================================================ */

#define FRAME_HEAD0     0xAA
#define FRAME_HEAD1     0x55
#define FRAME_TYPE_DATA 0x01

/* 传感器编号（接口规范 §1） */
#define SENSOR_ID_TEMP  1   /* 温度 ℃    value = ℃ × 10 */
#define SENSOR_ID_HUMI  2   /* 湿度 %RH  value = %RH × 10 */
#define SENSOR_ID_SMOKE 3   /* 烟雾 ADC原始值 0~4095（不缩放） */
#define SENSOR_ID_LUX   4   /* 光照 lux  整数（不缩放） */

#define FRAME_MAX_SENSORS 8
#define FRAME_BUF_SIZE    (4 + 1 + 3 * FRAME_MAX_SENSORS + 1)

typedef struct {
    uint8_t id;         /* 传感器编号 1~4 */
    int16_t value;      /* 已按上方规则缩放后的数值 */
} sensor_item_t;

/* 组帧：返回帧总长（字节），帧内容写入 buf（调用方保证 >= FRAME_BUF_SIZE） */
uint16_t build_sensor_frame(const sensor_item_t *sensors, uint8_t n, uint8_t *buf);

#endif
