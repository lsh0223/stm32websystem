#include "seat_bsp.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_adc.h"

void Seat_BSP_Init(void)
{
    GPIO_InitTypeDef        gpio;
    ADC_CommonInitTypeDef   adc_common;
    ADC_InitTypeDef         adc_init;

    // 打开 GPIOC / GPIOA 时钟
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOA, ENABLE);
    // 打开 ADC1 时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    // PC0(蜂鸣器)、PC1(电脑继电器)、PC2(灯继电器)  配置为推挽输出
    gpio.GPIO_Pin   = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2;
    gpio.GPIO_Mode  = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);

    // 默认全部关闭：这些都是低电平触发，所以拉高关闭
    GPIO_SetBits(GPIOC, GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2);

    // PC3 (雷达输入，高电平=有人)
    gpio.GPIO_Pin   = GPIO_Pin_3;
    gpio.GPIO_Mode  = GPIO_Mode_IN;
    gpio.GPIO_PuPd  = GPIO_PuPd_DOWN;  // 无人时保持低
    GPIO_Init(GPIOC, &gpio);

    // PA1 作为模拟输入（MQ-2 烟雾）
    gpio.GPIO_Pin   = GPIO_Pin_1;
    gpio.GPIO_Mode  = GPIO_Mode_AN;
    gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOA, &gpio);

    // ADC 公共配置
    adc_common.ADC_Mode             = ADC_Mode_Independent;
    adc_common.ADC_Prescaler        = ADC_Prescaler_Div4;
    adc_common.ADC_DMAAccessMode    = ADC_DMAAccessMode_Disabled;
    adc_common.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
    ADC_CommonInit(&adc_common);

    // ADC1 配置：单通道、软件触发
    adc_init.ADC_Resolution          = ADC_Resolution_12b;
    adc_init.ADC_ScanConvMode        = DISABLE;
    adc_init.ADC_ContinuousConvMode  = DISABLE;
    adc_init.ADC_ExternalTrigConvEdge= ADC_ExternalTrigConvEdge_None;
    adc_init.ADC_ExternalTrigConv    = ADC_ExternalTrigConv_T1_CC1;
    adc_init.ADC_DataAlign           = ADC_DataAlign_Right;
    adc_init.ADC_NbrOfConversion     = 1;
    ADC_Init(ADC1, &adc_init);

    ADC_Cmd(ADC1, ENABLE);
}

u8 Seat_Radar_Get(void)
{
    // PC3 高电平表示有人
    return GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_3) ? 1 : 0;
}

u16 Seat_Smoke_GetRaw(void)
{
    // ADC1 通道1 = PA1
    ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 1, ADC_SampleTime_144Cycles);
    ADC_SoftwareStartConv(ADC1);
    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET);
    return ADC_GetConversionValue(ADC1);  // 0~4095
}

void Seat_PC_Set(u8 on)
{
    if (on)
    {
        // 低电平触发
        GPIO_ResetBits(GPIOC, GPIO_Pin_1);
    }
    else
    {
        GPIO_SetBits(GPIOC, GPIO_Pin_1);
    }
}

void Seat_Light_Set(u8 on)
{
    if (on)
    {
        GPIO_ResetBits(GPIOC, GPIO_Pin_2);
    }
    else
    {
        GPIO_SetBits(GPIOC, GPIO_Pin_2);
    }
}

void Seat_Buzzer_Set(u8 on)
{
    if (on)
    {
        GPIO_ResetBits(GPIOC, GPIO_Pin_0);
    }
    else
    {
        GPIO_SetBits(GPIOC, GPIO_Pin_0);
    }
}
