#include "esp8266.h"

volatile u16       esp8266_rx_len = 0;
volatile u8        esp8266_rx_buf[ESP8266_RX_BUF_SIZE];
volatile WifiState g_wifi_state   = WIFI_STATE_IDLE;
volatile MQTTState g_mqtt_state   = MQTT_STATE_IDLE;

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

char        esp8266_remote_msg[32];
char        esp8266_card_err_msg[32];
char        esp8266_remote_user_name[32];
char        esp8266_remote_balance_str[16];
volatile u32 esp8266_remote_restore_sec    = 0;

static u8 s_mqtt_user_ok = 0;

// 底层串口配置
static void ESP8266_USART_Config(void) {
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;
    RCC_AHB1PeriphClockCmd(ESP8266_GPIO_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(ESP8266_USART_CLK, ENABLE);
    GPIO_PinAFConfig(ESP8266_TX_PORT, ESP8266_TX_PIN_SOURCE, GPIO_AF_USART3);
    GPIO_PinAFConfig(ESP8266_RX_PORT, ESP8266_RX_PIN_SOURCE, GPIO_AF_USART3);
    GPIO_InitStructure.GPIO_Pin = ESP8266_TX_PIN | ESP8266_RX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(ESP8266_TX_PORT, &GPIO_InitStructure);
    USART_InitStructure.USART_BaudRate = 115200;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(ESP8266_USART, &USART_InitStructure);
    USART_ITConfig(ESP8266_USART, USART_IT_RXNE, ENABLE);
    NVIC_InitStructure.NVIC_IRQChannel = ESP8266_USART_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    USART_Cmd(ESP8266_USART, ENABLE);
}

void USART3_IRQHandler(void) {
    if (USART_GetITStatus(ESP8266_USART, USART_IT_RXNE) != RESET) {
        u8 ch = USART_ReceiveData(ESP8266_USART);
        if (esp8266_rx_len < ESP8266_RX_BUF_SIZE - 1) {
            esp8266_rx_buf[esp8266_rx_len++] = ch;
            esp8266_rx_buf[esp8266_rx_len] = 0;
        }
    }
}

static void ESP8266_SendByte(u8 ch) {
    while (USART_GetFlagStatus(ESP8266_USART, USART_FLAG_TXE) == RESET);
    USART_SendData(ESP8266_USART, ch);
}

static void ESP8266_SendString(const char *s) {
    while (*s) ESP8266_SendByte((u8)*s++);
}

static void ESP8266_ClearBuf(void) {
    esp8266_rx_len = 0;
    esp8266_rx_buf[0] = 0;
}

ESP8266_Status ESP8266_SendCmdWait(const char *cmd, const char *expect, u32 timeout_ms) {
    u32 t = 0;
    ESP8266_ClearBuf();
    if (cmd && cmd[0]) ESP8266_SendString(cmd);
    while (t < timeout_ms) {
        if (strstr((const char *)esp8266_rx_buf, expect) != NULL) return ESP8266_OK;
        delay_ms(10);
        t += 10;
    }
    return ESP8266_TIMEOUT;
}

void ESP8266_Init(void) {
    ESP8266_USART_Config();
    delay_ms(200);
    ESP8266_ClearBuf();
    ESP8266_SendString("AT+RST\r\n");
    delay_ms(2500);
    ESP8266_ClearBuf();
    if (ESP8266_SendCmdWait("AT\r\n", "OK", 2000) != ESP8266_OK) {
        printf("ESP8266: AT fail\r\n");
        g_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    ESP8266_SendCmdWait("ATE0\r\n", "OK", 1000);
    ESP8266_SendCmdWait("AT+CWMODE=1\r\n", "OK", 1000);
    ESP8266_SendCmdWait("AT+CIPMUX=0\r\n", "OK", 1000);
    g_wifi_state = WIFI_STATE_DISCONNECTED;
}

ESP8266_Status ESP8266_ConnectAP(const char *ssid, const char *pwd) {
    char cmd[128];
    g_wifi_state = WIFI_STATE_CONNECTING;
    ESP8266_SendCmdWait("AT+CWQAP\r\n", "OK", 1000);
    delay_ms(200);
    sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
    printf("ESP8266: join AP...\r\n");
    if (ESP8266_SendCmdWait(cmd, "WIFI GOT IP", 20000) == ESP8266_OK) {
        printf("ESP8266: join AP ok\r\n");
        g_wifi_state = WIFI_STATE_CONNECTED;
        s_mqtt_user_ok = 0;
        return ESP8266_OK;
    }
    g_wifi_state = WIFI_STATE_ERROR;
    return ESP8266_TIMEOUT;
}

ESP8266_Status ESP8266_MQTT_Config(void) {
    char cmd[128];
    sprintf(cmd, "AT+MQTTUSERCFG=0,1,\"%s\",\"\",\"\",0,0,\"\"\r\n", MQTT_CLIENT_ID);
    printf("ESP8266: MQTTUSERCFG cmd: %s", cmd);
    ESP8266_Status st = ESP8266_SendCmdWait(cmd, "OK", 5000);
    if (st == ESP8266_OK) {
        printf("ESP8266: MQTTUSERCFG ok\r\n");
        g_mqtt_state = MQTT_STATE_CONFIGURED;
        s_mqtt_user_ok = 1;
    } else {
        printf("ESP8266: MQTTUSERCFG fail\r\n");
        g_mqtt_state = MQTT_STATE_ERROR;
        s_mqtt_user_ok = 0;
    }
    return st;
}

ESP8266_Status ESP8266_MQTT_Connect(void) {
    char cmd[128];
    if (g_wifi_state != WIFI_STATE_CONNECTED) return ESP8266_ERROR;
    
    // ★★★ 打印连接命令，确保IP正确 ★★★
    sprintf(cmd, "AT+MQTTCONN=%d,\"%s\",%d,%d\r\n", MQTT_LINK_ID, MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_RECONNECT);
    printf("ESP8266: MQTTCONN cmd: %s", cmd); 
    
    ESP8266_Status st = ESP8266_SendCmdWait(cmd, "OK", 10000);
    
    // 兼容 +MQTTCONNECTED 响应
    if ((st == ESP8266_OK) || (strstr((const char *)esp8266_rx_buf, "+MQTTCONNECTED") != NULL)) {
        printf("ESP8266: MQTT connected\r\n");
        g_mqtt_state = MQTT_STATE_CONNECTED;
        
        // ★★★ 核心修改：连接后延时 1秒，等待链路稳定 ★★★
        delay_ms(1000);
        
        ESP8266_SendCmdWait("AT+MQTTSUB=0,\"netbar/seat001/cmd\",0\r\n", "OK", 5000);
        
        // 发送同步请求
        ESP8266_MQTT_Publish("netbar/seat001/debug", "sync", 0, 0);
        return ESP8266_OK;
    }
    printf("ESP8266: MQTT connect fail\r\n");
    g_mqtt_state = MQTT_STATE_ERROR;
    return ESP8266_ERROR;
}

ESP8266_Status ESP8266_MQTT_Publish(const char *topic, const char *payload, u8 qos, u8 retain) {
    char cmd[256];
    
    // 如果还没连上，直接返回错误，不发命令，防止 ERROR 刷屏
    if (g_mqtt_state != MQTT_STATE_CONNECTED) return ESP8266_ERROR;
    
    sprintf(cmd, "AT+MQTTPUB=%d,\"%s\",\"%s\",%d,%d\r\n", MQTT_LINK_ID, topic, payload, qos, retain);
    
    // 调试打印
    // printf("ESP8266: MQTTPUB cmd: %s", cmd);

    ESP8266_Status st = ESP8266_SendCmdWait(cmd, "OK", 8000);
    if (st != ESP8266_OK) {
        printf("ESP8266: MQTTPUB fail -> Disconnecting\r\n");
        // 发布失败说明连接断了，强制状态复位，触发重连
        g_wifi_state = WIFI_STATE_DISCONNECTED;
        g_mqtt_state = MQTT_STATE_ERROR; 
    }
    return st;
}

void ESP8266_MQTT_Task_1s(void) {
    static u8 retry_cnt = 0;
    if (g_wifi_state != WIFI_STATE_CONNECTED) {
        g_mqtt_state = MQTT_STATE_IDLE;
        s_mqtt_user_ok = 0;
        return;
    }
    if (!s_mqtt_user_ok) {
        ESP8266_MQTT_Config();
        return;
    }
    if (g_mqtt_state != MQTT_STATE_CONNECTED) {
        retry_cnt++;
        if (retry_cnt >= 5) { // 每5秒重试
            retry_cnt = 0;
            ESP8266_MQTT_Connect();
        }
    }
}

void ESP8266_Task_1s(void) {
    static u8 wifi_retry_cnt = 0;
    if (g_wifi_state == WIFI_STATE_CONNECTED) {
        ESP8266_MQTT_Task_1s();
        wifi_retry_cnt = 0;
        return;
    }
    if (g_wifi_state == WIFI_STATE_CONNECTING) {
        wifi_retry_cnt++;
        if (wifi_retry_cnt > 20) { // 超时重置
            g_wifi_state = WIFI_STATE_DISCONNECTED;
            wifi_retry_cnt = 0;
        }
        return;
    }
    wifi_retry_cnt++;
    if (wifi_retry_cnt >= 5) {
        wifi_retry_cnt = 0;
        ESP8266_ConnectAP(WIFI_SSID, WIFI_PASS);
    }
}

static void extract_value(const char *src, const char *key, char *out, int max_len)
{
    char *pos = strstr(src, key);
    int i = 0;
    if (pos) {
        pos += strlen(key);
        while (*pos && *pos != ';' && *pos != '\r' && *pos != '\n' && i < max_len - 1) {
            out[i++] = *pos++;
        }
        out[i] = 0;
    }
}

void ESP8266_CheckRemoteCmd(void)
{
    char *buf = (char *)esp8266_rx_buf;
    char *topic_pos;
    char temp_sec[16];

    if (esp8266_rx_len == 0) return;
    topic_pos = strstr(buf, "netbar/seat001/cmd");
    if (topic_pos == NULL) return;

    if (strstr(topic_pos, "reset") != NULL)    esp8266_remote_reset_flag = 1;
    if (strstr(topic_pos, "pc_on") != NULL)    esp8266_remote_pc_on_flag = 1;
    if (strstr(topic_pos, "pc_off") != NULL)   esp8266_remote_pc_off_flag = 1;
    if (strstr(topic_pos, "light_on") != NULL) esp8266_remote_light_on_flag = 1;
    if (strstr(topic_pos, "light_off") != NULL) esp8266_remote_light_off_flag = 1;
    if (strstr(topic_pos, "checkout") != NULL) esp8266_remote_checkout_flag = 1;
    if (strstr(topic_pos, "maint_on") != NULL) esp8266_remote_maint_on_flag = 1;
    if (strstr(topic_pos, "maint_off") != NULL) esp8266_remote_maint_off_flag = 1;

    if (strstr(topic_pos, "card_ok") != NULL || strstr(topic_pos, "restore_session") != NULL)
    {
        esp8266_remote_card_ok_flag = 1;
        extract_value(topic_pos, "name=", esp8266_remote_user_name, 32);
        extract_value(topic_pos, "balance=", esp8266_remote_balance_str, 16);
        temp_sec[0] = 0;
        extract_value(topic_pos, "sec=", temp_sec, 16);
        if (temp_sec[0] != 0) sscanf(temp_sec, "%lu", &esp8266_remote_restore_sec);
        else esp8266_remote_restore_sec = 0;
    }
    
    if (strstr(topic_pos, "card_err") != NULL)
    {
        esp8266_remote_card_err_flag = 1;
        extract_value(topic_pos, "msg=", esp8266_card_err_msg, 32);
    }

    if (strstr(topic_pos, "msg:") != NULL)
    {
        char *msg_pos = strstr(topic_pos, "msg:");
        int i = 0;
        msg_pos += 4;
        while (msg_pos[i] != 0 && msg_pos[i] != '\r' && msg_pos[i] != '\n' && i < 31) {
            esp8266_remote_msg[i] = msg_pos[i];
            i++;
        }
        esp8266_remote_msg[i] = 0;
        esp8266_remote_msg_flag = 1;
    }

    esp8266_rx_len = 0;
    esp8266_rx_buf[0] = 0;
}

WifiState ESP8266_GetState(void) { return g_wifi_state; }
void ESP8266_DebugDump(void) { if (esp8266_rx_len > 0) { printf("ESP RX: %s\r\n", esp8266_rx_buf); ESP8266_ClearBuf(); } }
