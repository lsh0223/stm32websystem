#ifndef __ESP8266_H
#define __ESP8266_H

#include "sys.h"
#include "usart.h"
#include "delay.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

//================= 硬件引脚定义 ==========================
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

//================= WiFi 账号密码 ========================
#define WIFI_SSID       "9636"
#define WIFI_PASS       "123456789abcc"

//================= MQTT 配置 (关键) =====================
// ★★★ 必须填云服务器公网 IP ★★★
#define MQTT_BROKER_HOST    "1.14.163.35"    
#define MQTT_BROKER_PORT    1883            
#define MQTT_LINK_ID        0
#define MQTT_CLIENT_ID      "seat001"
#define MQTT_RECONNECT      1

// WiFi 连接状态
typedef enum
{
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_ERROR
} WifiState;

// ESP8266 AT 命令返回状态
typedef enum
{
    ESP8266_OK = 0,
    ESP8266_TIMEOUT = 1,
    ESP8266_ERROR = 2
} ESP8266_Status;

typedef enum
{
    MQTT_STATE_IDLE = 0,
    MQTT_STATE_CONFIGURED,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_ERROR
} MQTTState;

#define ESP8266_RX_BUF_SIZE   512

extern volatile u16       esp8266_rx_len;
extern volatile u8        esp8266_rx_buf[ESP8266_RX_BUF_SIZE];
extern volatile WifiState g_wifi_state;
extern volatile MQTTState g_mqtt_state;

void ESP8266_Init(void);
ESP8266_Status ESP8266_ConnectAP(const char *ssid, const char *pwd);
ESP8266_Status ESP8266_MQTT_Config(void);
ESP8266_Status ESP8266_MQTT_Connect(void);
ESP8266_Status ESP8266_MQTT_Publish(const char *topic, const char *payload, u8 qos, u8 retain);
void ESP8266_MQTT_Task_1s(void);
WifiState ESP8266_GetState(void);
void ESP8266_Task_1s(void);
void ESP8266_CheckRemoteCmd(void);

//================= 远程控制标志 =================

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
extern char        esp8266_remote_msg[32];
extern char        esp8266_card_err_msg[32];
extern char        esp8266_remote_user_name[32];
extern char        esp8266_remote_balance_str[16];

extern volatile u32 esp8266_remote_restore_sec;

#endif
