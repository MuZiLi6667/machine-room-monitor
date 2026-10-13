#include "stm32f10x.h"
#include "delay.h"
#include "usart1.h"
#include "frame.h"
#include "dht11.h"
#include "mq2.h"
#include "bh1750.h"

/* ==================== 裁剪/模式配置 ==================== */
#define ENABLE_MQ2      1   /* 无 MQ-2 模块时置 0（帧内传感器个数自动减1，帧格式不变） */
#define ENABLE_BH1750   1   /* 无 BH1750 模块时置 0 */
#define SELF_TEST       0   /* 置 1：进入测试向量自检模式（对应开发文档 A2.2/A2.3 验收） */

#define REPORT_PERIOD_MS  2000   /* 上报周期 2s（接口规范 §1） */

static uint8_t frame_buf[FRAME_BUF_SIZE];

/* float → int16（×10，四舍五入，支持负值） */
static int16_t scale_x10(float v)
{
    return (int16_t)(v >= 0 ? (v * 10.0f + 0.5f) : (v * 10.0f - 0.5f));
}

#if SELF_TEST
/* ==================== 自检模式：发送接口规范 V1/V2/V3 测试向量 ====================
 * 烧录后用串口助手（HEX 显示）逐字节比对：
 *   V1: AA 55 01 0D 04 01 01 00 02 02 26 03 01 38 04 01 64 E3
 *   V2: AA 55 01 07 02 01 00 C8 02 01 C2 98
 *   V3: AA 55 01 07 02 01 FF CC 02 02 58 32
 * 三个向量逐字节一致即协议正确性验收通过。 */
int main(void)
{
    uint16_t len;

    /* V1 全4传感器：25.6℃ / 55.0% / 烟雾312 / 光照356 */
    const sensor_item_t v1[] = {
        {SENSOR_ID_TEMP,  256}, {SENSOR_ID_HUMI, 550},
        {SENSOR_ID_SMOKE, 312}, {SENSOR_ID_LUX,  356}
    };
    /* V2 仅温湿度：20.0℃ / 45.0% */
    const sensor_item_t v2[] = {
        {SENSOR_ID_TEMP, 200}, {SENSOR_ID_HUMI, 450}
    };
    /* V3 负温度：-5.2℃ / 60.0%（验证 int16 补码） */
    const sensor_item_t v3[] = {
        {SENSOR_ID_TEMP, -52}, {SENSOR_ID_HUMI, 600}
    };

    delay_init();
    usart1_init(115200);
    delay_ms(500);

    while (1) {
        len = build_sensor_frame(v1, 4, frame_buf);
        uart1_send_bytes(frame_buf, len);
        delay_ms(1000);

        len = build_sensor_frame(v2, 2, frame_buf);
        uart1_send_bytes(frame_buf, len);
        delay_ms(1000);

        len = build_sensor_frame(v3, 2, frame_buf);
        uart1_send_bytes(frame_buf, len);
        delay_ms(1000);
    }
}

#else
/* ==================== 正常模式：2s 周期采集 + 组帧 + 发送 ==================== */
int main(void)
{
    sensor_item_t items[4];
    uint8_t  n;
    uint16_t len;

    float    temp = 0.0f, humi = 0.0f;   /* 保持上次有效值用 */
    uint8_t  temp_valid = 0, humi_valid = 0;
    uint8_t  dht11_fail_count = 0;       /* 传感器状态：连续失败计数 */

    delay_init();
    usart1_init(115200);
    DHT11_Init();
#if ENABLE_MQ2
    MQ2_Init();
#endif
#if ENABLE_BH1750
    BH1750_Init();
#endif

    delay_ms(1000);   /* 等待外设上电稳定（DHT11 要求上电 1s 后再通信） */

    while (1) {
        n = 0;

        /* ---- DHT11 温湿度：失败沿用上次有效值，不得发送脏数据 ---- */
        {
            float t, h;
            if (DHT11_Read(&t, &h) == 0) {
                temp = t; humi = h;
                temp_valid = humi_valid = 1;
                dht11_fail_count = 0;
            } else {
                dht11_fail_count++;     /* 状态位：客户端可通过保持的数值观察到数据停滞 */
            }
            if (temp_valid)  { items[n].id = SENSOR_ID_TEMP; items[n].value = scale_x10(temp); n++; }
            if (humi_valid)  { items[n].id = SENSOR_ID_HUMI; items[n].value = scale_x10(humi); n++; }
        }

#if ENABLE_MQ2
        {
            uint16_t raw = MQ2_ReadRaw();          /* ADC 原始值，不缩放（接口规范 V1） */
            items[n].id = SENSOR_ID_SMOKE; items[n].value = (int16_t)raw; n++;
        }
#endif

#if ENABLE_BH1750
        {
            uint16_t lux = BH1750_ReadLux();       /* lux 整数，不缩放 */
            items[n].id = SENSOR_ID_LUX; items[n].value = (int16_t)lux; n++;
        }
#endif

        /* ---- 组帧发送（传感器个数随裁剪自适应，帧格式不变） ---- */
        len = build_sensor_frame(items, n, frame_buf);
        uart1_send_bytes(frame_buf, len);

        delay_ms(REPORT_PERIOD_MS);
    }
}
#endif
