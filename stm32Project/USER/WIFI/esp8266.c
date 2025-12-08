#include "esp8266.h"


// 接收缓冲
volatile u16       esp8266_rx_len = 0;
volatile u8        esp8266_rx_buf[ESP8266_RX_BUF_SIZE];
volatile WifiState g_wifi_state   = WIFI_STATE_IDLE;
volatile MQTTState g_mqtt_state   = MQTT_STATE_IDLE;





// ★ 远程控制：标志 & 短消息缓冲
volatile u8 esp8266_remote_reset_flag      = 0;  // 远程软复位
volatile u8 esp8266_remote_pc_on_flag      = 0;
volatile u8 esp8266_remote_pc_off_flag     = 0;
volatile u8 esp8266_remote_light_on_flag   = 0;
volatile u8 esp8266_remote_light_off_flag  = 0;
volatile u8 esp8266_remote_checkout_flag   = 0;

volatile u8 esp8266_remote_maint_on_flag   = 0;  // 维护模式开
volatile u8 esp8266_remote_maint_off_flag  = 0;  // 维护模式关

volatile u8 esp8266_remote_card_ok_flag    = 0;  // 刷卡验证通过
volatile u8 esp8266_remote_card_err_flag   = 0;  // 刷卡验证失败

volatile u8 esp8266_remote_msg_flag        = 0;  // 短消息
char        esp8266_remote_msg[32];
char        esp8266_card_err_msg[32];





// MQTT USERCFG 是否已成功配置
static u8 s_mqtt_user_ok = 0;

//================= 串口底层 =========================

// 配置 USART3: PB10(TX), PB11(RX) 用于 ESP8266
static void ESP8266_USART_Config(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef  NVIC_InitStructure;

    // 打开 GPIOB 和 USART3 时钟
    RCC_AHB1PeriphClockCmd(ESP8266_GPIO_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(ESP8266_USART_CLK, ENABLE);

    // PB10 -> TX, PB11 -> RX 复用为 USART3
    GPIO_PinAFConfig(ESP8266_TX_PORT, ESP8266_TX_PIN_SOURCE, GPIO_AF_USART3);
    GPIO_PinAFConfig(ESP8266_RX_PORT, ESP8266_RX_PIN_SOURCE, GPIO_AF_USART3);

    // TX: PB10
    GPIO_InitStructure.GPIO_Pin   = ESP8266_TX_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(ESP8266_TX_PORT, &GPIO_InitStructure);

    // RX: PB11
    GPIO_InitStructure.GPIO_Pin   = ESP8266_RX_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_Init(ESP8266_RX_PORT, &GPIO_InitStructure);

    // USART3 参数
    USART_InitStructure.USART_BaudRate            = 115200;
    USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits            = USART_StopBits_1;
    USART_InitStructure.USART_Parity              = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(ESP8266_USART, &USART_InitStructure);

    // 开接收中断
    USART_ITConfig(ESP8266_USART, USART_IT_RXNE, ENABLE);

    // NVIC
    NVIC_InitStructure.NVIC_IRQChannel = ESP8266_USART_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(ESP8266_USART, ENABLE);
}

// USART3 中断服务函数
void USART3_IRQHandler(void)
{
    if (USART_GetITStatus(ESP8266_USART, USART_IT_RXNE) != RESET)
    {
        u8 ch = USART_ReceiveData(ESP8266_USART);

        if (esp8266_rx_len < ESP8266_RX_BUF_SIZE - 1)
        {
            esp8266_rx_buf[esp8266_rx_len++] = ch;
            esp8266_rx_buf[esp8266_rx_len]   = 0; // 末尾补 0，方便 strstr
        }
    }
}

// 发送一个字节
static void ESP8266_SendByte(u8 ch)
{
    while (USART_GetFlagStatus(ESP8266_USART, USART_FLAG_TXE) == RESET);
    USART_SendData(ESP8266_USART, ch);
}

// 发送字符串
static void ESP8266_SendString(const char *s)
{
    while (*s)
    {
        ESP8266_SendByte((u8)*s++);
    }
}

// 清空接收缓冲
static void ESP8266_ClearBuf(void)
{
    esp8266_rx_len    = 0;
    esp8266_rx_buf[0] = 0;
}

//================= 公共工具 =========================

// 发送命令并等待期望字符串或超时
ESP8266_Status ESP8266_SendCmdWait(const char *cmd, const char *expect, u32 timeout_ms)
{
    u32 t = 0;

    ESP8266_ClearBuf();
    if (cmd && cmd[0])
    {
        ESP8266_SendString(cmd);
    }

    while (t < timeout_ms)
{
    if (strstr((const char *)esp8266_rx_buf, expect) != NULL)
    {
        return ESP8266_OK;
    }
    delay_ms(10);
    t += 10;
}

    return ESP8266_TIMEOUT;
}

//================= 初始化 & 连接 AP ==================

// 初始化 ESP8266：只做 AT 基础配置，不连 AP
void ESP8266_Init(void)
{
    ESP8266_USART_Config();
    delay_ms(200);

    // ① 先软复位 ESP8266，一定要把上一次的 WiFi/MQTT 状态清空
    ESP8266_ClearBuf();
    ESP8266_SendString("AT+RST\r\n");
    delay_ms(2500);   // 模块重启时间，安信可文档一般建议 ≥2s
    ESP8266_ClearBuf();

    // ② 基本 AT 检测
    if (ESP8266_SendCmdWait("AT\r\n", "OK", 2000) != ESP8266_OK)
    {
        printf("ESP8266: AT fail\r\n");
        g_wifi_state   = WIFI_STATE_ERROR;
#ifdef MQTT_STATE_IDLE  // 如果你没定义 MQTT 状态，这一段可以删掉
        g_mqtt_state   = MQTT_STATE_ERROR;
#endif
        return;
    }
    printf("ESP8266: AT ok\r\n");

    // ③ 常规设置：关回显、STA 模式、单连接
    ESP8266_SendCmdWait("ATE0\r\n", "OK", 1000);
    ESP8266_SendCmdWait("AT+CWMODE=1\r\n", "OK", 1000);
    ESP8266_SendCmdWait("AT+CIPMUX=0\r\n", "OK", 1000);

    // ④ 本地状态机复位
    g_wifi_state   = WIFI_STATE_DISCONNECTED;
#ifdef MQTT_STATE_IDLE
    g_mqtt_state   = MQTT_STATE_IDLE;
#endif
    // 如果你前面有 s_mqtt_user_ok 之类的标志，也在这里清 0
    // s_mqtt_user_ok = 0;
}




// 连接指定 AP
ESP8266_Status ESP8266_ConnectAP(const char *ssid, const char *pwd)
{
    char cmd[128];

    g_wifi_state = WIFI_STATE_CONNECTING;

    // 先断开
    ESP8266_SendCmdWait("AT+CWQAP\r\n", "OK", 1000);
    delay_ms(200);

    sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
    printf("ESP8266: join AP...\r\n");

    if (ESP8266_SendCmdWait(cmd, "WIFI GOT IP", 20000) == ESP8266_OK)
    {
        printf("ESP8266: join AP ok\r\n");
        g_wifi_state   = WIFI_STATE_CONNECTED;
        g_mqtt_state   = MQTT_STATE_IDLE;
        s_mqtt_user_ok = 0;
        return ESP8266_OK;
    }

    printf("ESP8266: join AP timeout\r\n");
    g_wifi_state   = WIFI_STATE_ERROR;
    g_mqtt_state   = MQTT_STATE_ERROR;
    s_mqtt_user_ok = 0;
    return ESP8266_TIMEOUT;
}

//================= HTTP POST（可选） =================

// 发送一个 HTTP POST（JSON 文本）
ESP8266_Status ESP8266_HttpPost(
    const char *host,
    u16 port,
    const char *path,
    const char *body)
{
    char cmd[128];
    char http[512];
    u16 body_len;
    u16 http_len;
    u16 i;

    // 1. 建 TCP 连接
    sprintf(cmd, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", host, port);
    if (ESP8266_SendCmdWait(cmd, "CONNECT", 5000) != ESP8266_OK)
    {
        printf("ESP8266: CIPSTART fail\r\n");
        return ESP8266_ERROR;
    }

    // 2. 组 HTTP 报文
    body_len = (u16)strlen(body);
    http_len = (u16)sprintf(http,
                            "POST %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Content-Type: application/json\r\n"
                            "Connection: close\r\n"
                            "Content-Length: %d\r\n"
                            "\r\n"
                            "%s",
                            path, host, body_len, body);

    // 3. 通知将要发送的长度
    sprintf(cmd, "AT+CIPSEND=%d\r\n", http_len);
    if (ESP8266_SendCmdWait(cmd, ">", 2000) != ESP8266_OK)
    {
        printf("ESP8266: CIPSEND '>' timeout\r\n");
        return ESP8266_TIMEOUT;
    }

    // 4. 发送数据
    ESP8266_ClearBuf();
    for (i = 0; i < http_len; i++)
    {
        ESP8266_SendByte((u8)http[i]);
    }

    // 5. 等待发送完成
    if (ESP8266_SendCmdWait("", "SEND OK", 5000) != ESP8266_OK)
    {
        printf("ESP8266: SEND fail\r\n");
        return ESP8266_ERROR;
    }

    // 简单等连接关闭
    ESP8266_SendCmdWait("", "CLOSED", 5000);

    return ESP8266_OK;
}

//================= MQTT 实现 ==========================

// 配置 MQTT 用户信息：AT+MQTTUSERCFG
// 这里直接用你 USB-TTL 手打成功的格式：
// AT+MQTTUSERCFG=0,1,"seat001","","",0,0,""
ESP8266_Status ESP8266_MQTT_Config(void)
{
    char cmd[128];
    ESP8266_Status st;

    sprintf(cmd,
            "AT+MQTTUSERCFG=0,1,\"%s\",\"\",\"\",0,0,\"\"\r\n",
            MQTT_CLIENT_ID);

    printf("ESP8266: MQTTUSERCFG cmd: %s", cmd);

    st = ESP8266_SendCmdWait(cmd, "OK", 5000);

    // 打印模块原始返回，方便排查
    printf("ESP8266: MQTTUSERCFG resp: %s\r\n", esp8266_rx_buf);

    if (st == ESP8266_OK)
    {
        printf("ESP8266: MQTTUSERCFG ok\r\n");
        g_mqtt_state   = MQTT_STATE_CONFIGURED;
        s_mqtt_user_ok = 1;
    }
    else
    {
        printf("ESP8266: MQTTUSERCFG fail\r\n");
        g_mqtt_state   = MQTT_STATE_ERROR;
        s_mqtt_user_ok = 0;
    }

    return st;
}

// 连接 MQTT Broker：AT+MQTTCONN
// 使用官方格式：AT+MQTTCONN=<LinkID>,"host",port,reconnect
// 连接 MQTT Broker：AT+MQTTCONN=<LinkID>,"host",port,reconnect
ESP8266_Status ESP8266_MQTT_Connect(void)
{
    char cmd[128];
    ESP8266_Status st;
    ESP8266_Status sub_st;

    if (g_wifi_state != WIFI_STATE_CONNECTED)
    {
        printf("ESP8266: MQTTCONN skip, wifi not connected\r\n");
        return ESP8266_ERROR;
    }

    sprintf(cmd,
            "AT+MQTTCONN=%d,\"%s\",%d,%d\r\n",
            MQTT_LINK_ID,
            MQTT_BROKER_HOST,
            MQTT_BROKER_PORT,
            MQTT_RECONNECT);

    printf("ESP8266: MQTTCONN cmd: %s", cmd);

    st = ESP8266_SendCmdWait(cmd, "OK", 15000);

    // 打印模块返回内容
    printf("ESP8266: MQTTCONN resp: %s\r\n", esp8266_rx_buf);

    // ★ 只要认为“连接成功”（收到 OK 或者带 +MQTTCONNECTED），就统一走这里
    if ((st == ESP8266_OK) ||
        (strstr((const char *)esp8266_rx_buf, "+MQTTCONNECTED") != NULL))
    {
        printf("ESP8266: MQTT connected\r\n");
        g_mqtt_state = MQTT_STATE_CONNECTED;

        // ★ 连接成功后，订阅命令 topic：netbar/seat001/cmd
        sub_st = ESP8266_SendCmdWait(
            "AT+MQTTSUB=0,\"netbar/seat001/cmd\",0\r\n",
            "OK",
            5000
        );
        if (sub_st == ESP8266_OK)
        {
            printf("ESP8266: MQTTSUB cmd topic ok\r\n");
        }
        else
        {
            printf("ESP8266: MQTTSUB cmd topic FAIL\r\n");
        }

        return ESP8266_OK;
    }

    // 真连不上才走这里
    printf("ESP8266: MQTT connect fail\r\n");
    g_mqtt_state = MQTT_STATE_ERROR;
    return ESP8266_ERROR;
}

// 发布 MQTT 消息：AT+MQTTPUB=<LinkID>,"topic","data",qos,retain
ESP8266_Status ESP8266_MQTT_Publish(const char *topic,
                                    const char *payload,
                                    u8 qos,
                                    u8 retain)
{
    char cmd[256];
    ESP8266_Status st;

    // 如果你在文件前面有 MQTTState 和 g_mqtt_state，可以先判断一下
    // 没有的话，把下面这段 #if/#endif 整块删掉也可以
#ifdef MQTT_STATE_IDLE
    extern volatile MQTTState g_mqtt_state;
    if (g_mqtt_state != MQTT_STATE_CONNECTED)
    {
        printf("ESP8266: MQTTPUB skip, mqtt not connected\r\n");
        return ESP8266_ERROR;
    }
#endif

    // 组 AT 命令（注意 payload 里不要有双引号）
    sprintf(cmd,
            "AT+MQTTPUB=%d,\"%s\",\"%s\",%d,%d\r\n",
            MQTT_LINK_ID,
            topic,
            payload,
            qos,
            retain);

    printf("ESP8266: MQTTPUB cmd: %s", cmd);

    // 发送并等待 OK
    st = ESP8266_SendCmdWait(cmd, "OK", 8000);

    // 打印模块原始返回，方便调试
    printf("ESP8266: MQTTPUB resp: %s\r\n", esp8266_rx_buf);

    if (st == ESP8266_OK)
    {
        printf("ESP8266: MQTTPUB ok\r\n");
    }
    else
    {
        printf("ESP8266: MQTTPUB fail\r\n");

        // 这里顺便把 WiFi/MQTT 状态打成异常，方便上层更新图标、触发重连
        g_wifi_state = WIFI_STATE_DISCONNECTED;
#ifdef MQTT_STATE_IDLE
        g_mqtt_state   = MQTT_STATE_ERROR;
        // 如果文件里有 s_mqtt_user_ok 这类标志，也可以在这里清 0：
        // s_mqtt_user_ok = 0;
#endif
    }

    return st;
}

// 每秒调用一次，用于维持 MQTT 连接
void ESP8266_MQTT_Task_1s(void)
{
    static u8 retry_cnt = 0;

    // WiFi 没连上就清空 MQTT 状态
    if (g_wifi_state != WIFI_STATE_CONNECTED)
    {
        g_mqtt_state   = MQTT_STATE_IDLE;
        s_mqtt_user_ok = 0;
        retry_cnt      = 0;
        return;
    }

    // 1) 先确保 USERCFG 成功（失败就每次都重试）
    if (!s_mqtt_user_ok)
    {
        ESP8266_MQTT_Config();
        return;
    }

    // 2) 还没连上 Broker，每 5 秒重试一次 MQTTCONN
    if (g_mqtt_state != MQTT_STATE_CONNECTED)
    {
        retry_cnt++;
        if (retry_cnt >= 5)
        {
            retry_cnt = 0;
            ESP8266_MQTT_Connect();
        }
        return;
    }

    // 3) 已连接，暂时不做额外心跳，后面需要可以在这里加
}








//================= WiFi 状态 & 调试 ==================

WifiState ESP8266_GetState(void)
{
    return g_wifi_state;
}


// 每 1 秒调用一次：负责 WiFi 掉线重连 + MQTT 维护
void ESP8266_Task_1s(void)
{
    static u8 wifi_retry_cnt = 0;

    // 1. 如果已经连上 AP，交给 MQTT 的 1 秒任务去维护连接
    if (g_wifi_state == WIFI_STATE_CONNECTED)
    {
        // 你之前写好的 MQTT 心跳 / 重连逻辑
        ESP8266_MQTT_Task_1s();
        return;
    }

    // 2. 如果正在连 AP，就先等结果，避免狂发 AT+CWJAP
    if (g_wifi_state == WIFI_STATE_CONNECTING)
    {
        return;
    }

    // 3. 其它状态（IDLE / DISCONNECTED / ERROR）每 5 秒重连一次 AP
    wifi_retry_cnt++;
    if (wifi_retry_cnt < 5)
    {
        return;
    }
    wifi_retry_cnt = 0;

    printf("ESP8266: reconnect AP...\r\n");
    ESP8266_ConnectAP(WIFI_SSID, WIFI_PASS);
    // ESP8266_ConnectAP 内部会根据结果把 g_wifi_state 设为 CONNECTED 或 ERROR
}




void ESP8266_DebugDump(void)
{
    if (esp8266_rx_len > 0)
    {
        printf("ESP RX: %s\r\n", esp8266_rx_buf);
        ESP8266_ClearBuf();
    }
}

#include "stm32f4xx.h"   // 确保能用 NVIC_SystemReset，如果 sys.h 里已经包含也可以不加

//==================================================================
//   从串口接收缓冲里解析服务器下发的命令
//   Topic: netbar/seat001/cmd
//   Payload 可能是：
//      reset
//      pc_on / pc_off
//      light_on / light_off
//      checkout
//      maint_on / maint_off
//      msg:你好呀
//      card_ok;user=张三;balance=50.00;age=20
//      card_err;msg=余额不足，请充值
//
//   解析结果：只置位一批标志，由 main.c 在 1 秒任务里真正执行
//==================================================================
void ESP8266_CheckRemoteCmd(void)
{
    char *buf;
    char *topic_pos;
    char *msg_pos;
    int   i;

    // 1. 缓冲区里没东西，不处理
    if (esp8266_rx_len == 0)
    {
        return;
    }

    buf = (char *)esp8266_rx_buf;

    // 2. 只处理来自命令 topic 的消息：netbar/seat001/cmd
    topic_pos = strstr(buf, "netbar/seat001/cmd");
    if (topic_pos == NULL)
    {
        // 不是我们关心的主题，先不清空，留给其它调试用
        return;
    }

    // 3. 按关键字粗暴匹配（不再抠引号）

    // --- 基本控制 ---
    if (strstr(topic_pos, "reset") != NULL)
    {
        esp8266_remote_reset_flag = 1;
    }
    if (strstr(topic_pos, "pc_on") != NULL)
    {
        esp8266_remote_pc_on_flag = 1;
    }
    if (strstr(topic_pos, "pc_off") != NULL)
    {
        esp8266_remote_pc_off_flag = 1;
    }
    if (strstr(topic_pos, "light_on") != NULL)
    {
        esp8266_remote_light_on_flag = 1;
    }
    if (strstr(topic_pos, "light_off") != NULL)
    {
        esp8266_remote_light_off_flag = 1;
    }
    if (strstr(topic_pos, "checkout") != NULL)
    {
        esp8266_remote_checkout_flag = 1;
    }

    // --- 维护模式 ---
    if (strstr(topic_pos, "maint_on") != NULL)
    {
        esp8266_remote_maint_on_flag = 1;
    }
    if (strstr(topic_pos, "maint_off") != NULL)
    {
        esp8266_remote_maint_off_flag = 1;
    }

    // --- 刷卡验证结果 ---
    if (strstr(topic_pos, "card_ok") != NULL)
    {
        esp8266_remote_card_ok_flag = 1;
    }
    if (strstr(topic_pos, "card_err") != NULL)
    {
        esp8266_remote_card_err_flag = 1;

        // 从 payload 里提取 msg= 后面的文字
        msg_pos = strstr(topic_pos, "msg=");
        if (msg_pos != NULL)
        {
            msg_pos += 4; // 跳过 "msg="
            i = 0;
            while (msg_pos[i] != 0 &&
                   msg_pos[i] != '\r' &&
                   msg_pos[i] != '\n' &&
                   msg_pos[i] != '"' &&
                   i < (int)sizeof(esp8266_card_err_msg) - 1)
            {
                esp8266_card_err_msg[i] = msg_pos[i];
                i++;
            }
            esp8266_card_err_msg[i] = 0;
        }
    }

    // --- 短消息 msg:xxx ---
    msg_pos = strstr(topic_pos, "msg:");
    if (msg_pos != NULL)
    {
        msg_pos += 4; // 跳过 "msg:"
        i = 0;
        while (msg_pos[i] != 0 &&
               msg_pos[i] != '\r' &&
               msg_pos[i] != '\n' &&
               msg_pos[i] != '"' &&
               i < (int)sizeof(esp8266_remote_msg) - 1)
        {
            esp8266_remote_msg[i] = msg_pos[i];
            i++;
        }
        esp8266_remote_msg[i] = 0;
        esp8266_remote_msg_flag = 1;
    }

    // 5. 这条命令已经处理完了，清空缓冲，避免下次重复触发
    if (esp8266_remote_reset_flag      ||
        esp8266_remote_pc_on_flag      ||
        esp8266_remote_pc_off_flag     ||
        esp8266_remote_light_on_flag   ||
        esp8266_remote_light_off_flag  ||
        esp8266_remote_checkout_flag   ||
        esp8266_remote_maint_on_flag   ||
        esp8266_remote_maint_off_flag  ||
        esp8266_remote_card_ok_flag    ||
        esp8266_remote_card_err_flag   ||
        esp8266_remote_msg_flag)
    {
        printf("ESP8266: MQTT cmd handled, buf clear\r\n");
        esp8266_rx_len    = 0;
        esp8266_rx_buf[0] = 0;
    }
}
