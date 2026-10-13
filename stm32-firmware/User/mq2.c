#include "mq2.h"
#include "delay.h"

#define MQ2_SAMPLES 8

void MQ2_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    ADC_InitTypeDef  ADC_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1, ENABLE);
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);        /* ADC 时钟 72/6 = 12MHz（<=14MHz） */

    /* PA0：模拟输入 */
    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    ADC_InitStructure.ADC_Mode               = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode       = DISABLE;
    ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;   /* 单次转换 */
    ADC_InitStructure.ADC_ExternalTrigConv   = ADC_ExternalTrigConv_None;
    ADC_InitStructure.ADC_DataAlign          = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NbrOfChannel       = 1;
    ADC_Init(ADC1, &ADC_InitStructure);

    ADC_Cmd(ADC1, ENABLE);

    /* 上电校准 */
    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1));
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1));
}

static uint16_t adc1_read_ch0_once(void)
{
    ADC_RegularChannelConfig(ADC1, ADC_Channel_0, 1, ADC_SampleTime_239Cycles5);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET);
    return ADC_GetConversionValue(ADC1);
}

uint16_t MQ2_ReadRaw(void)
{
    uint16_t buf[MQ2_SAMPLES];
    uint8_t  i, j;
    uint16_t tmp;

    for (i = 0; i < MQ2_SAMPLES; i++) {
        buf[i] = adc1_read_ch0_once();
        delay_ms(2);
    }

    /* 冒泡排序后取中值 */
    for (i = 0; i < MQ2_SAMPLES - 1; i++) {
        for (j = 0; j < MQ2_SAMPLES - 1 - i; j++) {
            if (buf[j] > buf[j + 1]) {
                tmp = buf[j]; buf[j] = buf[j + 1]; buf[j + 1] = tmp;
            }
        }
    }
    return (uint16_t)((buf[MQ2_SAMPLES / 2 - 1] + buf[MQ2_SAMPLES / 2]) / 2);
}
