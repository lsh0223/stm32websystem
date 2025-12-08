#include "rc522.h"
#include "delay.h"

// 软件SPI写字节
void RC522_SPI_WriteByte(u8 byte)
{
    u8 i;
    for(i=0; i<8; i++)
    {
        RC522_SCK_L;
        if(byte & 0x80) RC522_MOSI_H;
        else RC522_MOSI_L;
        delay_us(1); // F407速度快，稍微延时
        RC522_SCK_H;
        byte <<= 1;
        delay_us(1);
    }
}

// 软件SPI读字节
u8 RC522_SPI_ReadByte(void)
{
    u8 i, byte=0;
    for(i=0; i<8; i++)
    {
        byte <<= 1;
        RC522_SCK_L;
        delay_us(1);
        RC522_SCK_H;
        if(RC522_MISO_READ) byte |= 0x01;
        delay_us(1);
    }
    return byte;
}

// 写寄存器
void Write_Raw_RC(unsigned char Address, unsigned char value)
{
    RC522_CS_L;
    RC522_SPI_WriteByte((Address << 1) & 0x7E);
    RC522_SPI_WriteByte(value);
    RC522_CS_H;
}

// 读寄存器
unsigned char Read_Raw_RC(unsigned char Address)
{
    unsigned char ucReturn;
    RC522_CS_L;
    RC522_SPI_WriteByte(((Address << 1) & 0x7E) | 0x80);
    ucReturn = RC522_SPI_ReadByte();
    RC522_CS_H;
    return ucReturn;
}

// 置位
void SetBitMask(unsigned char reg, unsigned char mask)
{
    char tmp = 0x0;
    tmp = Read_Raw_RC(reg);
    Write_Raw_RC(reg, tmp | mask);
}

// 清位
void ClearBitMask(unsigned char reg, unsigned char mask)
{
    char tmp = 0x0;
    tmp = Read_Raw_RC(reg);
    Write_Raw_RC(reg, tmp & (~mask));
}

// 开启天线
void PcdAntennaOn(void)
{
    unsigned char i;
    i = Read_Raw_RC(0x14);
    if (!(i & 0x03))
    {
        SetBitMask(0x14, 0x03);
    }
}

// 关闭天线
void PcdAntennaOff(void)
{
    ClearBitMask(0x14, 0x03);
}

// 复位 RC522
void PcdReset(void)
{
    RC522_RST_H;
    delay_us(1);
    RC522_RST_L;
    delay_us(1);
    RC522_RST_H;
    delay_us(1);
    Write_Raw_RC(0x01, 0x0F);
    while(Read_Raw_RC(0x01) & 0x10); // 等待复位完成
    delay_us(1);
}

// 初始化函数 (适配 STM32F407)
void RC522_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    // 1. 开启 GPIOA 和 GPIOB 时钟 (F407用AHB1)
    RCC_AHB1PeriphClockCmd(RC522_RCC_A | RC522_RCC_B, ENABLE);

    // 2. 配置 GPIOA 输出引脚 (CS, SCK, MOSI)
    GPIO_InitStructure.GPIO_Pin = PIN_SDA | PIN_SCK | PIN_MOSI;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(RC522_GPIO_A, &GPIO_InitStructure);

    // 3. 配置 GPIOB 输出引脚 (RST)
    GPIO_InitStructure.GPIO_Pin = PIN_RST;
    GPIO_Init(RC522_GPIO_B, &GPIO_InitStructure);

    // 4. 配置 GPIOA 输入引脚 (MISO)
    GPIO_InitStructure.GPIO_Pin = PIN_MISO;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(RC522_GPIO_A, &GPIO_InitStructure);

    RC522_CS_H;
    RC522_RST_H; // 释放复位
    
    // 5. 寄存器初始化配置
    PcdReset();
    
    Write_Raw_RC(0x2A, 0x8D); // TModeReg
    Write_Raw_RC(0x2B, 0x3E); // TPrescalerReg
    Write_Raw_RC(0x2D, 30);   // TReloadRegL
    Write_Raw_RC(0x2C, 0);    // TReloadRegH
    Write_Raw_RC(0x15, 0x40); // TxAutoReg
    Write_Raw_RC(0x11, 0x3D); // ModeReg
    
    // 【关键】设置天线增益 (48dB)，提高灵敏度
    SetBitMask(0x26, 0x70); 
    
    PcdAntennaOn(); // 开启天线
}

// 通信核心函数
char PcdComMF522(unsigned char Command, unsigned char *pDataIn, unsigned char InLenByte, unsigned char *pDataOut, unsigned int *pOutLenBit)
{
    char status = MI_ERR;
    unsigned char irqEn = 0x00;
    unsigned char waitFor = 0x00;
    unsigned char lastBits;
    unsigned char n;
    unsigned int i;
    switch (Command)
    {
    case PCD_AUTHENT:
        irqEn = 0x12;
        waitFor = 0x10;
        break;
    case PCD_TRANSCEIVE:
        irqEn = 0x77;
        waitFor = 0x30;
        break;
    default:
        break;
    }
    Write_Raw_RC(0x02, irqEn | 0x80);
    ClearBitMask(0x04, 0x80);
    SetBitMask(0x0A, 0x80);
    Write_Raw_RC(0x01, 0x00);
    for (i = 0; i < InLenByte; i++)
    {
        Write_Raw_RC(0x09, pDataIn[i]);
    }
    Write_Raw_RC(0x01, Command);
    if (Command == PCD_TRANSCEIVE)
    {
        SetBitMask(0x0D, 0x80);
    }
    i = 6000;
    do
    {
        n = Read_Raw_RC(0x04);
        i--;
    } while ((i != 0) && !(n & 0x01) && !(n & waitFor));
    ClearBitMask(0x0D, 0x80);
    if (i != 0)
    {
        if (!(Read_Raw_RC(0x06) & 0x1B))
        {
            status = MI_OK;
            if (n & irqEn & 0x01)
            {
                status = MI_NOTAGERR;
            }
            if (Command == PCD_TRANSCEIVE)
            {
                n = Read_Raw_RC(0x0A);
                lastBits = Read_Raw_RC(0x0C) & 0x07;
                if (lastBits)
                {
                    *pOutLenBit = (n - 1) * 8 + lastBits;
                }
                else
                {
                    *pOutLenBit = n * 8;
                }
                if (n == 0)
                {
                    n = 1;
                }
                if (n > 18)
                {
                    n = 18;
                }
                for (i = 0; i < n; i++)
                {
                    pDataOut[i] = Read_Raw_RC(0x09);
                }
            }
        }
        else
        {
            status = MI_ERR;
        }
    }
    return status;
}

// 寻卡
char PcdRequest(unsigned char req_code, unsigned char *pTagType)
{
    char status;
    unsigned int unLen;
    unsigned char ucComMF522Buf[16];

    ClearBitMask(0x0C, 0x80);
    // 【关键修正】发送位宽必须是7位
    Write_Raw_RC(0x0D, 0x07); 
    ClearBitMask(0x0E, 0x08);
    Write_Raw_RC(0x01, 0x00);
    ucComMF522Buf[0] = req_code;
    status = PcdComMF522(PCD_TRANSCEIVE, ucComMF522Buf, 1, ucComMF522Buf, &unLen);
    if ((status == MI_OK) && (unLen == 0x10))
    {
        *pTagType = ucComMF522Buf[0];
        *(pTagType + 1) = ucComMF522Buf[1];
    }
    else
    {
        status = MI_ERR;
    }
    return status;
}

// 防冲突
char PcdAnticoll(unsigned char *pSnr)
{
    char status;
    unsigned char i, snr_check = 0;
    unsigned int unLen;
    unsigned char ucComMF522Buf[16];

    ClearBitMask(0x0C, 0x80);
    // 【关键修正】发送位宽必须是0，清除对齐
    Write_Raw_RC(0x0D, 0x00); 
    ClearBitMask(0x0E, 0x08);
    ucComMF522Buf[0] = 0x93;
    ucComMF522Buf[1] = 0x20;
    status = PcdComMF522(PCD_TRANSCEIVE, ucComMF522Buf, 2, ucComMF522Buf, &unLen);
    if (status == MI_OK)
    {
        for (i = 0; i < 4; i++)
        {
            *(pSnr + i) = ucComMF522Buf[i];
            snr_check ^= ucComMF522Buf[i];
        }
        if (snr_check != ucComMF522Buf[i])
        {
            status = MI_ERR;
        }
    }
    SetBitMask(0x0C, 0x80);
    return status;
}
