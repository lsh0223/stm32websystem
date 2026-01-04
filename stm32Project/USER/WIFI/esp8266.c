#include "esp8266.h"
#include "delay.h"

#define ESP_RX_BUF_SIZE 512
volatile u16 esp8266_rx_len = 0;
volatile u8  esp8266_rx_buf[ESP_RX_BUF_SIZE];

volatile u8 esp8266_remote_reset_flag      = 0;
volatile u8 esp8266_remote_pc_on_flag      = 0;
volatile u8 esp8266_remote_pc_off_flag     = 0;
volatile u8 esp8266_remote_light_on_flag   = 0;
volatile u8 esp8266_remote_light_off_flag  = 0;
volatile u8 esp8266_remote_checkout_flag   = 0;
volatile u8 esp8266_remote_maint_on_flag   = 0;
volatile u8 esp8266_remote_maint_off_flag  = 0;
volatile u8 esp8266_remote_card_ok_flag    = 0;
volatile u8 esp8266_remote_card_err_flag   = 0;
volatile u8 esp8266_remote_msg_flag        = 0;

char esp8266_remote_msg[32];
char esp8266_card_err_msg[32];
char esp8266_remote_user_name[32];
char esp8266_remote_balance_str[16];
// ★★★ 新增：定义 UID 变量内存，解决 Linker Error ★★★
char esp8266_remote_uid[32]; 
volatile u32 esp8266_remote_restore_sec    = 0;

volatile WifiState_t g_net_state = WIFI_STATE_RESET;
static u32 g_state_tick = 0;   

typedef enum { AT_IDLE, AT_WAIT_RESP } AtState_t;
static AtState_t g_at_state = AT_IDLE;
static char g_at_expect[32];   
static u32  g_at_timeout = 0;  
static u32  g_at_start_ms = 0; 

static char g_pub_topic[64];
static char g_pub_payload[128];
static u8   g_pub_pending = 0; 

static void ESP8266_USART_Config(void) {
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;
    
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE); 
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource10, GPIO_AF_USART3);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource11, GPIO_AF_USART3);
    
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10 | GPIO_Pin_11;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    
    USART_InitStructure.USART_BaudRate = 115200;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART3, &USART_InitStructure);
    
    USART_ITConfig(USART3, USART_IT_RXNE, ENABLE);
    
    NVIC_InitStructure.NVIC_IRQChannel = USART3_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    
    USART_Cmd(USART3, ENABLE);
}

void USART3_IRQHandler(void) {
    if (USART_GetITStatus(USART3, USART_IT_RXNE) != RESET) {
        u8 ch = USART_ReceiveData(USART3);
        if (esp8266_rx_len < ESP_RX_BUF_SIZE - 1) {
            esp8266_rx_buf[esp8266_rx_len++] = ch;
            esp8266_rx_buf[esp8266_rx_len] = 0;
        }
    }
}

static void ESP8266_SendString(const char *s) {
    while (*s) {
        while (USART_GetFlagStatus(USART3, USART_FLAG_TXE) == RESET);
        USART_SendData(USART3, (u8)*s++);
    }
}

static void ESP8266_ClearBuf(void) {
    esp8266_rx_len = 0;
    esp8266_rx_buf[0] = 0;
}

static void AT_Send_Async(const char *cmd, const char *expect, u32 timeout) {
    ESP8266_ClearBuf();
    if (cmd && cmd[0]) ESP8266_SendString(cmd);
    strncpy(g_at_expect, expect, 31);
    g_at_timeout = timeout;
    g_at_start_ms = GetSysMs();
    g_at_state = AT_WAIT_RESP;
}

static int AT_Check_Status(void) {
    if (g_at_state == AT_IDLE) return 1; 
    
    if (esp8266_rx_len > 0) {
        if (strstr((const char *)esp8266_rx_buf, g_at_expect) != NULL) {
            g_at_state = AT_IDLE;
            return 1; 
        }
        if (strstr((const char *)esp8266_rx_buf, "ERROR") != NULL || strstr((const char *)esp8266_rx_buf, "FAIL") != NULL) {
            g_at_state = AT_IDLE;
            return -1; 
        }
    }
    
    if (GetSysMs() - g_at_start_ms > g_at_timeout) {
        g_at_state = AT_IDLE;
        return -1; 
    }
    
    return 0; 
}

// --- 公共接口 ---

void ESP8266_Init_Async(void) {
    ESP8266_USART_Config();
    g_net_state = WIFI_STATE_RESET;
    g_state_tick = GetSysMs();
    
    g_pub_pending = 0; 
    ESP8266_ClearBuf();
}

WifiState_t ESP8266_GetState(void) {
    return g_net_state;
}

u8 ESP8266_MQTT_Pub_Async(const char *topic, const char *payload) {
    if (g_net_state != WIFI_STATE_RUNNING) return 0;
    
    if (g_pub_pending) {
        if (strstr(topic, "state") != NULL) {
            return 0; 
        }
    }
    
    strncpy(g_pub_topic, topic, 63);
    strncpy(g_pub_payload, payload, 127);
    g_pub_pending = 1;
    return 1;
}

void ESP8266_Poll(void) {
    int res;
    u32 now = GetSysMs();
    char cmd_buf[256];

    ESP8266_CheckRemoteCmd();

    switch (g_net_state) {
        case WIFI_STATE_RESET:
            if (now - g_state_tick > 100) {
                AT_Send_Async("AT+RST\r\n", "OK", 1000);
                g_net_state = WIFI_STATE_INIT;
                g_state_tick = now;
            }
            break;

        case WIFI_STATE_INIT:
            if (g_at_state == AT_IDLE) {
                if (now - g_state_tick > 2500) {
                    AT_Send_Async("ATE0\r\n", "OK", 1000);
                    g_net_state = WIFI_STATE_JOIN_AP;
                    g_state_tick = now;
                }
            } else {
                 AT_Check_Status();
            }
            break;

        case WIFI_STATE_JOIN_AP:
            res = AT_Check_Status();
            if (res == 1) {
                sprintf(cmd_buf, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
                AT_Send_Async(cmd_buf, "GOT IP", 15000);
                g_net_state = WIFI_STATE_MQTT_CFG; 
            } else if (res == -1) {
                g_net_state = WIFI_STATE_ERROR;
                g_state_tick = now;
            }
            break;
            
        case WIFI_STATE_MQTT_CFG:
            res = AT_Check_Status();
            if (res == 1) {
                sprintf(cmd_buf, "AT+MQTTUSERCFG=0,1,\"%s\",\"\",\"\",0,0,\"\"\r\n", MQTT_CLIENT_ID);
                AT_Send_Async(cmd_buf, "OK", 2000);
                g_net_state = WIFI_STATE_MQTT_CONN;
            } else if (res == -1) {
                g_net_state = WIFI_STATE_ERROR;
                g_state_tick = now;
            }
            break;

        case WIFI_STATE_MQTT_CONN:
            res = AT_Check_Status();
            if (res == 1) {
                sprintf(cmd_buf, "AT+MQTTCONN=%d,\"%s\",%d,%d\r\n", MQTT_LINK_ID, MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_RECONNECT);
                AT_Send_Async(cmd_buf, "OK", 5000);
                g_net_state = WIFI_STATE_MQTT_SUB;
            } else if (res == -1) {
                g_net_state = WIFI_STATE_ERROR;
                g_state_tick = now;
            }
            break;
            
        case WIFI_STATE_MQTT_SUB:
            res = AT_Check_Status();
            if (res == 1) {
                AT_Send_Async("AT+MQTTSUB=0,\"netbar/seat001/cmd\",0\r\n", "OK", 3000);
                g_net_state = WIFI_STATE_RUNNING;
                ESP8266_MQTT_Pub_Async("netbar/seat001/debug", "sync");
            } else if (res == -1) {
                g_net_state = WIFI_STATE_ERROR;
                g_state_tick = now;
            }
            break;

        case WIFI_STATE_RUNNING:
            res = AT_Check_Status();
            if (res == 1) {
                if (g_pub_pending) {
                    sprintf(cmd_buf, "AT+MQTTPUB=%d,\"%s\",\"%s\",0,0\r\n", MQTT_LINK_ID, g_pub_topic, g_pub_payload);
                    AT_Send_Async(cmd_buf, "OK", 3000); 
                    g_pub_pending = 0;
                }
            } else if (res == -1) {
                 printf("ESP: Publish Failed. Resetting...\r\n");
                 g_net_state = WIFI_STATE_ERROR;
                 g_state_tick = now;
            }
            break;

        case WIFI_STATE_ERROR:
            if (now - g_state_tick > 5000) {
                printf("ESP: Resetting State Machine...\r\n");
                ESP8266_Init_Async();
            }
            break;
    }
}

static void extract_value(const char *src, const char *key, char *out, int max_len) {
    char *pos = strstr(src, key);
    int i = 0;
    if (pos) {
        pos += strlen(key);
        while (*pos && *pos != ';' && *pos != '\r' && *pos != '\n' && *pos != ',' && i < max_len - 1) {
            out[i++] = *pos++;
        }
        out[i] = 0;
    }
}

void ESP8266_CheckRemoteCmd(void) {
    char *buf = (char *)esp8266_rx_buf;
    char *msg_start;
    char temp_sec[16];

    if (esp8266_rx_len == 0) return;

    if (strstr(buf, "netbar/seat001/cmd") != NULL) {
        if (strstr(buf, "reset") != NULL)    esp8266_remote_reset_flag = 1;
        if (strstr(buf, "pc_on") != NULL)    esp8266_remote_pc_on_flag = 1;
        if (strstr(buf, "pc_off") != NULL)   esp8266_remote_pc_off_flag = 1;
        if (strstr(buf, "light_on") != NULL) esp8266_remote_light_on_flag = 1;
        if (strstr(buf, "light_off") != NULL) esp8266_remote_light_off_flag = 1;
        if (strstr(buf, "checkout") != NULL) esp8266_remote_checkout_flag = 1;
        if (strstr(buf, "maint_on") != NULL) esp8266_remote_maint_on_flag = 1;
        if (strstr(buf, "maint_off") != NULL) esp8266_remote_maint_off_flag = 1;

        if (strstr(buf, "card_ok") != NULL || strstr(buf, "restore_session") != NULL) {
            esp8266_remote_card_ok_flag = 1;
            extract_value(buf, "name=", esp8266_remote_user_name, 32);
            extract_value(buf, "balance=", esp8266_remote_balance_str, 16);
            
            // ★★★ 新增：解析服务器发来的 UID ★★★
            extract_value(buf, "uid=", esp8266_remote_uid, 32); 

            temp_sec[0] = 0;
            extract_value(buf, "sec=", temp_sec, 16);
            if (temp_sec[0] != 0) sscanf(temp_sec, "%lu", &esp8266_remote_restore_sec);
            else esp8266_remote_restore_sec = 0;
        }
        
        if (strstr(buf, "card_err") != NULL) {
            esp8266_remote_card_err_flag = 1;
            extract_value(buf, "msg=", esp8266_card_err_msg, 32);
        }

        if ((msg_start = strstr(buf, "msg:")) != NULL) {
            msg_start += 4;
            int k = 0;
            while (msg_start[k] && msg_start[k] != '\r' && msg_start[k] != '\n' && msg_start[k] != '"' && k < 31) {
                esp8266_remote_msg[k] = msg_start[k];
                k++;
            }
            esp8266_remote_msg[k] = 0;
            esp8266_remote_msg_flag = 1;
        }
        
        if (esp8266_rx_len > ESP_RX_BUF_SIZE - 64) {
             esp8266_rx_len = 0;
             esp8266_rx_buf[0] = 0;
        }
    }
}

void ESP8266_DebugDump(void) {
}
