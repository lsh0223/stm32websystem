#include "rfid_app.h"
#include "rc522.h"
#include "delay.h"
#include "usart.h"

// 初始化 RC522
void RFID_AppInit(void)
{
    RC522_Init();     // 初始化寄存器 & GPIO
    PcdAntennaOn();   // 打开发射天线
}

// 简单读卡：有卡就返回 UID，不做“同卡过滤”
u8 RFID_CheckCard(u8 uid[4])
{
    u8 status;
    u8 tag_type[2];

    // 1. 搜索卡片（寻卡）
    status = PcdRequest(PICC_REQIDL, tag_type);
    if (status != MI_OK)
    {
        return 0;   // 没有卡片
    }

    // 2. 防冲突，读出 4 字节卡号 UID
    status = PcdAnticoll(uid);
    if (status != MI_OK)
    {
        return 0;
    }

    // 这里 **不再调用 PcdSelect**，避免链接到不存在的函数
    // 如果后面你需要更严谨的流程，再考虑在 rc522.c 里自己实现 PcdSelect

    // 打一行调试信息
    printf("CARD UID: %02X%02X%02X%02X\r\n",
           uid[0], uid[1], uid[2], uid[3]);

    return 1;   // 读卡成功
}
