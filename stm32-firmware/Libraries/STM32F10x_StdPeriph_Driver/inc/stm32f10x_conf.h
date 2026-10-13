/**
  ******************************************************************************
  * @file    stm32f10x_conf.h
  * @brief   标准外设库配置文件（本项目为非 RTE 的传统工程模式：直接包含所用模块）
  *
  *          本文件由器件包 RTE 模板改写而来：原模板依赖 RTE_Components.h
  *          自动生成宏，本工程不使用 RTE，故改为直接 include。
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __STM32F10x_CONF_H
#define __STM32F10x_CONF_H

/* 本工程当前用到的外设模块（与 User/ 下源码对应） ---------------------------- */
#include "stm32f10x_adc.h"      /* MQ-2 烟雾：ADC1 通道 0（PA0） */
#include "stm32f10x_gpio.h"     /* 全部 GPIO / 软件 I2C */
#include "stm32f10x_rcc.h"      /* 外设时钟使能 */
#include "stm32f10x_usart.h"    /* USART1：PA9/PA10，115200-8-N-1 */
#include "misc.h"               /* NVIC 与 SysTick 时钟源（delay.c 用到） */

/* 需要更多外设时：取消注释对应头文件，并把同名 .c 加入工程 StdPeriph 组 -------
#include "stm32f10x_tim.h"
#include "stm32f10x_exti.h"
#include "stm32f10x_i2c.h"
#include "stm32f10x_spi.h"
#include "stm32f10x_flash.h"
#include "stm32f10x_dma.h"
--------------------------------------------------------------------------- */

/* Exported macro ------------------------------------------------------------*/
#ifdef  USE_FULL_ASSERT
  /**
    * @brief  assert_param 宏：用于函数参数检查（调试用，默认关闭）
    */
  #define assert_param(expr) ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
  void assert_failed(uint8_t* file, uint32_t line);
#else
  #define assert_param(expr) ((void)0)
#endif /* USE_FULL_ASSERT */

#endif /* __STM32F10x_CONF_H */
