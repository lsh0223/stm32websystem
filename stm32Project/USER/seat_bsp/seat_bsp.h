#ifndef __SEAT_BSP_H
#define __SEAT_BSP_H

#include "sys.h"

void Seat_BSP_Init(void);

// 雷达：LD2410C，PC3，高电平表示有人
u8  Seat_Radar_Get(void);

// 烟雾：MQ-2，PA1，ADC1 通道 1
u16 Seat_Smoke_GetRaw(void);

// 电脑继电器：PC1，低电平触发
void Seat_PC_Set(u8 on);      // on=1 开机(LED亮)，on=0 关机

// 灯光继电器：PC2，低电平触发
void Seat_Light_Set(u8 on);   // on=1 开灯，on=0 关灯

// 有源蜂鸣器：PC0，低电平鸣叫
void Seat_Buzzer_Set(u8 on);  // on=1 响，on=0 静音

#endif
