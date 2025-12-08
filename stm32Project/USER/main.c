#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "lcd.h"
#include "text.h"
#include "fontupd.h"
#include "w25qxx.h"
#include "touch.h"
#include "seat_bsp.h"
#include "rfid_app.h"
#include "esp8266.h"
#include <string.h>
#include <stdio.h>

void LCD_Display_Dir(u8 dir);  // 在 lcd.c 里实现

//----------------- 计费配置 -----------------
#define PRICE_PER_MIN       1       // 1 元/分钟（测试）

//----------------- 按钮坐标 -----------------
// 上机界面的三个按钮
#define BTN_PC_X1           20
#define BTN_PC_Y1           170
#define BTN_PC_X2           140
#define BTN_PC_Y2           210

#define BTN_LIGHT_X1        180
#define BTN_LIGHT_Y1        170
#define BTN_LIGHT_X2        300
#define BTN_LIGHT_Y2        210

#define BTN_EXIT_X1         80
#define BTN_EXIT_Y1         220
#define BTN_EXIT_X2         240
#define BTN_EXIT_Y2         260

// 确认弹窗
#define CONF_X1             40
#define CONF_Y1             130
#define CONF_X2             280
#define CONF_Y2             260

#define CONF_BTN_OK_X1      (CONF_X1 + 20)
#define CONF_BTN_OK_Y1      (CONF_Y2  - 50)
#define CONF_BTN_OK_X2      (CONF_X1 + 100)
#define CONF_BTN_OK_Y2      (CONF_Y2  - 20)

#define CONF_BTN_CANCEL_X1  (CONF_X2  - 100)
#define CONF_BTN_CANCEL_Y1  (CONF_Y2  - 50)
#define CONF_BTN_CANCEL_X2  (CONF_X2  - 20)
#define CONF_BTN_CANCEL_Y2  (CONF_Y2  - 20)

//----------------- 状态枚举 -----------------
typedef enum
{
    STATE_IDLE = 0,      // 未上机
    STATE_INUSE          // 上机中
} app_state_t;

typedef enum
{
    SCREEN_WELCOME = 0,  // 欢迎界面
    SCREEN_INUSE,        // 上机界面
    SCREEN_CONFIRM       // 确认弹窗界面（覆盖在上机界面上）
} screen_t;

typedef enum
{
    CONFIRM_NONE = 0,
    CONFIRM_PC_OFF,      // 确认关机
    CONFIRM_EXIT         // 确认下机结算
} confirm_type_t;

//----------------- 应用上下文 -----------------
typedef struct
{
    app_state_t   state;        // 当前业务状态
    u8            has_user;     // 是否有当前用户
    u8            uid[4];       // 当前用户卡 UID
    u8            user_name[16];// 用户名（服务器返回）
    u32           balance;      // 余额（单位：分），例：100 = 1.00 元
    u32           used_seconds; // 已用上机时间（秒）
    u8            pc_on;        // 电脑继电器状态
    u8            light_on;     // 灯状态
    u8            maint_mode;   // 0=正常，1=维护中（禁止上机 & 不触发占座报警）
} app_ctx_t;

//----------------- 全局变量 -----------------
static app_ctx_t      g_app;
static screen_t       g_screen      = SCREEN_WELCOME;
static confirm_type_t g_confirm     = CONFIRM_NONE;
static u8             g_touch_down  = 0;     // 触摸状态（用来识别按下）

// 时间相关（基于 SysTick 的真实毫秒）
static u32 g_last_1s   = 0;   // 上一次执行 1 秒任务的时间点（ms）
static u32 g_last_mqtt = 0;   // 上一次通过 MQTT 上报 state 的时间点（ms）

// ★ 欢迎界面占座计时相关
static u32 g_idle_human_seconds = 0;   // 欢迎界面下，有人连续坐着的秒数
static u8  g_idle_occupy_alarm  = 0;   // 是否已经触发“占座报警”


// 服务器短消息显示剩余秒数（0 表示不显示）
static u8 g_server_msg_secs = 0;



// 等待服务器刷卡验证
static u8  g_wait_card_auth        = 0;
static u32 g_card_auth_tick        = 0;

// 上机后“无人计时”（用于 15 分钟无人自动下机）
static u32 g_inuse_nohuman_seconds = 0;







//----------------- 静态函数声明 -----------------
static void App_InitContext(void);
static void UI_DrawWelcomeStatic(void);
static void UI_DrawInuseStatic(void);
static void UI_UpdateEnvWelcome(u8 human, u8 smoke_percent);
static void UI_UpdateEnvInuse(u8 human, u8 smoke_percent);
static void UI_UpdateStateLine(void);
static void UI_UpdateUserLine(void);
static void UI_UpdateCardLine(void);
static void UI_UpdateBalanceLine(void);
static void UI_UpdateRuntimeAndFee(void);
static void UI_UpdatePCButton(void);
static void UI_UpdateLightButton(void);
static void UI_UpdateWifiIcon(void);
static u8   Smoke_AdcToPercent(u16 adc);
static u8*  Smoke_LevelText(u8 percent);
static void App_OnCard(u8 *uid);
static void App_EndSession(void);
static void UI_ShowConfirmDialog(confirm_type_t type);
static void App_HandleTouch(void);
static void Netbar_Publish_SeatState(void);      // MQTT 上报函数
static void App_Task_1s(void);                   // 每 1 秒执行的任务

// ★ 在这里加这两行原型
static void UI_ShowServerMsg(const char *msg);
static void UI_ClearServerMsg(void);



static void UID_ToHex(const u8 *uid, char *out);



// Netbar_Publish_SeatState 的变量，用来显示串口输出了多少条
static u32 publish_seq = 0;

//软复位函数
static void App_SoftReset(void);

//================================================
//                  主函数
//================================================
int main(void)
{
    u8  font_res;
    u8  human;
    u16 smoke_adc;
    u8  smoke_percent;
    u8  uid[4] = {0};

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    delay_init(168);
    uart_init(115200);

    Seat_BSP_Init();      // 雷达、烟雾、继电器、蜂鸣器
    RFID_AppInit();       // RC522

    LCD_Init();
    LCD_Display_Dir(0);   // 竖屏
    LCD_Clear(WHITE);

    font_res = font_init();  // 外部字库
    if (font_res)
    {
        POINT_COLOR = RED;
        LCD_ShowString(10, 10, 300, 16, 16, (u8 *)"Font init error!");
        while (1);
    }

    TP_Init();            // 触摸初始化
    App_InitContext();    // 应用初始状态

    //------------- WiFi 初始化并连 AP ----------------
    ESP8266_Init();                         // 串口+模块
    ESP8266_ConnectAP(WIFI_SSID, WIFI_PASS);// 连接路由器（成功/失败由内部状态保存）

    // 等 1 秒给模块一点缓冲时间
    delay_ms(1000);

    // 尝试配置并连接 MQTT（如果连不上，后续每秒的 ESP8266_MQTT_Task_1s 还会继续尝试）
    ESP8266_MQTT_Config();
    ESP8266_MQTT_Connect();

    // 发一条测试消息到 netbar/seat001/debug
    ESP8266_MQTT_Publish("netbar/seat001/debug", "stm32 boot", 0, 0);

    // 上电后立刻上报一次当前座位状态（欢迎界面）
    Netbar_Publish_SeatState();

    g_screen  = SCREEN_WELCOME;
    g_confirm = CONFIRM_NONE;
    UI_DrawWelcomeStatic();  // 欢迎界面

    // 调试：上电采样一次环境并显示
    human         = Seat_Radar_Get();
    smoke_adc     = Seat_Smoke_GetRaw();
    smoke_percent = Smoke_AdcToPercent(smoke_adc);
    if (g_screen == SCREEN_WELCOME)
    {
        UI_UpdateEnvWelcome(human, smoke_percent);
    }
    else
    {
        UI_UpdateEnvInuse(human, smoke_percent);
    }

    while (1)
    {
        // 真实时间轴，单位 ms
        u32 now = GetSysMs();

        // 上报间隔（ms）：你想 1 秒一条就设 1000；想 0.5 秒一条就设 500
        const u32 MQTT_PUB_INTERVAL_MS = 1000;

        // 1) 触摸扫描 & 按钮处理
        tp_dev.scan(0);
        App_HandleTouch();

        // 2) 欢迎界面刷卡进场
        if ((g_screen == SCREEN_WELCOME) && (g_app.state == STATE_IDLE))
        {
            if (RFID_CheckCard(uid))
            {
                App_OnCard(uid);    // 刷卡成功后进入上机界面
            }
        }

        // 3) 基于毫秒计数的定时任务调度

        // 3.1 每 1 秒执行一次的任务（计时 / 环境 / WiFi 心跳等）
        if (now - g_last_1s >= 1000)
        {
            g_last_1s += 1000;      // 下次 1 秒点
            App_Task_1s();
        }

        // 3.2 每 MQTT_PUB_INTERVAL_MS 通过 MQTT 上报一次当前座位状态
        if (ESP8266_GetState() == WIFI_STATE_CONNECTED)
        {
            if (now - g_last_mqtt >= MQTT_PUB_INTERVAL_MS)
            {
                g_last_mqtt += MQTT_PUB_INTERVAL_MS;
                Netbar_Publish_SeatState();
            }
        }

        // 4) 主循环节奏（可选）：保留一个小 delay，减轻 CPU 占用
        delay_ms(10);
    }
}









// 在当前屏幕底部显示服务器短消息
static void UI_ShowServerMsg(const char *msg)
{
    u16 x = 10;
    u16 y = lcddev.height - 24;   // 底部一行
    u16 w = lcddev.width - 20;
    u16 h = 20;

    // 灰底
    LCD_Fill(x, y, x + w, y + h, GRAY);
    POINT_COLOR = BLACK;
    BACK_COLOR  = GRAY;

    Show_Str(x + 4, y + 2, w - 8, 16, (u8 *)msg, 16, 0);
}

// 清除短消息区域（根据当前 screen 重画对应背景）
static void UI_ClearServerMsg(void)
{
    u16 x = 10;
    u16 y = lcddev.height - 24;
    u16 w = lcddev.width - 20;
    u16 h = 20;

    // 欢迎界面：用白色清掉
    // 上机界面：用上机背景色清掉（这里简单用 WHITE，也可以用你之前的底色）
    LCD_Fill(x, y, x + w, y + h, WHITE);

    // 如有需要，可以在这里根据 g_screen 重新画底部原来的内容
}



















// 每 1 秒执行一次的任务：计时、环境采集、UI 更新、WiFi 维护
static void App_Task_1s(void)
{
    u8  human;
    u16 smoke_adc;
    u8  smoke_percent;

    //====================================================
    //  0. 处理来自服务器的远程控制命令
    //====================================================
    ESP8266_CheckRemoteCmd();   // 解析 "+MQTTSUBRECV" 并置标志

    // 0.1 远程软复位（优先级最高）
    if (esp8266_remote_reset_flag)
    {
        esp8266_remote_reset_flag = 0;
        App_SoftReset();        // 你之前已经实现的软复位函数
        return;                 // 本秒剩余逻辑不再执行
    }

    // 0.2 远程 PC 开关机
    if (esp8266_remote_pc_on_flag)
    {
        esp8266_remote_pc_on_flag = 0;

        g_app.pc_on = 1;
        Seat_PC_Set(1);

        if (g_screen == SCREEN_INUSE)
        {
            UI_UpdatePCButton();
        }

        Netbar_Publish_SeatState();
    }
    if (esp8266_remote_pc_off_flag)
    {
        esp8266_remote_pc_off_flag = 0;

        g_app.pc_on = 0;
        Seat_PC_Set(0);

        if (g_screen == SCREEN_INUSE)
        {
            UI_UpdatePCButton();
        }

        Netbar_Publish_SeatState();
    }

    // 0.3 远程灯光控制
    if (esp8266_remote_light_on_flag)
    {
        esp8266_remote_light_on_flag = 0;

        g_app.light_on = 1;
        Seat_Light_Set(1);

        if (g_screen == SCREEN_INUSE)
        {
            UI_UpdateLightButton();
        }

        Netbar_Publish_SeatState();
    }
    if (esp8266_remote_light_off_flag)
    {
        esp8266_remote_light_off_flag = 0;

        g_app.light_on = 0;
        Seat_Light_Set(0);

        if (g_screen == SCREEN_INUSE)
        {
            UI_UpdateLightButton();
        }

        Netbar_Publish_SeatState();
    }

    // 0.4 远程下机结算（只在上机状态有效）
    if (esp8266_remote_checkout_flag)
    {
        esp8266_remote_checkout_flag = 0;

        if (g_app.state == STATE_INUSE)
        {
            App_EndSession();       // 内部已经会回欢迎界面 + 上报
            Netbar_Publish_SeatState();
        }
    }

    // 0.5 远程短消息：显示 5 秒
    if (esp8266_remote_msg_flag)
    {
        esp8266_remote_msg_flag = 0;
        g_server_msg_secs       = 5;      // 显示 5 秒
        UI_ShowServerMsg(esp8266_remote_msg);
    }

    // 0.6 远程维护模式开关
    if (esp8266_remote_maint_on_flag)
    {
        esp8266_remote_maint_on_flag = 0;
        g_app.maint_mode             = 1;

        // 如果在欢迎界面，重新画一遍，让提示变成“维护中”
        if (g_screen == SCREEN_WELCOME)
        {
            UI_DrawWelcomeStatic();
        }

        UI_ShowServerMsg("座位已设为维护中");
        g_server_msg_secs = 5;
    }
    if (esp8266_remote_maint_off_flag)
    {
        esp8266_remote_maint_off_flag = 0;
        g_app.maint_mode              = 0;

        if (g_screen == SCREEN_WELCOME)
        {
            UI_DrawWelcomeStatic();
        }

        UI_ShowServerMsg("维护结束");
        g_server_msg_secs = 5;
    }

    // 0.7 服务器刷卡验证结果：成功
    if (esp8266_remote_card_ok_flag)
    {
        esp8266_remote_card_ok_flag = 0;
        g_wait_card_auth            = 0;

        // 开始本次上机
        g_app.state        = STATE_INUSE;
        g_app.used_seconds = 0;
        // 这里简单处理：余额由服务器维护，终端只显示；真正扣费在服务器
        // 如果以后要从 MQTT 里解析 balance，可以在 esp8266.c 里加一个缓冲传过来

        // 默认自动开电脑+灯光
        g_app.pc_on    = 1;
        g_app.light_on = 1;
        Seat_PC_Set(1);
        Seat_Light_Set(1);

        g_screen  = SCREEN_INUSE;
        g_confirm = CONFIRM_NONE;

        UI_DrawInuseStatic();
        UI_UpdateStateLine();
        UI_UpdateUserLine();
        UI_UpdateCardLine();
        UI_UpdateBalanceLine();
        UI_UpdateRuntimeAndFee();
        UI_UpdatePCButton();
        UI_UpdateLightButton();

        UI_ShowServerMsg("刷卡成功，开始计费");
        g_server_msg_secs = 3;
    }

    // 0.8 服务器刷卡验证结果：失败（无效卡 / 未成年 / 余额不足）
    if (esp8266_remote_card_err_flag)
    {
        esp8266_remote_card_err_flag = 0;
        g_wait_card_auth             = 0;

        if (esp8266_card_err_msg[0] != 0)
        {
            UI_ShowServerMsg(esp8266_card_err_msg);
        }
        else
        {
            UI_ShowServerMsg("刷卡失败");
        }
        g_server_msg_secs = 5;

        // 刷卡失败，不进入上机状态，清掉当前 UID
        g_app.has_user = 0;
    }

    // 0.9 刷卡验证超时（5 秒）
    if (g_wait_card_auth)
    {
        if (GetSysMs() - g_card_auth_tick >= 5000)
        {
            g_wait_card_auth = 0;
            UI_ShowServerMsg("验证超时，请重刷");
            g_server_msg_secs = 3;
        }
    }

    // 每秒递减一次短消息计时，到 0 时清掉
    if (g_server_msg_secs > 0)
    {
        g_server_msg_secs--;
        if (g_server_msg_secs == 0)
        {
            UI_ClearServerMsg();
        }
    }

    //====================================================
    //  1. 上机状态下累计时长 & 刷时长/费用
    //====================================================
    if ((g_app.state == STATE_INUSE) && (g_screen == SCREEN_INUSE))
    {
        g_app.used_seconds++;
        UI_UpdateRuntimeAndFee();   // 刷新“时长 / 费用”两行字
    }

    //====================================================
    //  2. 采样雷达和烟雾
    //====================================================
    human         = Seat_Radar_Get();
    smoke_adc     = Seat_Smoke_GetRaw();
    smoke_percent = Smoke_AdcToPercent(smoke_adc);

    // 2.1 欢迎界面“占座超过 2 分钟”检测（维护中不检测）
    if ((g_screen == SCREEN_WELCOME) &&
        (g_app.state == STATE_IDLE) &&
        (g_app.maint_mode == 0))
    {
        if (human)
        {
            if (g_idle_human_seconds < 0xFFFFFFFF)
            {
                g_idle_human_seconds++;
            }

            // 连续有人坐 >= 120 秒，判定为占座，只触发一次报警
            if ((g_idle_human_seconds >= 15) && (g_idle_occupy_alarm == 0))
            {
                g_idle_occupy_alarm = 1;

                // 通过 MQTT 通知服务器：占座超过两分钟
                if (ESP8266_GetState() == WIFI_STATE_CONNECTED)
                {
                    ESP8266_MQTT_Publish("netbar/seat001/alert",
                                         "occupy_over_120s",
                                         0,
                                         0);
                }
            }
        }
        else
        {
            // 人走了，清零计时和标志
            g_idle_human_seconds = 0;
            g_idle_occupy_alarm  = 0;
        }
    }
    else
    {
        // 非欢迎/非空闲状态，不算占座
        g_idle_human_seconds = 0;
        g_idle_occupy_alarm  = 0;
    }

    // 2.2 上机后“无人超过 15 分钟”自动下机结算
    if (g_app.state == STATE_INUSE)
    {
        if (human)
        {
            g_inuse_nohuman_seconds = 0;
        }
        else
        {
            if (g_inuse_nohuman_seconds < 0xFFFFFFFF)
            {
                g_inuse_nohuman_seconds++;
            }

            if (g_inuse_nohuman_seconds >= 15 * 60)   // 15 分钟
            {
                // 通知服务器：无人在座位，自动下机
                if (ESP8266_GetState() == WIFI_STATE_CONNECTED)
                {
                    ESP8266_MQTT_Publish("netbar/seat001/alert",
                                         "auto_checkout_nohuman_15min",
                                         0,
                                         0);
                }

                App_EndSession();
                Netbar_Publish_SeatState();

                g_inuse_nohuman_seconds = 0;
            }
        }
    }
    else
    {
        g_inuse_nohuman_seconds = 0;
    }

    //====================================================
    //  3. 更新环境显示（根据当前界面）
    //====================================================
    if (g_screen == SCREEN_WELCOME)
    {
        UI_UpdateEnvWelcome(human, smoke_percent);
    }
    else
    {
        UI_UpdateEnvInuse(human, smoke_percent);
    }

    // 4) WiFi 图标刷新
    UI_UpdateWifiIcon();

    // 5) 每秒维护一次 WiFi + MQTT 连接
    ESP8266_Task_1s();
}

//================================================
//             应用层 & UI 实现
//================================================

// 初始化业务上下文
static void App_InitContext(void)
{
    g_app.state        = STATE_IDLE;
    g_app.has_user     = 0;
    g_app.uid[0]       = 0;
    g_app.uid[1]       = 0;
    g_app.uid[2]       = 0;
    g_app.uid[3]       = 0;
    strcpy((char *)g_app.user_name, "--");
    g_app.balance      = 0;        // 单位：分
    g_app.used_seconds = 0;
    g_app.pc_on        = 0;
    g_app.light_on     = 0;
    g_app.maint_mode   = 0;        // 默认非维护状态

    Seat_PC_Set(0);
    Seat_Light_Set(0);
    Seat_Buzzer_Set(0);

    // 占座 / 无人 等计数清零
    g_idle_human_seconds    = 0;
    g_idle_occupy_alarm     = 0;
    g_inuse_nohuman_seconds = 0;
    g_wait_card_auth        = 0;
    g_server_msg_secs       = 0;
}

//================================================
//             应用层“软复位”：回到上电初始状态
//================================================
static void App_SoftReset(void)
{
    printf("APP: soft reset start...\r\n");

    // 1. 重置应用上下文（状态机清空、计时清零）
    App_InitContext();
    g_screen        = SCREEN_WELCOME;
    g_confirm       = CONFIRM_NONE;
    //g_loop_cnt      = 0;
    //g_mqtt_sync_cnt = 0;

    // 2. 重新画欢迎界面
    LCD_Clear(WHITE);
    UI_DrawWelcomeStatic();

    // 3. 重新初始化 ESP8266（相当于 AT+RST + 基本配置）
    ESP8266_Init();

    // 4. 重新连 WiFi
    ESP8266_ConnectAP(WIFI_SSID, WIFI_PASS);
    // 简单等一等，给模块连 AP 的时间（也可以依赖 ESP8266_Task_1s 自动维护）
    delay_ms(1000);

    // 5. 重新配置 MQTT 用户（MQTTUSERCFG）
    ESP8266_MQTT_Config();

    // 6. ★ 关键：重新连接 MQTT Broker（AT+MQTTCONN）
    if (ESP8266_MQTT_Connect() == ESP8266_OK)
    {
        printf("APP: MQTT reconnect ok\r\n");
    }
    else
    {
        printf("APP: MQTT reconnect FAIL\r\n");
    }

    // 7. 启动时主动上报一次当前状态，方便网页端同步显示
    Netbar_Publish_SeatState();

    printf("APP: soft reset done.\r\n");
}


// 欢迎界面：标题 + 费率 + 座位/烟雾 + “请刷卡上机”
static void UI_DrawWelcomeStatic(void)
{
    LCD_Clear(WHITE);

    // 顶部蓝色标题栏
    LCD_Fill(0, 0, lcddev.width - 1, 30, BLUE);
    POINT_COLOR = WHITE;
    BACK_COLOR  = BLUE;
    Show_Str(10, 5, lcddev.width - 90, 24, (u8 *)"智能无人网吧系统", 24, 0); // 给右侧留空间画 WiFi

    UI_UpdateWifiIcon();   // 右上角 WiFi 图标

    // 副标题 + 费率
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    Show_Str(10, 40, lcddev.width - 20, 16, (u8 *)"欢迎使用", 16, 0);
    Show_Str(10, 60, lcddev.width - 20, 16, (u8 *)"费率: 1元/分钟(测试)", 16, 0);

    // 座位/烟雾标签
    Show_Str(10, 90,  80, 16, (u8 *)"座位:", 16, 0);
    Show_Str(10, 110, 80, 16, (u8 *)"烟雾:", 16, 0);

    // 数值区域先清空
    LCD_Fill(70, 90,  lcddev.width - 10, 106, WHITE);
    LCD_Fill(70, 110, lcddev.width - 10, 126, WHITE);

    // 底部提示条：正常=请刷卡上机；维护中=维护中，请勿上机
    LCD_Fill(10, 150, lcddev.width - 10, 190, GRAY);
    POINT_COLOR = BLUE;
    BACK_COLOR  = GRAY;
    if (g_app.maint_mode)
    {
        Show_Str(20, 160, lcddev.width - 40, 16, (u8 *)"维护中，请勿上机", 16, 0);
    }
    else
    {
        Show_Str(20, 160, lcddev.width - 40, 16, (u8 *)"请刷卡上机", 16, 0);
    }

    // 底部再显示一次费率
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    Show_Str(10, 210, lcddev.width - 20, 16, (u8 *)"费率:1元/分钟", 16, 0);

    // 提示信息预留
    LCD_Fill(10, 230, lcddev.width - 10, 250, WHITE);
}

// 上机界面：标题 + 用户/卡号/余额/时长/费用 + 按钮
static void UI_DrawInuseStatic(void)
{
    LCD_Clear(WHITE);

    // 顶部蓝色标题栏
    LCD_Fill(0, 0, lcddev.width - 1, 30, BLUE);
    POINT_COLOR = WHITE;
    BACK_COLOR  = BLUE;
    Show_Str(10, 5, lcddev.width - 90, 24, (u8 *)"上机中", 24, 0); // 右侧留空间画 WiFi

    UI_UpdateWifiIcon();

    // 费率
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    Show_Str(10, 32, lcddev.width - 20, 16, (u8 *)"费率: 1元/分钟(测试)", 16, 0);

    // 标签：状态/用户/卡号/余额/时长/费用
    Show_Str(10, 50,  70, 16, (u8 *)"状态:", 16, 0);
    Show_Str(10, 70,  70, 16, (u8 *)"用户:", 16, 0);
    Show_Str(10, 90,  70, 16, (u8 *)"卡号:", 16, 0);
    Show_Str(10, 110, 70, 16, (u8 *)"余额:", 16, 0);
    Show_Str(10, 130, 70, 16, (u8 *)"时长:", 16, 0);
    Show_Str(10, 150, 70, 16, (u8 *)"费用:", 16, 0);

    // 按钮边框
    LCD_DrawRectangle(BTN_PC_X1, BTN_PC_Y1, BTN_PC_X2, BTN_PC_Y2);             // 电脑
    LCD_DrawRectangle(BTN_LIGHT_X1, BTN_LIGHT_Y1, BTN_LIGHT_X2, BTN_LIGHT_Y2); // 灯光
    LCD_DrawRectangle(BTN_EXIT_X1, BTN_EXIT_Y1, BTN_EXIT_X2, BTN_EXIT_Y2);     // 下机结算

    // 底部环境信息标签
    Show_Str(10, 280, 80, 16, (u8 *)"座位:", 16, 0);
    Show_Str(10, 300, 80, 16, (u8 *)"烟雾:", 16, 0);
    Show_Str(10, 320, 120, 16, (u8 *)"费率:1元/分钟", 16, 0);

    // 清空环境数值区域
    LCD_Fill(70, 280, lcddev.width - 10, 336, WHITE);
}

// 欢迎界面更新座位 / 烟雾
static void UI_UpdateEnvWelcome(u8 human, u8 smoke_percent)
{
    char buf[16];
    u8  *level;

    //---------------- 座位状态：有人 / 无人 ----------------
    LCD_Fill(70, 90, lcddev.width - 10, 106, WHITE);

    if (human)
    {
        POINT_COLOR = RED;
        Show_Str(70, 90, lcddev.width - 80, 16, (u8 *)"有人", 16, 0);
    }
    else
    {
        POINT_COLOR = BLACK;
        Show_Str(70, 90, lcddev.width - 80, 16, (u8 *)"无人", 16, 0);
    }

    //---------------- 烟雾百分比 + 等级文字 ----------------
    LCD_Fill(70, 110, lcddev.width - 10, 126, WHITE);

    sprintf(buf, "%3d%%", smoke_percent);
    POINT_COLOR = BLACK;
    LCD_ShowString(70, 110, 80, 16, 16, (u8 *)buf);

    level = Smoke_LevelText(smoke_percent);
    Show_Str(150, 110, 100, 16, level, 16, 0);

    //---------------- 报警逻辑（欢迎界面） ----------------
    // 烟雾超阈值 或 被判定为占座，都要报警
    if ((smoke_percent >= 60) || (g_idle_occupy_alarm))
    {
        Seat_Buzzer_Set(1);
    }
    else
    {
        Seat_Buzzer_Set(0);
    }

    POINT_COLOR = BLACK;
}

// 上机界面更新座位 / 烟雾
static void UI_UpdateEnvInuse(u8 human, u8 smoke_percent)
{
    char buf[16];
    u8  *level;

    //---------------- 座位状态：有人 / 无人 ----------------
    LCD_Fill(70, 280, lcddev.width - 10, 296, WHITE);

    if (human)
    {
        POINT_COLOR = RED;
        Show_Str(70, 280, lcddev.width - 80, 16, (u8 *)"有人", 16, 0);
    }
    else
    {
        POINT_COLOR = BLACK;
        Show_Str(70, 280, lcddev.width - 80, 16, (u8 *)"无人", 16, 0);
    }

    //---------------- 烟雾百分比 + 等级文字 ----------------
    LCD_Fill(70, 300, lcddev.width - 10, 316, WHITE);

    sprintf(buf, "%3d%%", smoke_percent);
    POINT_COLOR = BLACK;
    LCD_ShowString(70, 300, 80, 16, 16, (u8 *)buf);

    level = Smoke_LevelText(smoke_percent);
    Show_Str(150, 300, 100, 16, level, 16, 0);

    //---------------- 报警逻辑（上机界面） ----------------
    if (smoke_percent >= 60)
    {
        Seat_Buzzer_Set(1);
    }
    else
    {
        Seat_Buzzer_Set(0);
    }

    POINT_COLOR = BLACK;
}

// 状态行：待机 / 上机中
static void UI_UpdateStateLine(void)
{
    u8 *text;

    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    LCD_Fill(80, 50, 310, 66, WHITE);

    if (g_app.state == STATE_IDLE)
    {
        text = (u8 *)"待机";
    }
    else
    {
        text = (u8 *)"上机中";
    }
    Show_Str(80, 50, 230, 16, text, 16, 0);
}

// 用户名行
static void UI_UpdateUserLine(void)
{
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    LCD_Fill(80, 70, 310, 86, WHITE);

    if (g_app.state == STATE_IDLE || g_app.has_user == 0)
    {
        Show_Str(80, 70, 230, 16, (u8 *)"--", 16, 0);
    }
    else
    {
        Show_Str(80, 70, 230, 16, g_app.user_name, 16, 0);
    }
}

// 卡号行（十六进制显示 UID）
static void UI_UpdateCardLine(void)
{
    char buf[24];

    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    LCD_Fill(80, 90, 310, 106, WHITE);

    if (g_app.has_user)
    {
        sprintf(buf, "%02X %02X %02X %02X", g_app.uid[0], g_app.uid[1], g_app.uid[2], g_app.uid[3]);
        LCD_ShowString(80, 90, 230, 16, 16, (u8 *)buf);
    }
    else
    {
        Show_Str(80, 90, 230, 16, (u8 *)"--", 16, 0);
    }
}

// 余额行（★ 显示两位小数）
static void UI_UpdateBalanceLine(void)
{
    char buf[24];
    u32 yuan;
    u32 cent;

    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    LCD_Fill(80, 110, 310, 126, WHITE);

    // g_app.balance 单位为“分”
    yuan = g_app.balance / 100;
    cent = g_app.balance % 100;

    sprintf(buf, "%lu.%02lu 元", (unsigned long)yuan, (unsigned long)cent);
    Show_Str(80, 110, 230, 16, (u8 *)buf, 16, 0);
}

// 时长 + 费用
static void UI_UpdateRuntimeAndFee(void)
{
    u32 min;
    u32 sec;
    u32 fee;
    char buf[24];

    min = g_app.used_seconds / 60;
    sec = g_app.used_seconds % 60;

    // 时长
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    LCD_Fill(80, 130, 310, 146, WHITE);
    sprintf(buf, "%02lu:%02lu", min, sec);
    Show_Str(80, 130, 230, 16, (u8 *)buf, 16, 0);

    // 费用 = 已用分钟 * 费率（向下取整）
    fee = (g_app.used_seconds / 60) * PRICE_PER_MIN;
    LCD_Fill(80, 150, 310, 166, WHITE);
    sprintf(buf, "%4lu 元", fee);
    Show_Str(80, 150, 230, 16, (u8 *)buf, 16, 0);
}







// 通过 MQTT 上报当前座位状态到服务器，无论是否上机
// Topic: netbar/seat001/state
// Payload: s=1;iu=1;pc=1;lt=0;hm=1;sm=23;sec=120;fee=2
static void Netbar_Publish_SeatState(void)
{
    char payload[96];
    u8  human;
    u16 smoke_adc;
    u8  smoke_percent;
    u8  iu;
    u32 used_seconds;
    u32 fee;

    // WiFi 未连接，直接返回（避免死等）
    if (ESP8266_GetState() != WIFI_STATE_CONNECTED)
    {
        return;
    }

    // 采集当前传感器
    human        = Seat_Radar_Get();
    smoke_adc    = Seat_Smoke_GetRaw();
    smoke_percent= Smoke_AdcToPercent(smoke_adc);

    // 是否正在上机
    iu           = (g_app.state == STATE_INUSE) ? 1 : 0;

    used_seconds = g_app.used_seconds;
    fee          = (used_seconds / 60) * PRICE_PER_MIN;   // 现在是 1 分钟 1 元

    // 组装 payload，注意不要用双引号
    sprintf(payload,
            "s=1;iu=%d;pc=%d;lt=%d;hm=%d;sm=%d;sec=%lu;fee=%lu",
            iu,
            g_app.pc_on,
            g_app.light_on,
            human,
            smoke_percent,
            used_seconds,
            fee);

    // 调试信息（串口能看到）
    publish_seq++;
    printf("STATE哈哈哈 #%lu, payload: %s\r\n", publish_seq, payload);

    // 发布到 netbar/seat001/state
    ESP8266_MQTT_Publish("netbar/seat001/state", payload, 0, 0);
}

// 电脑按钮显示
static void UI_UpdatePCButton(void)
{
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;

    if (g_app.pc_on)
    {
        LCD_Fill(BTN_PC_X1 + 1, BTN_PC_Y1 + 1, BTN_PC_X2 - 1, BTN_PC_Y2 - 1, GREEN);
        Show_Str(BTN_PC_X1 + 20, BTN_PC_Y1 + 12, 100, 16, (u8 *)"关电脑", 16, 0);
    }
    else
    {
        LCD_Fill(BTN_PC_X1 + 1, BTN_PC_Y1 + 1, BTN_PC_X2 - 1, BTN_PC_Y2 - 1, GRAY);
        Show_Str(BTN_PC_X1 + 20, BTN_PC_Y1 + 12, 100, 16, (u8 *)"开电脑", 16, 0);
    }
    LCD_DrawRectangle(BTN_PC_X1, BTN_PC_Y1, BTN_PC_X2, BTN_PC_Y2);
}

// 灯光按钮显示
static void UI_UpdateLightButton(void)
{
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;

    if (g_app.light_on)
    {
        LCD_Fill(BTN_LIGHT_X1 + 1, BTN_LIGHT_Y1 + 1, BTN_LIGHT_X2 - 1, BTN_LIGHT_Y2 - 1, GREEN);
        Show_Str(BTN_LIGHT_X1 + 20, BTN_LIGHT_Y1 + 12, 100, 16, (u8 *)"关灯光", 16, 0);
    }
    else
    {
        LCD_Fill(BTN_LIGHT_X1 + 1, BTN_LIGHT_Y1 + 1, BTN_LIGHT_X2 - 1, BTN_LIGHT_Y2 - 1, GRAY);
        Show_Str(BTN_LIGHT_X1 + 20, BTN_LIGHT_Y1 + 12, 100, 16, (u8 *)"开灯光", 16, 0);
    }
    LCD_DrawRectangle(BTN_LIGHT_X1, BTN_LIGHT_Y1, BTN_LIGHT_X2, BTN_LIGHT_Y2);

    // 下机结算按钮固定为红色
    LCD_Fill(BTN_EXIT_X1 + 1, BTN_EXIT_Y1 + 1, BTN_EXIT_X2 - 1, BTN_EXIT_Y2 - 1, RED);
    POINT_COLOR = WHITE;
    BACK_COLOR  = RED;
    Show_Str(BTN_EXIT_X1 + 20, BTN_EXIT_Y1 + 12, 120, 16, (u8 *)"下机结算", 16, 0);
    LCD_DrawRectangle(BTN_EXIT_X1, BTN_EXIT_Y1, BTN_EXIT_X2, BTN_EXIT_Y2);

    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
}

// 顶部 WiFi 状态图标（右上角三格信号条：从左矮到右高）
static void UI_UpdateWifiIcon(void)
{
    WifiState cur;
    u16 base_x;
    u16 base_y;

    cur = ESP8266_GetState();

    base_x = lcddev.width - 40; // 图标左边
    base_y = 6;                 // 底部 y

    // 先清除标题栏右侧区域
    POINT_COLOR = WHITE;
    BACK_COLOR  = BLUE;
    LCD_Fill(lcddev.width - 70, 0, lcddev.width - 1, 30, BLUE);

    // 画“反向”的三格：左矮右高
    if (cur == WIFI_STATE_CONNECTED)
    {
        // 三格全满
        LCD_Fill(base_x,     base_y + 8,  base_x + 6, base_y + 14, WHITE); // 矮
        LCD_Fill(base_x + 8, base_y + 4,  base_x +14, base_y + 14, WHITE); // 中
        LCD_Fill(base_x +16, base_y,      base_x +22, base_y + 14, WHITE); // 高
    }
    else if (cur == WIFI_STATE_CONNECTING)
    {
        // 两格
        LCD_Fill(base_x,     base_y + 8,  base_x + 6, base_y + 14, WHITE);
        LCD_Fill(base_x + 8, base_y + 4,  base_x +14, base_y + 14, WHITE);
        LCD_DrawRectangle(base_x +16, base_y, base_x +22, base_y + 14);
    }
    else if (cur == WIFI_STATE_DISCONNECTED)
    {
        // 一格亮，两格空
        LCD_Fill(base_x,     base_y + 8,  base_x + 6, base_y + 14, WHITE);
        LCD_DrawRectangle(base_x + 8, base_y + 4, base_x +14, base_y + 14);
        LCD_DrawRectangle(base_x +16, base_y,      base_x +22, base_y + 14);
    }
    else
    {
        // 错误/未初始化：画一个小 X
        POINT_COLOR = WHITE;
        LCD_DrawLine(base_x,     base_y,     base_x + 15, base_y + 15);
        LCD_DrawLine(base_x,     base_y +15, base_x + 15, base_y);
    }

    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
}

// ADC 转烟雾百分比
static u8 Smoke_AdcToPercent(u16 adc)
{
    u8 p;

    if (adc <= 500)   return 0;
    if (adc >= 3500)  return 100;

    p = (u8)((adc - 500) * 100 / (3500 - 500));
    return p;
}

// 根据百分比返回等级文本
static u8* Smoke_LevelText(u8 percent)
{
    if (percent < 30)
        return (u8 *)"良好";
    else if (percent < 60)
        return (u8 *)"中等";
    else if (percent < 80)
        return (u8 *)"较高";
    else
        return (u8 *)"危险";
}

// 把 UID 转成 8 位十六进制字符串：例如 53 94 AF F7 -> "5394AFF7"
static void UID_ToHex(const u8 *uid, char *out)
{
    sprintf(out, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
}






// 刷卡：把 UID 发给服务器，由服务器决定是否允许上机
static void App_OnCard(u8 *uid)
{
    u8  i;
    char uid_hex[9];
    char payload[32];

    // 维护模式：禁止上机
    if (g_app.maint_mode)
    {
        UI_ShowServerMsg("座位维护中，暂不提供上机服务");
        g_server_msg_secs = 5;
        return;
    }

    // 这里暂时不做“二次刷卡下机”，如果以后要加，可以在 STATE_INUSE 时判断
    if (g_app.state == STATE_INUSE)
    {
        UI_ShowServerMsg("已在上机中");
        g_server_msg_secs = 3;
        return;
    }

    // 记录 UID，方便上机界面显示卡号
    for (i = 0; i < 4; i++)
    {
        g_app.uid[i] = uid[i];
    }
    g_app.has_user = 1;

    UID_ToHex(uid, uid_hex);
    sprintf(payload, "uid=%s", uid_hex);

    // 通过 MQTT 把 UID 发给服务器：netbar/seat001/card
    if (ESP8266_GetState() == WIFI_STATE_CONNECTED)
    {
        ESP8266_MQTT_Publish("netbar/seat001/card", payload, 0, 0);
        g_wait_card_auth = 1;
        g_card_auth_tick = GetSysMs();
        UI_ShowServerMsg("正在验证，请稍候...");
        g_server_msg_secs = 5;
    }
    else
    {
        UI_ShowServerMsg("网络未连接，无法验证");
        g_server_msg_secs = 5;
    }
}

// 结束本次上机：回到欢迎界面 + 立刻上报一次空闲状态
static void App_EndSession(void)
{
    App_InitContext();   // 回到空闲状态

    // 回到欢迎界面
    g_screen  = SCREEN_WELCOME;
    g_confirm = CONFIRM_NONE;
    UI_DrawWelcomeStatic();

    // 主动上报一帧 “空闲状态”
    Netbar_Publish_SeatState();
}

// 显示确认弹窗
static void UI_ShowConfirmDialog(confirm_type_t type)
{
    LCD_Fill(CONF_X1, CONF_Y1, CONF_X2, CONF_Y2, GRAY);
    LCD_DrawRectangle(CONF_X1, CONF_Y1, CONF_X2, CONF_Y2);

    POINT_COLOR = BLACK;
    BACK_COLOR  = GRAY;

    if (type == CONFIRM_PC_OFF)
    {
        Show_Str(CONF_X1 + 20, CONF_Y1 + 20, 220, 16, (u8 *)"确认关机？", 16, 0);
    }
    else if (type == CONFIRM_EXIT)
    {
        Show_Str(CONF_X1 + 20, CONF_Y1 + 20, 220, 16, (u8 *)"确认下机结算？", 16, 0);
    }

    // “确认”按钮
    LCD_Fill(CONF_BTN_OK_X1, CONF_BTN_OK_Y1, CONF_BTN_OK_X2, CONF_BTN_OK_Y2, GREEN);
    POINT_COLOR = WHITE;
    BACK_COLOR  = GREEN;
    Show_Str(CONF_BTN_OK_X1 + 8, CONF_BTN_OK_Y1 + 8, 80, 16, (u8 *)"确认", 16, 0);
    LCD_DrawRectangle(CONF_BTN_OK_X1, CONF_BTN_OK_Y1, CONF_BTN_OK_X2, CONF_BTN_OK_Y2);

    // “取消”按钮
    LCD_Fill(CONF_BTN_CANCEL_X1, CONF_BTN_CANCEL_Y1, CONF_BTN_CANCEL_X2, CONF_BTN_CANCEL_Y2, RED);
    POINT_COLOR = WHITE;
    BACK_COLOR  = RED;
    Show_Str(CONF_BTN_CANCEL_X1 + 8, CONF_BTN_CANCEL_Y1 + 8, 80, 16, (u8 *)"取消", 16, 0);
    LCD_DrawRectangle(CONF_BTN_CANCEL_X1, CONF_BTN_CANCEL_Y1, CONF_BTN_CANCEL_X2, CONF_BTN_CANCEL_Y2);

    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
}

// 触摸处理：欢迎界面 / 上机界面 / 确认弹窗
static void App_HandleTouch(void)
{
    u8  is_down;
    u16 x;
    u16 y;

    // 获取一次触摸状态
    if (tp_dev.scan(0))  // 有事件
    {
        if (tp_dev.sta & TP_PRES_DOWN)
        {
            is_down = 1;
        }
        else
        {
            is_down = 0;
        }

        x = tp_dev.x[0];
        y = tp_dev.y[0];
    }
    else
    {
        is_down = 0;
        x       = 0;
        y       = 0;
    }

    // 边沿检测：只在“刚按下”的瞬间处理一次
    if (is_down)
    {
        if (!g_touch_down)
        {
            g_touch_down = 1;

            if (g_screen == SCREEN_INUSE)
            {
                // 电脑按钮
                if ((x > BTN_PC_X1) && (x < BTN_PC_X2) && (y > BTN_PC_Y1) && (y < BTN_PC_Y2))
                {
                    // 当前是关机 -> 直接开机
                    if (!g_app.pc_on)
                    {
                        g_app.pc_on = 1;
                        Seat_PC_Set(1);
                        UI_UpdatePCButton();
                    }
                    // 当前是开机 -> 弹出“确认关机”
                    else
                    {
                        g_confirm = CONFIRM_PC_OFF;
                        g_screen  = SCREEN_CONFIRM;
                        UI_ShowConfirmDialog(g_confirm);
                    }
                }
                // 灯光按钮（不强制确认，直接切换）
                else if ((x > BTN_LIGHT_X1) && (x < BTN_LIGHT_X2) && (y > BTN_LIGHT_Y1) && (y < BTN_LIGHT_Y2))
                {
                    g_app.light_on = g_app.light_on ? 0 : 1;
                    Seat_Light_Set(g_app.light_on);
                    UI_UpdateLightButton();
                }
                // 下机结算按钮 -> 弹出确认
                else if ((x > BTN_EXIT_X1) && (x < BTN_EXIT_X2) && (y > BTN_EXIT_Y1) && (y < BTN_EXIT_Y2))
                {
                    if (g_app.state == STATE_INUSE)
                    {
                        g_confirm = CONFIRM_EXIT;
                        g_screen  = SCREEN_CONFIRM;
                        UI_ShowConfirmDialog(g_confirm);
                    }
                }
            }
            else if (g_screen == SCREEN_CONFIRM)
            {
                // 在确认弹窗里判断按的是确认还是取消
                // 确认
                if ((x > CONF_BTN_OK_X1) && (x < CONF_BTN_OK_X2) &&
                    (y > CONF_BTN_OK_Y1) && (y < CONF_BTN_OK_Y2))
                {
                    if (g_confirm == CONFIRM_PC_OFF)
                    {
                        g_app.pc_on = 0;
                        Seat_PC_Set(0);

                        // 返回上机界面，重新绘制上机界面
                        g_screen  = SCREEN_INUSE;
                        g_confirm = CONFIRM_NONE;
                        UI_DrawInuseStatic();
                        UI_UpdateStateLine();
                        UI_UpdateUserLine();
                        UI_UpdateCardLine();
                        UI_UpdateBalanceLine();
                        UI_UpdateRuntimeAndFee();
                        UI_UpdatePCButton();
                        UI_UpdateLightButton();
                    }
                    else if (g_confirm == CONFIRM_EXIT)
                    {
                        // 下机结算：回到欢迎界面
                        App_EndSession();
                    }
                }
                // 取消
                else if ((x > CONF_BTN_CANCEL_X1) && (x < CONF_BTN_CANCEL_X2) &&
                         (y > CONF_BTN_CANCEL_Y1) && (y < CONF_BTN_CANCEL_Y2))
                {
                    // 取消后回到上机界面
                    g_screen  = SCREEN_INUSE;
                    g_confirm = CONFIRM_NONE;
                    UI_DrawInuseStatic();
                    UI_UpdateStateLine();
                    UI_UpdateUserLine();
                    UI_UpdateCardLine();
                    UI_UpdateBalanceLine();
                    UI_UpdateRuntimeAndFee();
                    UI_UpdatePCButton();
                    UI_UpdateLightButton();
                }
            }
            // 欢迎界面目前没触摸按钮，“请刷卡上机”只靠刷卡触发
        }
    }
    else
    {
        g_touch_down = 0;
    }
}
