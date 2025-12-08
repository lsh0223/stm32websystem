#ifndef __RFID_APP_H
#define __RFID_APP_H

#include "sys.h"

// 初始化 RC522 硬件
void RFID_AppInit(void);

// 检测是否有卡，读出 4 字节 UID
// 有卡返回 1，并把 uid[0..3] 写好；没卡返回 0
u8 RFID_CheckCard(u8 uid[4]);

#endif
