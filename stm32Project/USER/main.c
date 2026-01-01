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

// --- UI 坐标定义 ---
void LCD_Display_Dir(u8 dir);  
#define PRICE_PER_MIN       1       
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

// --- 类型定义 ---
typedef enum { STATE_IDLE = 0, STATE_INUSE } app_state_t;
typedef enum { SCREEN_WELCOME = 0, SCREEN_INUSE, SCREEN_CONFIRM, SCREEN_MAINTENANCE } screen_t;
typedef enum { CONFIRM_NONE = 0, CONFIRM_PC_OFF, CONFIRM_EXIT } confirm_type_t;

typedef struct {
    app_state_t   state;        
    u8            has_user;     
    u8            uid[4];       
    u8            user_name[32]; 
    u32           balance;      
    u32           used_seconds; 
    u8            pc_on;        
    u8            light_on;     
    u8            maint_mode;   
} app_ctx_t;

// --- 全局变量 ---
static app_ctx_t      g_app;
static screen_t       g_screen      = SCREEN_WELCOME;
static confirm_type_t g_confirm     = CONFIRM_NONE;
static u8             g_touch_down  = 0;     
static u32 g_last_1s   = 0;   
static u32 g_last_mqtt = 0;   
static u32 g_idle_human_seconds = 0;   
static u8  g_idle_occupy_alarm  = 0;   
static u8  g_smoke_alarm        = 0;   
static u8  g_server_msg_secs = 0;
static u8  g_wait_card_auth        = 0;
static u32 g_card_auth_tick        = 0;
static u32 g_inuse_nohuman_seconds = 0;
static u32 publish_seq = 0;
static u32 g_card_cooldown_tick    = 0;

// --- 函数声明 ---
static void App_InitContext(void);
static void UI_DrawSplashScreen(void);
static void UI_DrawWelcomeStatic(void);
static void UI_DrawInuseStatic(void);
static void UI_DrawMaintenanceStatic(void); 
static void UI_DrawAlarmWindow(const char* title, const char* msg); 
static void UI_ClearAlarmWindow(void);      
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
static void UI_ShowServerMsg(const char *msg); 
static void UI_ClearServerMsg(void);           
static u8   Smoke_AdcToPercent(u16 adc);
static u8* Smoke_LevelText(u8 percent);
static void App_OnCard(u8 *uid);
static void App_EndSession(void);
static void UI_ShowConfirmDialog(confirm_type_t type);
static void App_HandleTouch(void);
static void Netbar_Publish_SeatState(void);      
static void App_Task_1s(void);                   
static void UID_ToHex(const u8 *uid, char *out);

// --- 主函数 ---
int main(void)
{
    u8  font_res;
    u8  human;
    u16 smoke_adc;
    u8  smoke_percent;
    u8  uid[4] = {0};
    u32 now;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    delay_init(168);
    uart_init(115200);
    Seat_BSP_Init();      
    RFID_AppInit();       
    LCD_Init();
    LCD_Display_Dir(0);   
    LCD_Clear(WHITE);

    // 1. 先初始化字库
    font_res = font_init();  
    if (font_res) {
        LCD_Clear(WHITE);
        LCD_ShowString(10, 10, 300, 16, 16, (u8 *)"Font init error!");
        LCD_ShowString(10, 30, 300, 16, 16, (u8 *)"Please check W25QXX");
        while (1); 
    }

    // 2. 再显示开机画面
    UI_DrawSplashScreen();
    delay_ms(2000); 

    TP_Init();            
    App_InitContext();    
    ESP8266_Init_Async();                         

    g_screen  = SCREEN_WELCOME;
    g_confirm = CONFIRM_NONE;
    UI_DrawWelcomeStatic();  

    human         = Seat_Radar_Get();
    smoke_adc     = Seat_Smoke_GetRaw();
    smoke_percent = Smoke_AdcToPercent(smoke_adc);
    if (g_screen == SCREEN_WELCOME) UI_UpdateEnvWelcome(human, smoke_percent);
    else UI_UpdateEnvInuse(human, smoke_percent);

    g_last_1s = GetSysMs();
    g_last_mqtt = GetSysMs();

    while (1)
    {
        ESP8266_Poll();
        tp_dev.scan(0);
        App_HandleTouch();

        now = GetSysMs();

        // ★★★ 蜂鸣器逻辑改进：急促滴滴声 ★★★
        // 只有在【有报警】且【不在维护模式】时才响
        if ((g_idle_occupy_alarm || g_smoke_alarm) && (g_app.maint_mode == 0)) {
            // 200ms一个周期：100ms响，100ms停 (急促)
            if ((now % 200) < 100) Seat_Buzzer_Set(1);
            else Seat_Buzzer_Set(0);
        } else {
            Seat_Buzzer_Set(0); // 必须静音
        }

        // 维护模式禁止刷卡
        if ((g_screen == SCREEN_WELCOME) && (g_app.state == STATE_IDLE) && (g_app.maint_mode == 0)) {
            if (g_wait_card_auth == 0 && now > g_card_cooldown_tick) {
                if (RFID_CheckCard(uid)) {
                    App_OnCard(uid);
                    g_card_cooldown_tick = now + 3000; 
                }
            }
        }

        if (now - g_last_1s >= 1000) {
            if (now - g_last_1s > 5000) g_last_1s = now;
            else g_last_1s += 1000;
            App_Task_1s();
        }

        if (ESP8266_GetState() == WIFI_STATE_RUNNING) {
            if (now - g_last_mqtt >= 1000) {
                if (now - g_last_mqtt > 5000) g_last_mqtt = now;
                else g_last_mqtt += 1000;
                Netbar_Publish_SeatState();
            }
        }
    }
}

static void App_Task_1s(void)
{
    u8  human;
    u16 smoke_adc;
    u8  smoke_percent;
    float bal_f = 0.0f;

    // 重置逻辑
    if (esp8266_remote_reset_flag) {
        esp8266_remote_reset_flag = 0;
        App_InitContext(); 
        g_screen = SCREEN_WELCOME;
        g_confirm = CONFIRM_NONE;
        UI_DrawWelcomeStatic();
        ESP8266_Init_Async(); 
        UI_ShowServerMsg("系统已重置");
        g_server_msg_secs = 3; 
        return;                 
    }

    // ★★★ 维护模式切换 ★★★
    if (esp8266_remote_maint_on_flag) {
        esp8266_remote_maint_on_flag = 0;
        g_app.maint_mode = 1;
        // 清除所有报警
        g_idle_occupy_alarm = 0;
        g_smoke_alarm = 0;
        g_screen = SCREEN_MAINTENANCE;
        UI_DrawMaintenanceStatic(); // 显示黄色维护窗口
        UI_ShowServerMsg("已进入维护模式");
        g_server_msg_secs = 3;
    }
    if (esp8266_remote_maint_off_flag) {
        esp8266_remote_maint_off_flag = 0;
        g_app.maint_mode = 0;
        g_screen = SCREEN_WELCOME;
        UI_DrawWelcomeStatic();
        UI_ShowServerMsg("维护结束，恢复正常");
        g_server_msg_secs = 3;
    }

    // 远程控制逻辑
    if (esp8266_remote_pc_on_flag) {
        esp8266_remote_pc_on_flag = 0;
        g_app.pc_on = 1; Seat_PC_Set(1);
        if (g_screen == SCREEN_INUSE) UI_UpdatePCButton();
        Netbar_Publish_SeatState();
    }
    if (esp8266_remote_pc_off_flag) {
        esp8266_remote_pc_off_flag = 0;
        g_app.pc_on = 0; Seat_PC_Set(0);
        if (g_screen == SCREEN_INUSE) UI_UpdatePCButton();
        Netbar_Publish_SeatState();
    }
    if (esp8266_remote_light_on_flag) {
        esp8266_remote_light_on_flag = 0;
        g_app.light_on = 1; Seat_Light_Set(1);
        if (g_screen == SCREEN_INUSE) UI_UpdateLightButton();
        Netbar_Publish_SeatState();
    }
    if (esp8266_remote_light_off_flag) {
        esp8266_remote_light_off_flag = 0;
        g_app.light_on = 0; Seat_Light_Set(0);
        if (g_screen == SCREEN_INUSE) UI_UpdateLightButton();
        Netbar_Publish_SeatState();
    }
    if (esp8266_remote_checkout_flag) {
        esp8266_remote_checkout_flag = 0;
        if (g_app.state == STATE_INUSE) {
            App_EndSession();       
        }
    }
    if (esp8266_remote_msg_flag) {
        esp8266_remote_msg_flag = 0;
        g_server_msg_secs       = 5;      
        UI_ShowServerMsg(esp8266_remote_msg);
    }

    // 刷卡反馈处理
    if (esp8266_remote_card_ok_flag) {
        esp8266_remote_card_ok_flag = 0;
        g_wait_card_auth = 0;
        g_app.used_seconds = esp8266_remote_restore_sec; 
        memset(g_app.user_name, 0, sizeof(g_app.user_name));
        strcpy((char*)g_app.user_name, esp8266_remote_user_name);
        sscanf(esp8266_remote_balance_str, "%f", &bal_f);
        g_app.balance = (u32)(bal_f * 100);
        if (g_app.state != STATE_INUSE) {
            g_app.state = STATE_INUSE;
            g_app.pc_on = 1; g_app.light_on = 1;
            Seat_PC_Set(1); Seat_Light_Set(1);
            g_screen = SCREEN_INUSE;
            g_confirm = CONFIRM_NONE;
            UI_DrawInuseStatic();
            UI_ShowServerMsg("刷卡成功");
        }
        UI_UpdateStateLine(); UI_UpdateUserLine(); UI_UpdateCardLine(); UI_UpdateBalanceLine(); UI_UpdateRuntimeAndFee(); UI_UpdatePCButton(); UI_UpdateLightButton();
        g_server_msg_secs = 3;
    }
    if (esp8266_remote_card_err_flag) {
        esp8266_remote_card_err_flag = 0;
        g_wait_card_auth = 0;
        UI_ShowServerMsg(esp8266_card_err_msg[0] ? esp8266_card_err_msg : "刷卡失败");
        g_server_msg_secs = 5;
        g_app.has_user = 0;
        g_card_cooldown_tick = GetSysMs() + 3000;
    }
    if (g_wait_card_auth && GetSysMs() - g_card_auth_tick >= 5000) {
        g_wait_card_auth = 0;
        UI_ShowServerMsg("验证超时");
        g_server_msg_secs = 3;
        g_card_cooldown_tick = GetSysMs() + 3000;
    }

    if (g_server_msg_secs > 0) {
        g_server_msg_secs--;
        if (g_server_msg_secs == 0) UI_ClearServerMsg();
    }

    if ((g_app.state == STATE_INUSE) && (g_screen == SCREEN_INUSE)) {
        g_app.used_seconds++;
        UI_UpdateRuntimeAndFee();   
    }

    // ★★★ 报警检测逻辑 ★★★
    human         = Seat_Radar_Get();
    smoke_adc     = Seat_Smoke_GetRaw();
    smoke_percent = Smoke_AdcToPercent(smoke_adc);

    // 只有在【非维护模式】下才检测报警
    if (g_app.maint_mode == 0) 
    {
        // 1. 烟雾报警
        if (smoke_percent >= 60) {
            if (g_smoke_alarm == 0) {
                g_smoke_alarm = 1;
                UI_DrawAlarmWindow("危险!", "烟雾报警"); // 弹窗
            }
        } else {
            if (g_smoke_alarm == 1) {
                g_smoke_alarm = 0;
                UI_ClearAlarmWindow(); // 清除弹窗
            }
        }

        // 2. 占座报警
        if ((g_screen == SCREEN_WELCOME) && (g_app.state == STATE_IDLE)) {
            if (human) {
                if (g_idle_human_seconds < 0xFFFFFFFF) g_idle_human_seconds++;
                if ((g_idle_human_seconds >= 12) && (g_idle_occupy_alarm == 0)) {
                    g_idle_occupy_alarm = 1;
                    ESP8266_MQTT_Pub_Async("netbar/seat001/alert", "occupy_over_120s");
                    UI_DrawAlarmWindow("警告", "请勿占座!"); // 弹窗
                }
            } else {
                if (g_idle_occupy_alarm == 1) {
                    g_idle_occupy_alarm = 0;
                    UI_ClearAlarmWindow(); // 清除弹窗
                }
                g_idle_human_seconds = 0;
            }
        } else {
            g_idle_occupy_alarm = 0;
            g_idle_human_seconds = 0;
        }
    } 
    else 
    {
        // 维护模式：强制清除所有报警标志
        g_idle_occupy_alarm = 0;
        g_smoke_alarm = 0;
    }

    if (g_app.state == STATE_INUSE) {
        if (human) g_inuse_nohuman_seconds = 0;
        else {
            if (g_inuse_nohuman_seconds < 0xFFFFFFFF) g_inuse_nohuman_seconds++;
            if (g_inuse_nohuman_seconds >= 15 * 60) {
                ESP8266_MQTT_Pub_Async("netbar/seat001/alert", "auto_checkout_nohuman_15min");
                App_EndSession();
                g_inuse_nohuman_seconds = 0;
            }
        }
    }

    // 只有在【没有弹窗】且【不在维护模式】时才刷新环境数据，防止覆盖弹窗
    if (g_app.maint_mode == 0 && g_smoke_alarm == 0 && g_idle_occupy_alarm == 0) {
        if (g_screen == SCREEN_WELCOME) UI_UpdateEnvWelcome(human, smoke_percent);
        else if (g_screen == SCREEN_INUSE) UI_UpdateEnvInuse(human, smoke_percent);
    }

    UI_UpdateWifiIcon();
}

static void App_InitContext(void)
{
    g_app.state        = STATE_IDLE;
    g_app.has_user     = 0;
    memset(g_app.uid, 0, 4);
    strcpy((char *)g_app.user_name, "--");
    g_app.balance      = 0;        
    g_app.used_seconds = 0;
    g_app.pc_on        = 0;
    g_app.light_on     = 0;
    g_app.maint_mode   = 0;        

    Seat_PC_Set(0);
    Seat_Light_Set(0);
    Seat_Buzzer_Set(0);

    g_idle_human_seconds    = 0;
    g_idle_occupy_alarm     = 0;
    g_smoke_alarm           = 0;
    g_inuse_nohuman_seconds = 0;
    g_wait_card_auth        = 0;
    g_server_msg_secs       = 0;
}

// --- 界面绘制函数 ---

static void UI_DrawSplashScreen(void) {
    LCD_Clear(BLUE);
    POINT_COLOR = WHITE;
    BACK_COLOR  = BLUE;
    Show_Str(30, 100, 240, 24, (u8 *)"智能无人网吧系统", 24, 0);
    Show_Str(80, 140, 200, 16, (u8 *)"System Init...", 16, 0);
    Show_Str(60, 280, 200, 16, (u8 *)"By: Student", 16, 0);
}

static void UI_DrawMaintenanceStatic(void) {
    LCD_Clear(WHITE);
    LCD_Fill(0, 0, lcddev.width - 1, 40, BLUE);
    POINT_COLOR = WHITE;
    BACK_COLOR  = BLUE;
    Show_Str(10, 10, 240, 24, (u8 *)"设备维护中", 24, 0);
    UI_UpdateWifiIcon();
    LCD_Fill(40, 100, 280, 220, YELLOW);
    LCD_DrawRectangle(40, 100, 280, 220);
    POINT_COLOR = RED;
    BACK_COLOR  = YELLOW;
    Show_Str(80, 130, 200, 24, (u8 *)"暂停服务", 24, 0);
    POINT_COLOR = BLACK;
    Show_Str(60, 170, 240, 16, (u8 *)"工程师正在维护...", 16, 0);
}

static void UI_DrawAlarmWindow(const char* title, const char* msg) {
    u16 x = 30, y = 80, w = 260, h = 120;
    LCD_Fill(x, y, x+w, y+h, RED);
    LCD_DrawRectangle(x, y, x+w, y+h);
    LCD_DrawRectangle(x+2, y+2, x+w-2, y+h-2); 
    POINT_COLOR = WHITE;
    BACK_COLOR  = RED;
    Show_Str(x+20, y+30, 200, 24, (u8 *)title, 24, 0);
    Show_Str(x+20, y+70, 220, 16, (u8 *)msg, 16, 0);
}

static void UI_ClearAlarmWindow(void) {
    if (g_screen == SCREEN_WELCOME) UI_DrawWelcomeStatic();
    else if (g_screen == SCREEN_INUSE) UI_DrawInuseStatic();
    else if (g_screen == SCREEN_MAINTENANCE) UI_DrawMaintenanceStatic();
}

static void UI_DrawWelcomeStatic(void) {
    LCD_Clear(WHITE);
    LCD_Fill(0, 0, lcddev.width - 1, 30, BLUE);
    POINT_COLOR = WHITE; BACK_COLOR = BLUE;
    Show_Str(10, 5, lcddev.width - 90, 24, (u8 *)"智能无人网吧系统", 24, 0); 
    UI_UpdateWifiIcon();   
    POINT_COLOR = BLACK; BACK_COLOR = WHITE;
    Show_Str(10, 40, lcddev.width - 20, 16, (u8 *)"欢迎使用", 16, 0);
    Show_Str(10, 60, lcddev.width - 20, 16, (u8 *)"费率: 1元/分钟(测试)", 16, 0);
    Show_Str(10, 90,  80, 16, (u8 *)"座位:", 16, 0);
    Show_Str(10, 110, 80, 16, (u8 *)"烟雾:", 16, 0);
    LCD_Fill(70, 90,  lcddev.width - 10, 106, WHITE);
    LCD_Fill(70, 110, lcddev.width - 10, 126, WHITE);
    LCD_Fill(10, 150, lcddev.width - 10, 190, GRAY);
    POINT_COLOR = BLUE; BACK_COLOR  = GRAY;
    if (g_app.maint_mode) Show_Str(20, 160, lcddev.width - 40, 16, (u8 *)"维护中，请勿上机", 16, 0);
    else Show_Str(20, 160, lcddev.width - 40, 16, (u8 *)"请刷卡上机", 16, 0);
    POINT_COLOR = BLACK; BACK_COLOR  = WHITE;
    Show_Str(10, 210, lcddev.width - 20, 16, (u8 *)"费率:1元/分钟", 16, 0);
    LCD_Fill(10, 230, lcddev.width - 10, 250, WHITE);
}

static void UI_DrawInuseStatic(void) {
    LCD_Clear(WHITE);
    LCD_Fill(0, 0, lcddev.width - 1, 30, BLUE);
    POINT_COLOR = WHITE; BACK_COLOR  = BLUE;
    Show_Str(10, 5, lcddev.width - 90, 24, (u8 *)"上机中", 24, 0); 
    UI_UpdateWifiIcon();
    POINT_COLOR = BLACK; BACK_COLOR  = WHITE;
    Show_Str(10, 32, lcddev.width - 20, 16, (u8 *)"费率: 1元/分钟(测试)", 16, 0);
    Show_Str(10, 50,  70, 16, (u8 *)"状态:", 16, 0);
    Show_Str(10, 70,  70, 16, (u8 *)"用户:", 16, 0);
    Show_Str(10, 90,  70, 16, (u8 *)"卡号:", 16, 0);
    Show_Str(10, 110, 70, 16, (u8 *)"余额:", 16, 0);
    Show_Str(10, 130, 70, 16, (u8 *)"时长:", 16, 0);
    Show_Str(10, 150, 70, 16, (u8 *)"费用:", 16, 0);
    LCD_DrawRectangle(BTN_PC_X1, BTN_PC_Y1, BTN_PC_X2, BTN_PC_Y2);             
    LCD_DrawRectangle(BTN_LIGHT_X1, BTN_LIGHT_Y1, BTN_LIGHT_X2, BTN_LIGHT_Y2); 
    LCD_DrawRectangle(BTN_EXIT_X1, BTN_EXIT_Y1, BTN_EXIT_X2, BTN_EXIT_Y2);     
    Show_Str(10, 280, 80, 16, (u8 *)"座位:", 16, 0);
    Show_Str(10, 300, 80, 16, (u8 *)"烟雾:", 16, 0);
    Show_Str(10, 320, 120, 16, (u8 *)"费率:1元/分钟", 16, 0);
    LCD_Fill(70, 280, lcddev.width - 10, 336, WHITE);
}

static void UI_ShowServerMsg(const char *msg) {
    u16 x = 10;
    u16 y = lcddev.height - 24;   
    u16 w = lcddev.width - 20;
    u16 h = 20;
    LCD_Fill(x, y, x + w, y + h, GRAY);
    POINT_COLOR = BLACK;
    BACK_COLOR  = GRAY;
    Show_Str(x + 4, y + 2, w - 8, 16, (u8 *)msg, 16, 0);
}

static void UI_ClearServerMsg(void) {
    u16 x = 10;
    u16 y = lcddev.height - 24;
    u16 w = lcddev.width - 20;
    u16 h = 20;
    LCD_Fill(x, y, x + w, y + h, WHITE);
}

static void UI_UpdateEnvWelcome(u8 human, u8 smoke_percent) { char buf[16]; u8 *level; LCD_Fill(70, 90, lcddev.width - 10, 106, WHITE); if (human) { POINT_COLOR = RED; Show_Str(70, 90, lcddev.width - 80, 16, (u8 *)"有人", 16, 0); } else { POINT_COLOR = BLACK; Show_Str(70, 90, lcddev.width - 80, 16, (u8 *)"无人", 16, 0); } LCD_Fill(70, 110, lcddev.width - 10, 126, WHITE); sprintf(buf, "%3d%%", smoke_percent); POINT_COLOR = BLACK; LCD_ShowString(70, 110, 80, 16, 16, (u8 *)buf); level = Smoke_LevelText(smoke_percent); Show_Str(150, 110, 100, 16, level, 16, 0); }
static void UI_UpdateEnvInuse(u8 human, u8 smoke_percent) { char buf[16]; u8 *level; LCD_Fill(70, 280, lcddev.width - 10, 296, WHITE); if (human) { POINT_COLOR = RED; Show_Str(70, 280, lcddev.width - 80, 16, (u8 *)"有人", 16, 0); } else { POINT_COLOR = BLACK; Show_Str(70, 280, lcddev.width - 80, 16, (u8 *)"无人", 16, 0); } LCD_Fill(70, 300, lcddev.width - 10, 316, WHITE); sprintf(buf, "%3d%%", smoke_percent); POINT_COLOR = BLACK; LCD_ShowString(70, 300, 80, 16, 16, (u8 *)buf); level = Smoke_LevelText(smoke_percent); Show_Str(150, 300, 100, 16, level, 16, 0); }
static void UI_UpdateStateLine(void) { u8 *text; POINT_COLOR = BLACK; BACK_COLOR = WHITE; LCD_Fill(80, 50, 310, 66, WHITE); if (g_app.state == STATE_IDLE) text = (u8 *)"待机"; else text = (u8 *)"上机中"; Show_Str(80, 50, 230, 16, text, 16, 0); }
static void UI_UpdateUserLine(void) { POINT_COLOR = BLACK; BACK_COLOR = WHITE; LCD_Fill(80, 70, 310, 86, WHITE); if (g_app.state == STATE_IDLE || g_app.has_user == 0) Show_Str(80, 70, 230, 16, (u8 *)"--", 16, 0); else Show_Str(80, 70, 230, 16, g_app.user_name, 16, 0); }
static void UI_UpdateCardLine(void) { char buf[24]; POINT_COLOR = BLACK; BACK_COLOR = WHITE; LCD_Fill(80, 90, 310, 106, WHITE); if (g_app.has_user) { sprintf(buf, "%02X %02X %02X %02X", g_app.uid[0], g_app.uid[1], g_app.uid[2], g_app.uid[3]); LCD_ShowString(80, 90, 230, 16, 16, (u8 *)buf); } else Show_Str(80, 90, 230, 16, (u8 *)"--", 16, 0); }
static void UI_UpdateBalanceLine(void) { char buf[24]; u32 yuan; u32 cent; POINT_COLOR = BLACK; BACK_COLOR = WHITE; LCD_Fill(80, 110, 310, 126, WHITE); yuan = g_app.balance / 100; cent = g_app.balance % 100; sprintf(buf, "%lu.%02lu 元", (unsigned long)yuan, (unsigned long)cent); Show_Str(80, 110, 230, 16, (u8 *)buf, 16, 0); }
static void UI_UpdateRuntimeAndFee(void) { u32 min; u32 sec; u32 fee; char buf[24]; min = g_app.used_seconds / 60; sec = g_app.used_seconds % 60; POINT_COLOR = BLACK; BACK_COLOR = WHITE; LCD_Fill(80, 130, 310, 146, WHITE); sprintf(buf, "%02lu:%02lu", min, sec); Show_Str(80, 130, 230, 16, (u8 *)buf, 16, 0); fee = (g_app.used_seconds / 60) * PRICE_PER_MIN; LCD_Fill(80, 150, 310, 166, WHITE); sprintf(buf, "%4lu 元", fee); Show_Str(80, 150, 230, 16, (u8 *)buf, 16, 0); }
static void UI_UpdatePCButton(void) { POINT_COLOR = BLACK; BACK_COLOR = WHITE; if (g_app.pc_on) { LCD_Fill(BTN_PC_X1 + 1, BTN_PC_Y1 + 1, BTN_PC_X2 - 1, BTN_PC_Y2 - 1, GREEN); Show_Str(BTN_PC_X1 + 20, BTN_PC_Y1 + 12, 100, 16, (u8 *)"关电脑", 16, 0); } else { LCD_Fill(BTN_PC_X1 + 1, BTN_PC_Y1 + 1, BTN_PC_X2 - 1, BTN_PC_Y2 - 1, GRAY); Show_Str(BTN_PC_X1 + 20, BTN_PC_Y1 + 12, 100, 16, (u8 *)"开电脑", 16, 0); } LCD_DrawRectangle(BTN_PC_X1, BTN_PC_Y1, BTN_PC_X2, BTN_PC_Y2); }
static void UI_UpdateLightButton(void) { POINT_COLOR = BLACK; BACK_COLOR = WHITE; if (g_app.light_on) { LCD_Fill(BTN_LIGHT_X1 + 1, BTN_LIGHT_Y1 + 1, BTN_LIGHT_X2 - 1, BTN_LIGHT_Y2 - 1, GREEN); Show_Str(BTN_LIGHT_X1 + 20, BTN_LIGHT_Y1 + 12, 100, 16, (u8 *)"关灯光", 16, 0); } else { LCD_Fill(BTN_LIGHT_X1 + 1, BTN_LIGHT_Y1 + 1, BTN_LIGHT_X2 - 1, BTN_LIGHT_Y2 - 1, GRAY); Show_Str(BTN_LIGHT_X1 + 20, BTN_LIGHT_Y1 + 12, 100, 16, (u8 *)"开灯光", 16, 0); } LCD_DrawRectangle(BTN_LIGHT_X1, BTN_LIGHT_Y1, BTN_LIGHT_X2, BTN_LIGHT_Y2); LCD_Fill(BTN_EXIT_X1 + 1, BTN_EXIT_Y1 + 1, BTN_EXIT_X2 - 1, BTN_EXIT_Y2 - 1, RED); POINT_COLOR = WHITE; BACK_COLOR = RED; Show_Str(BTN_EXIT_X1 + 20, BTN_EXIT_Y1 + 12, 120, 16, (u8 *)"下机结算", 16, 0); LCD_DrawRectangle(BTN_EXIT_X1, BTN_EXIT_Y1, BTN_EXIT_X2, BTN_EXIT_Y2); POINT_COLOR = BLACK; BACK_COLOR = WHITE; }
static void UI_UpdateWifiIcon(void) { WifiState_t cur; u16 base_x; u16 base_y; cur = ESP8266_GetState(); base_x = lcddev.width - 40; base_y = 6; POINT_COLOR = WHITE; BACK_COLOR = BLUE; LCD_Fill(lcddev.width - 70, 0, lcddev.width - 1, 30, BLUE); if (cur == WIFI_STATE_RUNNING) { LCD_Fill(base_x, base_y + 8, base_x + 6, base_y + 14, WHITE); LCD_Fill(base_x + 8, base_y + 4, base_x + 14, base_y + 14, WHITE); LCD_Fill(base_x + 16, base_y, base_x + 22, base_y + 14, WHITE); } else if (cur > WIFI_STATE_INIT && cur < WIFI_STATE_RUNNING) { LCD_Fill(base_x, base_y + 8, base_x + 6, base_y + 14, WHITE); LCD_Fill(base_x + 8, base_y + 4, base_x + 14, base_y + 14, WHITE); LCD_DrawRectangle(base_x + 16, base_y, base_x + 22, base_y + 14); } else { POINT_COLOR = WHITE; LCD_DrawLine(base_x, base_y, base_x + 15, base_y + 15); LCD_DrawLine(base_x, base_y + 15, base_x + 15, base_y); } POINT_COLOR = BLACK; BACK_COLOR = WHITE; }
static u8 Smoke_AdcToPercent(u16 adc) { if (adc <= 500) return 0; if (adc >= 3500) return 100; return (u8)((adc - 500) * 100 / (3500 - 500)); }
static u8* Smoke_LevelText(u8 percent) { if (percent < 30) return (u8 *)"良好"; else if (percent < 60) return (u8 *)"中等"; else if (percent < 80) return (u8 *)"较高"; else return (u8 *)"危险"; }
static void UID_ToHex(const u8 *uid, char *out) { sprintf(out, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]); }
static void App_OnCard(u8 *uid) { u8 i; char uid_hex[9]; char payload[32]; if (g_app.maint_mode) { UI_ShowServerMsg("座位维护中，暂不提供上机服务"); g_server_msg_secs = 5; return; } if (g_app.state == STATE_INUSE) { UI_ShowServerMsg("已在上机中"); g_server_msg_secs = 3; return; } for (i = 0; i < 4; i++) g_app.uid[i] = uid[i]; g_app.has_user = 1; UID_ToHex(uid, uid_hex); sprintf(payload, "uid=%s", uid_hex); if (ESP8266_GetState() == WIFI_STATE_RUNNING) { ESP8266_MQTT_Pub_Async("netbar/seat001/card", payload); g_wait_card_auth = 1; g_card_auth_tick = GetSysMs(); UI_ShowServerMsg("正在验证，请稍候..."); g_server_msg_secs = 5; } else { UI_ShowServerMsg("网络未连接，无法验证"); g_server_msg_secs = 5; } }
static void App_EndSession(void) { App_InitContext(); g_screen = SCREEN_WELCOME; g_confirm = CONFIRM_NONE; UI_DrawWelcomeStatic(); ESP8266_MQTT_Pub_Async("netbar/seat001/cmd", "checkout"); }
static void UI_ShowConfirmDialog(confirm_type_t type) { LCD_Fill(CONF_X1, CONF_Y1, CONF_X2, CONF_Y2, GRAY); LCD_DrawRectangle(CONF_X1, CONF_Y1, CONF_X2, CONF_Y2); POINT_COLOR = BLACK; BACK_COLOR = GRAY; if (type == CONFIRM_PC_OFF) Show_Str(CONF_X1 + 20, CONF_Y1 + 20, 220, 16, (u8 *)"确认关机？", 16, 0); else if (type == CONFIRM_EXIT) Show_Str(CONF_X1 + 20, CONF_Y1 + 20, 220, 16, (u8 *)"确认下机结算？", 16, 0); LCD_Fill(CONF_BTN_OK_X1, CONF_BTN_OK_Y1, CONF_BTN_OK_X2, CONF_BTN_OK_Y2, GREEN); POINT_COLOR = WHITE; BACK_COLOR = GREEN; Show_Str(CONF_BTN_OK_X1 + 8, CONF_BTN_OK_Y1 + 8, 80, 16, (u8 *)"确认", 16, 0); LCD_DrawRectangle(CONF_BTN_OK_X1, CONF_BTN_OK_Y1, CONF_BTN_OK_X2, CONF_BTN_OK_Y2); LCD_Fill(CONF_BTN_CANCEL_X1, CONF_BTN_CANCEL_Y1, CONF_BTN_CANCEL_X2, CONF_BTN_CANCEL_Y2, RED); POINT_COLOR = WHITE; BACK_COLOR = RED; Show_Str(CONF_BTN_CANCEL_X1 + 8, CONF_BTN_CANCEL_Y1 + 8, 80, 16, (u8 *)"取消", 16, 0); LCD_DrawRectangle(CONF_BTN_CANCEL_X1, CONF_BTN_CANCEL_Y1, CONF_BTN_CANCEL_X2, CONF_BTN_CANCEL_Y2); POINT_COLOR = BLACK; BACK_COLOR = WHITE; }
static void App_HandleTouch(void) { u8 is_down; u16 x, y; if (tp_dev.scan(0)) { is_down = (tp_dev.sta & TP_PRES_DOWN) ? 1 : 0; x = tp_dev.x[0]; y = tp_dev.y[0]; } else { is_down = 0; x = 0; y = 0; } if (is_down) { if (!g_touch_down) { g_touch_down = 1; if (g_screen == SCREEN_INUSE) { if ((x > BTN_PC_X1) && (x < BTN_PC_X2) && (y > BTN_PC_Y1) && (y < BTN_PC_Y2)) { if (!g_app.pc_on) { g_app.pc_on = 1; Seat_PC_Set(1); UI_UpdatePCButton(); } else { g_confirm = CONFIRM_PC_OFF; g_screen = SCREEN_CONFIRM; UI_ShowConfirmDialog(g_confirm); } } else if ((x > BTN_LIGHT_X1) && (x < BTN_LIGHT_X2) && (y > BTN_LIGHT_Y1) && (y < BTN_LIGHT_Y2)) { g_app.light_on = g_app.light_on ? 0 : 1; Seat_Light_Set(g_app.light_on); UI_UpdateLightButton(); } else if ((x > BTN_EXIT_X1) && (x < BTN_EXIT_X2) && (y > BTN_EXIT_Y1) && (y < BTN_EXIT_Y2)) { if (g_app.state == STATE_INUSE) { g_confirm = CONFIRM_EXIT; g_screen = SCREEN_CONFIRM; UI_ShowConfirmDialog(g_confirm); } } } else if (g_screen == SCREEN_CONFIRM) { if ((x > CONF_BTN_OK_X1) && (x < CONF_BTN_OK_X2) && (y > CONF_BTN_OK_Y1) && (y < CONF_BTN_OK_Y2)) { if (g_confirm == CONFIRM_PC_OFF) { g_app.pc_on = 0; Seat_PC_Set(0); g_screen = SCREEN_INUSE; g_confirm = CONFIRM_NONE; UI_DrawInuseStatic(); UI_UpdateStateLine(); UI_UpdateUserLine(); UI_UpdateCardLine(); UI_UpdateBalanceLine(); UI_UpdateRuntimeAndFee(); UI_UpdatePCButton(); UI_UpdateLightButton(); } else if (g_confirm == CONFIRM_EXIT) App_EndSession(); } else if ((x > CONF_BTN_CANCEL_X1) && (x < CONF_BTN_CANCEL_X2) && (y > CONF_BTN_CANCEL_Y1) && (y < CONF_BTN_CANCEL_Y2)) { g_screen = SCREEN_INUSE; g_confirm = CONFIRM_NONE; UI_DrawInuseStatic(); UI_UpdateStateLine(); UI_UpdateUserLine(); UI_UpdateCardLine(); UI_UpdateBalanceLine(); UI_UpdateRuntimeAndFee(); UI_UpdatePCButton(); UI_UpdateLightButton(); } } } } else g_touch_down = 0; }

static void Netbar_Publish_SeatState(void) { char payload[96]; u8 human; u16 smoke_adc; u8 smoke_percent; u8 iu; u32 used_seconds; u32 fee; u8 alarm_active; if (ESP8266_GetState() != WIFI_STATE_RUNNING) return; human = Seat_Radar_Get(); smoke_adc = Seat_Smoke_GetRaw(); smoke_percent = Smoke_AdcToPercent(smoke_adc); iu = (g_app.state == STATE_INUSE) ? 1 : 0; used_seconds = g_app.used_seconds; fee = (used_seconds / 60) * PRICE_PER_MIN; alarm_active = g_idle_occupy_alarm || g_smoke_alarm; sprintf(payload, "s=1;iu=%d;pc=%d;lt=%d;hm=%d;sm=%d;sec=%lu;fee=%lu;al=%d", iu, g_app.pc_on, g_app.light_on, human, smoke_percent, used_seconds, fee, alarm_active); publish_seq++; ESP8266_MQTT_Pub_Async("netbar/seat001/state", payload); }
	