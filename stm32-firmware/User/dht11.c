#include "dht11.h"
#include "delay.h"

#define DHT11_PORT      GPIOB
#define DHT11_PIN       GPIO_Pin_5
#define DHT11_CLK       RCC_APB2Periph_GPIOB

#define DHT11_OUT_HIGH  GPIO_SetBits(DHT11_PORT, DHT11_PIN)
#define DHT11_OUT_LOW   GPIO_ResetBits(DHT11_PORT, DHT11_PIN)
#define DHT11_IN        GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN)

/* 等待引脚变为指定电平，带超时（单位约 us）。返回剩余超时计数，0=超时 */
static uint16_t dht11_wait_level(uint8_t level, uint16_t timeout_us)
{
    while (DHT11_IN != level) {
        if (timeout_us-- == 0) return 0;
        delay_us(1);
    }
    return timeout_us;
}

static void dht11_pin_output(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.GPIO_Pin   = DHT11_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_OD;   /* 开漏输出，配合上拉 */
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(DHT11_PORT, &GPIO_InitStructure);
    DHT11_OUT_HIGH;
}

static void dht11_pin_input(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.GPIO_Pin  = DHT11_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;  /* 靠外部上拉 */
    GPIO_Init(DHT11_PORT, &GPIO_InitStructure);
}

void DHT11_Init(void)
{
    RCC_APB2PeriphClockCmd(DHT11_CLK, ENABLE);
    dht11_pin_output();
    DHT11_OUT_HIGH;
}

/* 读取一个 bit：格式为 [50us 低前导][高电平：26~28us=0，70us=1]
 * 必须先等引脚变低（确保上一位的高电平已结束、重新对齐），
 * 再等前导低电平结束（变高），最后延时 40us 采样 */
static uint8_t dht11_read_bit(void)
{
    if (dht11_wait_level(0, 100) == 0) return 0xFF;   /* 先等高电平结束（对齐到本 bit 前导低） */
    if (dht11_wait_level(1, 100) == 0) return 0xFF;   /* 再等前导低电平结束（引脚变高） */
    delay_us(40);                                     /* 40us 后采样：bit=0 已变低，bit=1 仍为高 */
    return DHT11_IN ? 1 : 0;
}

uint8_t DHT11_Read(float *temp, float *humi)
{
    uint8_t data[5] = {0};
    uint8_t i, j, bit;
    uint8_t retry;

    /* ---- 主机起始信号：拉低 >= 18ms ---- */
    dht11_pin_output();
    DHT11_OUT_LOW;
    delay_ms(20);
    DHT11_OUT_HIGH;
    delay_us(30);

    /* ---- 切换为输入，等待 DHT11 响应 ---- */
    dht11_pin_input();

    /* 读取期间禁止中断干扰时序（本工程无其他中断，保险起见仍关闭） */
    __disable_irq();

    /* 响应：80us 低 + 80us 高 */
    if (dht11_wait_level(0, 150) == 0) { __enable_irq(); return 1; }
    if (dht11_wait_level(1, 150) == 0) { __enable_irq(); return 2; }

    /* ---- 连续读 40 bit ---- */
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 8; j++) {
            bit = dht11_read_bit();
            if (bit == 0xFF) { __enable_irq(); return 3; }
            data[i] = (data[i] << 1) | bit;
        }
    }
    __enable_irq();

    /* ---- 校验：前4字节之和的低8位 == 第5字节 ---- */
    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) {
        return 4;
    }

    /* data[0]=湿度整数 data[1]=湿度小数 data[2]=温度整数 data[3]=温度小数(高位为符号) */
    *humi = data[0] + data[1] / 10.0f;
    if (data[3] & 0x80) {
        *temp = -(data[2] + (data[3] & 0x7F) / 10.0f);   /* 负温度 */
    } else {
        *temp = data[2] + data[3] / 10.0f;
    }
    (void)retry;
    return 0;
}
