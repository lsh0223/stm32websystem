#ifndef __RC522_H
#define __RC522_H
#include "sys.h"  // 引入sys.h以便使用u8, u16等类型

// ==========================================================
//                    RC522 引脚定义 (适配你的接线)
// ==========================================================
// RST  -> PB12
// MISO -> PA6
// MOSI -> PA7
// SCK  -> PA5
// SDA  -> PA15 (CS)
// ==========================================================

// 1. 端口和引脚定义
#define RC522_GPIO_A    GPIOA
#define RC522_GPIO_B    GPIOB
#define RC522_RCC_A     RCC_AHB1Periph_GPIOA
#define RC522_RCC_B     RCC_AHB1Periph_GPIOB

#define PIN_SDA         GPIO_Pin_15  // PA15
#define PIN_SCK         GPIO_Pin_5   // PA5
#define PIN_MOSI        GPIO_Pin_7   // PA7
#define PIN_MISO        GPIO_Pin_6   // PA6
#define PIN_RST         GPIO_Pin_12  // PB12

// 2. 电平控制宏
#define RC522_CS_H      GPIO_SetBits(RC522_GPIO_A, PIN_SDA)
#define RC522_CS_L      GPIO_ResetBits(RC522_GPIO_A, PIN_SDA)

#define RC522_SCK_H     GPIO_SetBits(RC522_GPIO_A, PIN_SCK)
#define RC522_SCK_L     GPIO_ResetBits(RC522_GPIO_A, PIN_SCK)

#define RC522_MOSI_H    GPIO_SetBits(RC522_GPIO_A, PIN_MOSI)
#define RC522_MOSI_L    GPIO_ResetBits(RC522_GPIO_A, PIN_MOSI)

#define RC522_RST_H     GPIO_SetBits(RC522_GPIO_B, PIN_RST)
#define RC522_RST_L     GPIO_ResetBits(RC522_GPIO_B, PIN_RST)

#define RC522_MISO_READ GPIO_ReadInputDataBit(RC522_GPIO_A, PIN_MISO)

// ==========================================================
//                    MFRC522 命令字 & 寄存器
// ==========================================================
#define PCD_IDLE              0x00               // 取消当前命令
#define PCD_AUTHENT           0x0E               // 验证密钥
#define PCD_RECEIVE           0x08               // 接收数据
#define PCD_TRANSMIT          0x04               // 发送数据
#define PCD_TRANSCEIVE        0x0C               // 发送并接收数据
#define PCD_RESETPHASE        0x0F               // 复位
#define PCD_CALCCRC           0x03               // CRC计算

#define PICC_REQIDL           0x26               // 寻天线区内未进入休眠状态
#define PICC_REQALL           0x52               // 寻所有状态卡
#define PICC_ANTICOLL1        0x93               // 防冲撞
#define PICC_ANTICOLL2        0x95               // 防冲撞
#define PICC_AUTHENT1A        0x60               // 验证A密钥
#define PICC_AUTHENT1B        0x61               // 验证B密钥
#define PICC_READ             0x30               // 读块
#define PICC_WRITE            0xA0               // 写块
#define PICC_DECREMENT        0xC0               // 扣款
#define PICC_INCREMENT        0xC1               // 充值
#define PICC_RESTORE          0xC2               // 调块数据到缓冲区
#define PICC_TRANSFER         0xB0               // 保存缓冲区中数据
#define PICC_HALT             0x50               // 休眠

// 状态返回码
#define MI_OK                 0
#define MI_NOTAGERR           1
#define MI_ERR                2

// 函数声明
void RC522_Init(void);
char PcdRequest(unsigned char req_code, unsigned char *pTagType);
char PcdAnticoll(unsigned char *pSnr);
char PcdSelect(unsigned char *pSnr);
char PcdAuthState(unsigned char auth_mode, unsigned char addr, unsigned char *pKey, unsigned char *pSnr);
char PcdRead(unsigned char addr, unsigned char *pData);
char PcdWrite(unsigned char addr, unsigned char *pData);
void PcdAntennaOn(void);
void PcdAntennaOff(void);
unsigned char Read_Raw_RC(unsigned char Address); // 暴露给main.c做自检

#endif
