#ifndef __DELAY_H
#define __DELAY_H

#include "sys.h"   // 里边有 u8/u16/u32 的 typedef

// 初始化延时模块
// SYSCLK: 系统时钟频率，单位 MHz（例如 168）
void delay_init(u8 SYSCLK);

// 微秒级阻塞延时
void delay_us(u32 nus);

// 毫秒级阻塞延时
void delay_ms(u32 nms);

// 获取自上电以来经过的毫秒数（真实时间），用于软件定时
u32 GetSysMs(void);

// 供 SysTick_Handler 调用，每 1ms 调一次
void Delay_TickInc(void);

#endif
