#include "frame.h"

uint16_t build_sensor_frame(const sensor_item_t *sensors, uint8_t n, uint8_t *buf)
{
    uint8_t  i;
    uint8_t  sum = 0;
    uint16_t pos = 0;
    uint8_t  data_len = (uint8_t)(1 + 3 * n);   /* 个数1B + n×(编号1B+数值2B) */

    if (n > FRAME_MAX_SENSORS) {
        n = FRAME_MAX_SENSORS;
        data_len = (uint8_t)(1 + 3 * n);
    }

    buf[pos++] = FRAME_HEAD0;        /* 帧头 0xAA */
    buf[pos++] = FRAME_HEAD1;        /* 帧头 0x55 */
    buf[pos++] = FRAME_TYPE_DATA;    /* 帧类型 0x01 */
    buf[pos++] = data_len;           /* 数据长度 N */

    buf[pos++] = n;                  /* 传感器个数 */
    for (i = 0; i < n; i++) {
        buf[pos++] = sensors[i].id;
        buf[pos++] = (uint8_t)((sensors[i].value >> 8) & 0xFF);  /* 大端高字节 */
        buf[pos++] = (uint8_t)(sensors[i].value & 0xFF);         /* 大端低字节 */
    }

    /* 校验和：帧类型字节 ~ 数据域末字节，即 buf[2] ~ buf[pos-1] */
    for (i = 2; i < pos; i++) {
        sum += buf[i];
    }
    buf[pos++] = sum;                /* 校验和低8位（uint8 自然 mod 256） */

    return pos;                      /* 总长 = 4 + N + 1 */
}
