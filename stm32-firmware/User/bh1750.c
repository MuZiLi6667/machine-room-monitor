#include "bh1750.h"
#include "delay.h"

#define BH1750_ADDR      0x23    /* ADDR 接地 */
#define BH1750_CMD_ON    0x01    /* Power On */
#define BH1750_CMD_CHRM  0x10    /* 连续高分辨率模式 1lx，典型转换 120ms（规格书最大 180ms） */
/* 注：不使用 0x07（Reset）——它会清空数据寄存器导致复位后首帧读到 0；
 *     上电 + 进入连续模式即可，连续模式下重复发 CHRM 仅保持模式、不打断测量 */

#define SCL_PORT   GPIOB
#define SCL_PIN    GPIO_Pin_6
#define SDA_PORT   GPIOB
#define SDA_PIN    GPIO_Pin_7

#define SCL_H  GPIO_SetBits(SCL_PORT, SCL_PIN)
#define SCL_L  GPIO_ResetBits(SCL_PORT, SCL_PIN)
#define SDA_H  GPIO_SetBits(SDA_PORT, SDA_PIN)
#define SDA_L  GPIO_ResetBits(SDA_PORT, SDA_PIN)
#define SDA_READ  GPIO_ReadInputDataBit(SDA_PORT, SDA_PIN)

/* I2C 时序参数：实测本模块对过快时序不应答——从机 ACK 建立较慢，
 * 且主机释放 SDA 后需留出上拉恢复时间（3~5us 采样会把"未恢复/未建立"
 * 误判为取反的 ACK 结果，导致写指令假成功、读地址假失败）。
 * 统一按 ~25kHz 安全速率操作（经诊断固件逐项实测验证可稳定读通）。 */
#define T_HALF   20   /* SCL 半周期（us） */
#define T_SET    10   /* SDA 建立/保持（us） */

static void bh1750_write_cmd(uint8_t cmd);   /* 前置声明：BH1750_Init 中先于定义使用 */

/* PB6/PB7 均为开漏输出（输出1时释放总线，由模块/外部上拉拉高） */
void BH1750_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = SCL_PIN | SDA_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    SCL_H; SDA_H;
    delay_ms(50);       /* 等模块上电稳定（BH1750 上电后需数毫秒才能接收命令） */

    /* Power On + 进入连续高分辨率模式（ReadLux 中会重复发送以保持状态）。
     * 关键：0x01 与 0x10 之间必须留 ≥100ms 间隔 —— Power On 后芯片
     * 振荡器稳定需要时间，间隔过短（100us 级）会导致 CHRM 不生效、
     * 芯片不进测量模式（表现为读回 0xFFFF）。 */
    bh1750_write_cmd(BH1750_CMD_ON);
    delay_ms(100);
    bh1750_write_cmd(BH1750_CMD_CHRM);
    delay_ms(250);      /* 等首次转换完成 */

    /* 热身读两次并丢弃：实测上电/复位后前两个读事务返回无效值，
     * 丢弃后主循环首帧即稳定 */
    BH1750_ReadLux();
    BH1750_ReadLux();
}

/* ---------------- 软件 I2C 基础时序（约 25kHz） ---------------- */

static void i2c_start(void)
{
    SDA_H; SCL_H; delay_us(T_HALF);
    SDA_L; delay_us(T_HALF);
    SCL_L; delay_us(T_HALF);
}

static void i2c_stop(void)
{
    SDA_L; delay_us(T_HALF);
    SCL_H; delay_us(T_HALF);
    SDA_H; delay_us(T_HALF);
}

/* 返回 0=收到 ACK，1=无 ACK */
static uint8_t i2c_write_byte(uint8_t byte)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        if (byte & 0x80) SDA_H; else SDA_L;
        delay_us(T_SET);
        SCL_H; delay_us(T_HALF);
        SCL_L; delay_us(T_SET);
        byte <<= 1;
    }
    SDA_H;                      /* 释放 SDA，先留出上拉恢复时间 */
    delay_us(T_SET);
    SCL_H; delay_us(T_HALF);    /* 等从机 ACK 建立后再采样 */
    i = SDA_READ ? 1 : 0;
    SCL_L; delay_us(T_SET);
    return i;
}

static uint8_t i2c_read_byte(uint8_t ack)   /* ack=1 主机应答，0=不应答（最后一字节） */
{
    uint8_t i, byte = 0;
    SDA_H;              /* 释放总线，交给从机驱动 */
    for (i = 0; i < 8; i++) {
        byte <<= 1;
        SCL_H; delay_us(T_HALF);
        if (SDA_READ) byte |= 0x01;
        SCL_L; delay_us(T_HALF);
    }
    if (ack) SDA_L; else SDA_H;   /* 主机 ACK / NACK */
    delay_us(T_SET);
    SCL_H; delay_us(T_HALF);
    SCL_L; delay_us(T_SET);
    SDA_H;
    return byte;
}

static void bh1750_write_cmd(uint8_t cmd)
{
    i2c_start();
    i2c_write_byte(BH1750_ADDR << 1);   /* 写 */
    i2c_write_byte(cmd);
    i2c_stop();
}

uint16_t BH1750_ReadLux(void)
{
    uint8_t hi, lo;
    uint16_t raw;

    /* 完整重发 Power On + CHRM（与诊断固件逐项验证通过的序列一致：
     * 0x01 → 100ms → 0x10 → 250ms → 读） */
    bh1750_write_cmd(BH1750_CMD_ON);
    delay_ms(100);
    bh1750_write_cmd(BH1750_CMD_CHRM);
    delay_ms(250);          /* 等转换完成（规格书最大 180ms，留余量） */

    /* 连读两次、取第二次的值：诊断固件实测本模块"单次读事务"会得到
     * 无效数据（0x1A80~0x7E00 乱跳），"连续读两次"则两次均为正确值
     * （0x0080 稳定）——疑为该模块读事务后内部状态需一次同步 */
    i2c_start();
    i2c_write_byte((BH1750_ADDR << 1) | 0x01);
    i2c_read_byte(1);
    i2c_read_byte(0);
    i2c_stop();

    i2c_start();
    i2c_write_byte((BH1750_ADDR << 1) | 0x01);   /* 正式读取 */
    hi = i2c_read_byte(1);
    lo = i2c_read_byte(0);
    i2c_stop();

    raw = ((uint16_t)hi << 8) | lo;
    /* lux = raw / 1.2，用整数运算：raw*10/12 */
    return (uint16_t)((uint32_t)raw * 10 / 12);
}
