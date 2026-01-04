#ifndef __ESP8266_H
#define __ESP8266_H
#include "sys.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// 配置参数
#define WIFI_SSID           "9636"            // 请修改为你的WIFI名称
#define WIFI_PASS           "123456789abcc"   // 请修改为你的WIFI密码
#define MQTT_BROKER_HOST    "1.14.163.35"     // 服务器IP
#define MQTT_BROKER_PORT    1883
#define MQTT_CLIENT_ID      "seat001"
#define MQTT_LINK_ID        0
#define MQTT_RECONNECT      1

// 状态机定义
typedef enum {
    WIFI_STATE_RESET = 0,   // 初始复位
    WIFI_STATE_INIT,        // 发送AT等基础配置
    WIFI_STATE_JOIN_AP,     // 连接路由
    WIFI_STATE_MQTT_CFG,    // 配置MQTT用户
    WIFI_STATE_MQTT_CONN,   // 连接MQTT服务器
    WIFI_STATE_MQTT_SUB,    // 订阅主题
    WIFI_STATE_RUNNING,     // 正常运行(心跳/发送数据)
    WIFI_STATE_ERROR        // 错误等待(稍后重试)
} WifiState_t;

// 外部标志位
extern volatile u8 esp8266_remote_reset_flag;
extern volatile u8 esp8266_remote_pc_on_flag;
extern volatile u8 esp8266_remote_pc_off_flag;
extern volatile u8 esp8266_remote_light_on_flag;
extern volatile u8 esp8266_remote_light_off_flag;
extern volatile u8 esp8266_remote_checkout_flag;
extern volatile u8 esp8266_remote_maint_on_flag;
extern volatile u8 esp8266_remote_maint_off_flag;
extern volatile u8 esp8266_remote_card_ok_flag;
extern volatile u8 esp8266_remote_card_err_flag;
extern volatile u8 esp8266_remote_msg_flag;

// ★★★ 新增：费率设置相关 ★★★
extern volatile u8 esp8266_remote_set_rate_flag;
extern volatile float esp8266_remote_rate_val;

// 数据缓冲
extern char esp8266_remote_msg[32];
extern char esp8266_card_err_msg[32];
extern char esp8266_remote_user_name[32];
extern char esp8266_remote_balance_str[16];
extern char esp8266_remote_uid[32]; 
extern volatile u32 esp8266_remote_restore_sec;

void ESP8266_Init_Async(void);
void ESP8266_Poll(void);
WifiState_t ESP8266_GetState(void);
u8 ESP8266_MQTT_Pub_Async(const char *topic, const char *payload);
void ESP8266_CheckRemoteCmd(void);

#endif

