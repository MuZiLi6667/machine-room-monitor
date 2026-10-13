#include "delay.h"

static uint8_t  fac_us = 0;   /* 每微秒计数值，72MHz 时为 72 */

void delay_init(void)
{
    /* SysTick 时钟源选择 HCLK（72MHz，不分频） */
    SysTick_CLKSourceConfig(SysTick_CLKSource_HCLK);
    fac_us = SystemCoreClock / 1000000;
    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;   /* 先关闭 */
}

void delay_us(uint32_t nus)
{
    uint32_t temp;
    SysTick->LOAD = nus * fac_us;            /* 加载计数值（24位，nus*72 < 0xFFFFFF） */
    SysTick->VAL  = 0x00;                    /* 清空当前值 */
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
    do {
        temp = SysTick->CTRL;
    } while ((temp & SysTick_CTRL_ENABLE_Msk) && !(temp & SysTick_CTRL_COUNTFLAG_Msk));
    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
    SysTick->VAL = 0x00;
}

void delay_ms(uint16_t nms)
{
    while (nms--) {
        delay_us(1000);
    }
}
