#include "delay.h"
#include "stm32f4xx.h"

// 毫秒计数（SysTick 中断每 1ms 调一次 Delay_TickInc）
static volatile u32 s_ms = 0;

// DWT 周期计数用的因子：每 1us 需要多少个 CPU 时钟周期
static u32 fac_us = 0;

/**
 * @brief  延时模块初始化
 * @param  SYSCLK: 系统时钟频率，单位 MHz（如 168）
 * @note   1) 配置 SysTick 产生 1ms 中断，用于 GetSysMs
 *         2) 打开 DWT 周期计数器，用于 delay_us
 */
void delay_init(u8 SYSCLK)
{
    /* ---------- 1. 打开 DWT 周期计数器，用于 us 延时 ---------- */
    // 使能 DWT 外设
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    // 复位周期计数器
    DWT->CYCCNT = 0;
    // 使能周期计数
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    // CPU 时钟 = SYSCLK MHz -> 每 1us 有 SYSCLK 个时钟周期
    fac_us = SYSCLK;

    /* ---------- 2. 配置 SysTick，每 1ms 触发一次中断 ---------- */
    // 先关闭 SysTick
    SysTick->CTRL = 0;

    // 选择时钟源为 HCLK（CPU 主频）
    SysTick->CTRL |= SysTick_CTRL_CLKSOURCE_Msk; // CLKSOURCE = HCLK

    // 计算重装载值：
    // HCLK = SYSCLK MHz -> 每秒 SYSCLK * 1e6 个周期
    // 1ms 需要 SYSCLK * 1e6 / 1000 = SYSCLK * 1000 个周期
    SysTick->LOAD = (u32)SYSCLK * 1000 - 1;
    SysTick->VAL  = 0;  // 清空当前计数

    // 使能中断 + 启动 SysTick
    SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

/**
 * @brief  每 1ms 被 SysTick_Handler 调用一次
 */
void Delay_TickInc(void)
{
    s_ms++;
}

/**
 * @brief  获取自上电以来的毫秒数
 */
u32 GetSysMs(void)
{
    return s_ms;
}

/**
 * @brief  微秒级延时（使用 DWT 周期计数器）
 * @param  nus: 微秒数
 * @note   最大延时大约几十秒，完全够用了
 */
void delay_us(u32 nus)
{
    u32 start = DWT->CYCCNT;
    u32 ticks = nus * fac_us;   // 需要的时钟周期数

    // 这里利用无符号减法溢出特性，安全地等待到达目标周期数
    while ((DWT->CYCCNT - start) < ticks)
    {
        // busy wait
    }
}

/**
 * @brief  毫秒级延时（基于 GetSysMs）
 * @param  nms: 毫秒数
 */
void delay_ms(u32 nms)
{
    u32 start = GetSysMs();
    while ((GetSysMs() - start) < nms)
    {
        // busy wait
    }
}
