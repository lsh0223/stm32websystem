#ifndef __ESP8266_H
#define __ESP8266_H
#include "sys.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// 配置参数
#define WIFI_SSID           "9636"        // 请修改为你的WIFI名称
#define WIFI_PASS           "123456789abcc"        // 请修改为你的WIFI密码
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

// 全局变量导出
extern volatile WifiState_t g_net_state;
extern volatile u8  esp8266_remote_reset_flag;
extern volatile u8  esp8266_remote_pc_on_flag;
extern volatile u8  esp8266_remote_pc_off_flag;
extern volatile u8  esp8266_remote_light_on_flag;
extern volatile u8  esp8266_remote_light_off_flag;
extern volatile u8  esp8266_remote_checkout_flag;
extern volatile u8  esp8266_remote_maint_on_flag;
extern volatile u8  esp8266_remote_maint_off_flag;
extern volatile u8  esp8266_remote_card_ok_flag;
extern volatile u8  esp8266_remote_card_err_flag;
extern volatile u8  esp8266_remote_msg_flag;

extern char esp8266_remote_msg[32];
extern char esp8266_card_err_msg[32];
extern char esp8266_remote_user_name[32];
extern char esp8266_remote_balance_str[16];
extern volatile u32 esp8266_remote_restore_sec;

// 函数接口
void ESP8266_Init_Async(void);        // 异步初始化
void ESP8266_Poll(void);              // 状态机轮询(核心，必须在主循环快跑)
void ESP8266_CheckRemoteCmd(void);    // 解析接收到的数据

// 异步发送接口，成功返回 1 (表示已入队)，失败返回 0 (忙)
u8 ESP8266_MQTT_Pub_Async(const char *topic, const char *payload);

WifiState_t ESP8266_GetState(void);

// 兼容旧代码的空函数(如果不需要可删除)
void ESP8266_DebugDump(void);

#endif
