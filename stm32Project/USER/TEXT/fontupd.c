#include "sys.h"
#include "fontupd.h"
#include "w25qxx.h"
#include "delay.h"

// 字库信息存在外部 Flash 的地址（12M 以后）
// 对应原例程里的 FONTINFOADDR
u32 FONTINFOADDR = 1024 * 1024 * 12;

// 字库信息结构体实例，在 text.c 里会用到
_font_info ftinfo;

/**
 * @brief  初始化字库信息
 * @retval 0: 字库正常
 *         1: 字库丢失或数据错误
 */
u8 font_init(void)
{
    u8 t = 0;

    W25QXX_Init();  // 初始化外部 Flash

    // 连续读取多次，确保读取稳定
    while (t < 10)
    {
        t++;
        // 从外部 Flash 读取 ftinfo 结构体
        W25QXX_Read((u8 *)&ftinfo, FONTINFOADDR, sizeof(ftinfo));
        if (ftinfo.fontok == 0xAA) break;
        delay_ms(20);
    }

    if (ftinfo.fontok != 0xAA) return 1;  // 字库信息无效，需要重新烧录字库
    return 0;                             // 正常
}
