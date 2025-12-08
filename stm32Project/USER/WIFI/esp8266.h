#ifndef __ESP8266_H
#define __ESP8266_H

#include "sys.h"
#include "usart.h"
#include "delay.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

//================= 硬件引脚定义 ==========================
// ESP-01S 接到 USART3:  PB10(TX), PB11(RX)
#define ESP8266_GPIO_CLK      RCC_AHB1Periph_GPIOB
#define ESP8266_USART_CLK     RCC_APB1Periph_USART3

#define ESP8266_TX_PORT       GPIOB
#define ESP8266_RX_PORT       GPIOB
#define ESP8266_TX_PIN        GPIO_Pin_10
#define ESP8266_RX_PIN        GPIO_Pin_11
#define ESP8266_TX_PIN_SOURCE GPIO_PinSource10
#define ESP8266_RX_PIN_SOURCE GPIO_PinSource11

#define ESP8266_USART         USART3
#define ESP8266_USART_IRQn    USART3_IRQn
//========================================================

//=== 根据实际 WiFi 修改这里 =============================
#define WIFI_SSID       "9636"
#define WIFI_PASS       "123456789abcc"

// HTTP 服务器（如果后面还用 HTTP，可以继续用）
#define SERVER_HOST     "1.14.163.35"     // 你的云服务器 IP 或域名
#define SERVER_PORT     5000              // HTTP 端口
//========================================================

//===================== MQTT 配置 =========================
// 需要在服务器上先装好 MQTT Broker（如 emqx），端口一般 1883
#define MQTT_LINK_ID        0
#define MQTT_BROKER_HOST    "1.14.163.35"    // 与 HTTP 同一台服务器
#define MQTT_BROKER_PORT    1883            // EMQX 默认 1883

// client_id（后期可以按座位号改：seat001, seat002...）
#define MQTT_CLIENT_ID      "seat001"

// 先不在 EMQX 开认证，这里只是占位，真正用的是 MQTTUSERCFG 里固定的那条指令
#define MQTT_USERNAME       ""
#define MQTT_PASSWORD       ""

// 是否让模块自动重连：0 不自动重连，1 自动重连
#define MQTT_RECONNECT      1
//========================================================

// WiFi 连接状态
typedef enum
{
    WIFI_STATE_IDLE = 0,        // 未初始化
    WIFI_STATE_DISCONNECTED,    // 已初始化但未连接
    WIFI_STATE_CONNECTING,      // 正在连接 AP
    WIFI_STATE_CONNECTED,       // 已连接
    WIFI_STATE_ERROR            // 出错
} WifiState;

// ESP8266 AT 命令返回状态
typedef enum
{
    ESP8266_OK = 0,
    ESP8266_TIMEOUT = 1,
    ESP8266_ERROR = 2
} ESP8266_Status;

// MQTT 状态（简单标记一下）
typedef enum
{
    MQTT_STATE_IDLE = 0,
    MQTT_STATE_CONFIGURED,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_ERROR
} MQTTState;

// 接收缓冲区
#define ESP8266_RX_BUF_SIZE   512

extern volatile u16       esp8266_rx_len;
extern volatile u8        esp8266_rx_buf[ESP8266_RX_BUF_SIZE];
extern volatile WifiState g_wifi_state;
extern volatile MQTTState g_mqtt_state;

//================= 基本 AT & WiFi =======================

// 初始化：串口 + ESP 基本设置
void ESP8266_Init(void);

// 连接路由器
ESP8266_Status ESP8266_ConnectAP(const char *ssid, const char *pwd);

// 发送 AT 命令并等待关键字符串
ESP8266_Status ESP8266_SendCmdWait(const char *cmd, const char *expect, u32 timeout_ms);

//================= HTTP（可选） ========================
// 发送 HTTP POST
ESP8266_Status ESP8266_HttpPost(
    const char *host,
    u16 port,
    const char *path,
    const char *body);

//================= MQTT 接口 ===========================

// 配置 MQTT 用户信息（AT+MQTTUSERCFG）
ESP8266_Status ESP8266_MQTT_Config(void);

// 连接 MQTT 服务器（AT+MQTTCONN）
ESP8266_Status ESP8266_MQTT_Connect(void);

// 发布一条 MQTT 消息（payload 里不要放双引号）
ESP8266_Status ESP8266_MQTT_Publish(const char *topic,
                                    const char *payload,
                                    u8 qos,
                                    u8 retain);

// 每秒调用一次，自动维持 MQTT 连接
void ESP8266_MQTT_Task_1s(void);

//================= 状态 / 调试 =========================

WifiState ESP8266_GetState(void);

// 每 1 秒调用一次：负责 WiFi 掉线重连 + MQTT 维护
void ESP8266_Task_1s(void);


// 把 ESP 收到的数据打印到串口1
void ESP8266_DebugDump(void);

// ★ 检查串口缓冲区里是否出现过 "reset" 字符串，出现就复位 MCU
void ESP8266_CheckRemoteReset(void);
void ESP8266_ProcessDownlink(void);

// 处理远程复位命令：在缓冲区中找到 "reset" 时置位标志
void ESP8266_CheckRemoteReset(void);





//================= 远程控制标志（由 esp8266.c 置位，main.c 读取） =================

// 来自服务器的远程复位标志（软复位），由主循环读取并执行软复位
extern volatile u8 esp8266_remote_reset_flag;

// PC / 灯 / 下机 控制标志
extern volatile u8 esp8266_remote_pc_on_flag;
extern volatile u8 esp8266_remote_pc_off_flag;
extern volatile u8 esp8266_remote_light_on_flag;
extern volatile u8 esp8266_remote_light_off_flag;
extern volatile u8 esp8266_remote_checkout_flag;

// 维护模式开关
extern volatile u8 esp8266_remote_maint_on_flag;
extern volatile u8 esp8266_remote_maint_off_flag;

// 刷卡验证反馈
extern volatile u8 esp8266_remote_card_ok_flag;
extern volatile u8 esp8266_remote_card_err_flag;

// 短消息 & 刷卡错误消息
extern volatile u8 esp8266_remote_msg_flag;
extern char        esp8266_remote_msg[32];
extern char        esp8266_card_err_msg[32];

// 每秒调用一次：检查下行 MQTT 指令（reset/pc_on/.../msg:xxx 等）
void ESP8266_CheckRemoteCmd(void);



#endif
