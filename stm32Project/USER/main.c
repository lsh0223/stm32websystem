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
#include "24cxx.h"
#include <string.h>
#include <stdio.h>

// 引用外部标准RC522操作函数
extern u8 PcdAuthState(u8 auth_mode,u8 addr,u8 *pKey,u8 *pSnr);
extern u8 PcdRead(u8 addr,u8 *pData);
extern char PcdSelect(unsigned char *pSnr); 
extern void ClearBitMask(unsigned char reg, unsigned char mask); 

void LCD_Display_Dir(u8 dir);  

float g_price_per_min = 1.0f;     

// ==========================================
// ★★★ 现代扁平化 UI 配色方案 (RGB565) ★★★
// ==========================================
#define C_BG          0xFFFF // 纯白背景
#define C_TOPBAR      0x2945 // 深蓝灰色 (现代科技感)
#define C_CARD        0xF7DE // 浅灰色 (用于模块卡片背景)
#define C_PRIMARY     0x3498 // 扁平蓝 (按钮主色)
#define C_SUCCESS     0x2E6C // 扁平绿 (开启状态)
#define C_DANGER      0xE328 // 扁平红 (关闭/警告/下机)
#define C_WARN        0xFDC0 // 扁平黄 (提醒/维护)
#define C_TEXT        0x0000 // 黑字
#define C_GRAYTEXT    0x4A49 // 深灰字

// UI 坐标宏定义
#define BTN_PC_X1           20
#define BTN_PC_Y1           180
#define BTN_PC_X2           140
#define BTN_PC_Y2           220
#define BTN_LIGHT_X1        180
#define BTN_LIGHT_Y1        180
#define BTN_LIGHT_X2        300
#define BTN_LIGHT_Y2        220
#define BTN_EXIT_X1         60
#define BTN_EXIT_Y1         235
#define BTN_EXIT_X2         260
#define BTN_EXIT_Y2         275

// ★ 修复：将按钮向上方移动，彻底避开底部 30 像素的消息提示区
#define BTN_MODE_X1         220
#define BTN_MODE_Y1         410
#define BTN_MODE_X2         300
#define BTN_MODE_Y2         440

#define BIND_BOX_X1         20
#define BIND_BOX_Y1         265
#define BIND_BOX_X2         300
#define BIND_BOX_Y2         320
#define BIND_CLOSE_X1       260
#define BIND_CLOSE_Y1       275
#define BIND_CLOSE_X2       290
#define BIND_CLOSE_Y2       305

char g_device_id[32];
char g_seat_name[32] = {0}; // 存储云端下发的自定义设备名

#define CONF_X1             30
#define CONF_Y1             120
#define CONF_X2             290
#define CONF_Y2             260
#define CONF_BTN_OK_X1      (CONF_X1 + 20)
#define CONF_BTN_OK_Y1      (CONF_Y2  - 50)
#define CONF_BTN_OK_X2      (CONF_X1 + 110)
#define CONF_BTN_OK_Y2      (CONF_Y2  - 20)
#define CONF_BTN_CANCEL_X1  (CONF_X2  - 110)
#define CONF_BTN_CANCEL_Y1  (CONF_Y2  - 50)
#define CONF_BTN_CANCEL_X2  (CONF_X2  - 20)
#define CONF_BTN_CANCEL_Y2  (CONF_Y2  - 20)

extern char esp8266_remote_uid[32]; 
char g_device_id[32];

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

static u8 g_system_mode = 0;
static u8 g_show_bind_msg = 0;
static char g_bind_code_str[64] = {0};

#define RATE_SAVE_ADDR 0x50 
void Save_Rate_To_EEPROM(float price) {
    u8 *p = (u8*)&price; 
    u8 i;
    for(i=0; i<4; i++) { AT24CXX_WriteOneByte(RATE_SAVE_ADDR + i, p[i]); delay_ms(10); }
}

float Read_Rate_From_EEPROM(void) {
    float price; u8 *p = (u8*)&price; u8 i;
    for(i=0; i<4; i++) p[i] = AT24CXX_ReadOneByte(RATE_SAVE_ADDR + i);
    if(price < 0.01f || price > 1000.0f || price != price) return 1.0f; 
    return price;
}



// ================= ★ 新增：设备名断电保存逻辑 ★ =================
#define NAME_SAVE_ADDR 0x60 // 给设备名分配EEPROM的 0x60~0x7F 地址 (最多存32字符)

void Save_Name_To_EEPROM(const char* name) {
    u8 i = 0;
    // 逐字节写入EEPROM，遇到字符串结束符停止
    while(name[i] != '\0' && i < 31) {
        AT24CXX_WriteOneByte(NAME_SAVE_ADDR + i, name[i]);
        delay_ms(10); // EEPROM 写入需要物理延时
        i++;
    }
    AT24CXX_WriteOneByte(NAME_SAVE_ADDR + i, '\0'); // 写入结束符
    delay_ms(10);
}

void Read_Name_From_EEPROM(char* name) {
    u8 i = 0;
    u8 c = AT24CXX_ReadOneByte(NAME_SAVE_ADDR);
    // 如果首字节是 0xFF (EEPROM默认空数据) 或者 0x00，说明从没改过名字
    if(c == 0xFF || c == 0x00) {
        strcpy(name, g_device_id); // 使用物理设备号作为默认名字
        return;
    }
    // 循环读取直到遇到结束符
    name[0] = c;
    for(i = 1; i < 31; i++) {
        c = AT24CXX_ReadOneByte(NAME_SAVE_ADDR + i);
        name[i] = c;
        if(c == '\0') break;
    }
    name[31] = '\0'; // 保险起见，最后封口
}
// =================================================================




void Get_ChipID(void) {
    u32 id[3];
    id[0] = *(__IO u32*)(0x1FFF7A10);
    id[1] = *(__IO u32*)(0x1FFF7A14);
    id[2] = *(__IO u32*)(0x1FFF7A18);
    sprintf(g_device_id, "Seat_%04X%04X", (u16)(id[0]>>16), (u16)(id[2]&0xFFFF));
}

u8 Read_ID_Card(u8 *uid, char *out_id) {
    u8 key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; 
    u8 buf4[16]; u8 buf5[16]; u8 res = 0;
    memset(out_id, 0, 19);
    PcdSelect(uid);
    if (PcdAuthState(0x60, 4, key, uid) == 0) { 
        if (PcdRead(4, buf4) == 0 && PcdRead(5, buf5) == 0) {
            int i;
            for(i=0; i<16; i++) out_id[i] = buf4[i];
            out_id[16] = buf5[0]; out_id[17] = buf5[1]; out_id[18] = '\0';
            res = 1;
        }
    }
    ClearBitMask(0x08, 0x08);
    return res; 
}

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
static void UI_DrawBindMsg(void);              
static void UI_ClearBindMsg(void);             
static u8   Smoke_AdcToPercent(u16 adc);
static u8* Smoke_LevelText(u8 percent);
static void App_OnCard(u8 *uid, char *id_card);
static void App_EndSession(void);
static void UI_ShowConfirmDialog(confirm_type_t type);
static void App_HandleTouch(void);
static void Netbar_Publish_SeatState(void);      
static void App_Task_1s(void);                   
static void UID_ToHex(const u8 *uid, char *out);
static void UID_HexStr_To_Bytes(const char *str, u8 *out_uid);

// ★ 辅助UI绘制函数：带边框扁平卡片
static void Draw_Card(u16 x1, u16 y1, u16 x2, u16 y2) {
    LCD_Fill(x1, y1, x2, y2, C_CARD);
    POINT_COLOR = C_GRAYTEXT;
    LCD_DrawRectangle(x1, y1, x2, y2);
}

// ★ 辅助UI绘制函数：支持自定义文字颜色的带边框按钮
static void Draw_Button(u16 x1, u16 y1, u16 x2, u16 y2, u16 bg_color, u16 text_color, const char* text) {
    LCD_Fill(x1, y1, x2, y2, bg_color);
    POINT_COLOR = C_GRAYTEXT; // 统一灰色极简边框
    LCD_DrawRectangle(x1, y1, x2, y2);
    POINT_COLOR = text_color; BACK_COLOR = bg_color;
    Show_Str(x1 + (x2-x1)/2 - (strlen(text)*8)/2, y1 + (y2-y1)/2 - 8, x2-x1, 16, (u8*)text, 16, 0);
}

// ============================================================
// ★★★ 核心重构：纯黑炫酷进度条开机动画 ★★★
// ============================================================
static void UI_DrawSplashScreen(void) {
    int i; char buf[32];
    
    // ★ 修复1：使用纯黑背景彻底清屏，绝不会看到底层杂乱文字
    LCD_Clear(BLACK); 
    
    // 标题区域
    POINT_COLOR = WHITE; BACK_COLOR = BLACK;
    Show_Str(80, 160, 240, 24, (u8 *)"智能网吧终端", 24, 0);
    
    // 渐变加载动画
    for(i = 0; i <= 100; i+=2) {
        // 绘制进度条外框
        POINT_COLOR = C_GRAYTEXT;
        LCD_DrawRectangle(40, 240, 280, 260);
        
        // 填充进度条内部 (平滑增长)
        LCD_Fill(42, 242, 42 + (236 * i / 100), 258, C_PRIMARY);
        
        // 动态文字刷新
        sprintf(buf, "System Loading... %3d%%", i);
        POINT_COLOR = WHITE;
        Show_Str(60, 270, 200, 16, (u8 *)buf, 16, 0);
        
        POINT_COLOR = C_GRAYTEXT; // 设备号用暗色显示
        Show_Str(80, 420, 200, 16, (u8 *)g_device_id, 16, 0);
        
        delay_ms(25); 
    }
    delay_ms(300); 
}

int main(void)
{
    u8  font_res; u8  human; u16 smoke_adc; u8  smoke_percent; u8  uid[4] = {0}; u32 now; char id_card_str[20];

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    delay_init(168);
    uart_init(115200);
    Get_ChipID();
		strcpy(g_seat_name, g_device_id); // 开机时默认名字等于物理ID
    Seat_BSP_Init();      
    RFID_AppInit();       
    LCD_Init();
    AT24CXX_Init();
    LCD_Display_Dir(0);   
    
    font_res = font_init();  
    if (font_res) {
        LCD_Clear(WHITE); POINT_COLOR = RED;
        LCD_ShowString(10, 10, 300, 16, 16, (u8 *)"Font init error!");
        LCD_ShowString(10, 30, 300, 16, 16, (u8 *)"Please check W25QXX");
        while (1); 
    }

    // 首次开机播放动画
    UI_DrawSplashScreen();
    
    strcpy(g_seat_name, g_device_id); // 先默认设为物理ID
    
    if(AT24CXX_Check() == 0) {
        g_price_per_min = Read_Rate_From_EEPROM();
        Read_Name_From_EEPROM(g_seat_name); // ★ 新增：从芯片读取上一次保存的名字
    }

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

        if ((g_idle_occupy_alarm || g_smoke_alarm) && (g_app.maint_mode == 0)) {
            if ((now % 200) < 100) Seat_Buzzer_Set(1);
            else Seat_Buzzer_Set(0);
        } else Seat_Buzzer_Set(0); 

        if ((g_screen == SCREEN_WELCOME) && (g_app.state == STATE_IDLE) && (g_app.maint_mode == 0)) {
            if (g_wait_card_auth == 0 && now > g_card_cooldown_tick) {
                if (RFID_CheckCard(uid)) {
                    if (Read_ID_Card(uid, id_card_str) == 0) strcpy(id_card_str, "NO_ID_DATA");
                    App_OnCard(uid, id_card_str);
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
    u8  human; u16 smoke_adc; u8  smoke_percent; float bal_f = 0.0f; char topic[64];

    // ★ 修复3：网页点击复位时，强制完整播放开机动画
    if (esp8266_remote_reset_flag) {
        esp8266_remote_reset_flag = 0; 
        
        UI_DrawSplashScreen(); // 先播放纯黑动画清屏
        
        App_InitContext(); 
        g_screen = SCREEN_WELCOME; 
        g_confirm = CONFIRM_NONE; 
        UI_DrawWelcomeStatic(); // 动画播完再画主界面
        
        ESP8266_Init_Async(); 
        UI_ShowServerMsg("系统已重置"); 
        g_server_msg_secs = 3; 
        return;                 
    }

    if (esp8266_remote_set_rate_flag) {
        esp8266_remote_set_rate_flag = 0; g_price_per_min = esp8266_remote_rate_val; Save_Rate_To_EEPROM(g_price_per_min);
        if (g_screen == SCREEN_WELCOME) UI_DrawWelcomeStatic(); 
        else if (g_screen == SCREEN_INUSE) { UI_DrawInuseStatic(); UI_UpdateStateLine(); UI_UpdateUserLine(); UI_UpdateCardLine(); UI_UpdateBalanceLine(); UI_UpdateRuntimeAndFee(); UI_UpdatePCButton(); UI_UpdateLightButton(); }
    }

    if (esp8266_remote_maint_on_flag) {
        esp8266_remote_maint_on_flag = 0; g_app.maint_mode = 1; g_idle_occupy_alarm = 0; g_smoke_alarm = 0;
        g_screen = SCREEN_MAINTENANCE; UI_DrawMaintenanceStatic(); UI_ShowServerMsg("已进入维护模式"); g_server_msg_secs = 3;
    }
    if (esp8266_remote_maint_off_flag) {
        esp8266_remote_maint_off_flag = 0; g_app.maint_mode = 0;
        g_screen = SCREEN_WELCOME; UI_DrawWelcomeStatic(); UI_ShowServerMsg("维护结束，恢复正常"); g_server_msg_secs = 3;
    }

    if (esp8266_remote_pc_on_flag) { esp8266_remote_pc_on_flag = 0; g_app.pc_on = 1; Seat_PC_Set(1); if (g_screen == SCREEN_INUSE) UI_UpdatePCButton(); Netbar_Publish_SeatState(); }
    if (esp8266_remote_pc_off_flag) { esp8266_remote_pc_off_flag = 0; g_app.pc_on = 0; Seat_PC_Set(0); if (g_screen == SCREEN_INUSE) UI_UpdatePCButton(); Netbar_Publish_SeatState(); }
    if (esp8266_remote_light_on_flag) { esp8266_remote_light_on_flag = 0; g_app.light_on = 1; Seat_Light_Set(1); if (g_screen == SCREEN_INUSE) UI_UpdateLightButton(); Netbar_Publish_SeatState(); }
    if (esp8266_remote_light_off_flag) { esp8266_remote_light_off_flag = 0; g_app.light_on = 0; Seat_Light_Set(0); if (g_screen == SCREEN_INUSE) UI_UpdateLightButton(); Netbar_Publish_SeatState(); }
    if (esp8266_remote_checkout_flag) { esp8266_remote_checkout_flag = 0; if (g_app.state == STATE_INUSE) App_EndSession(); }
    
    if (esp8266_remote_msg_flag) {
        esp8266_remote_msg_flag = 0;
        
        // ★ 新增拦截逻辑：如果是重命名指令，拦截它并刷新顶部栏
        if (strncmp(esp8266_remote_msg, "SYS_RENAME:", 11) == 0) {
            memset(g_seat_name, 0, sizeof(g_seat_name));
            strncpy(g_seat_name, esp8266_remote_msg + 11, sizeof(g_seat_name) - 1);
					
					// ★ 新增：将新名字持久化保存进硬件 EEPROM
            Save_Name_To_EEPROM(g_seat_name);
            
            // 局部刷新顶部状态栏以显示新名字
            POINT_COLOR = WHITE; BACK_COLOR = C_TOPBAR;
            LCD_Fill(10, 8, lcddev.width - 90, 32, C_TOPBAR); // 清除旧名字区域
            Show_Str(10, 8, lcddev.width - 90, 24, (u8 *)g_seat_name, 24, 0); // 写新名字
            UI_UpdateWifiIcon();
            return; // 拦截成功，不再往下走弹出消息框的逻辑
        }
        
        if (strstr(esp8266_remote_msg, "未绑定") != NULL) {
            g_show_bind_msg = 1; memset(g_bind_code_str, 0, sizeof(g_bind_code_str)); strncpy(g_bind_code_str, esp8266_remote_msg, sizeof(g_bind_code_str) - 1);
            if (g_screen == SCREEN_WELCOME) UI_DrawBindMsg();
            Seat_Buzzer_Set(1); delay_ms(500); Seat_Buzzer_Set(0);
        } else {
            g_server_msg_secs = 6; UI_ShowServerMsg(esp8266_remote_msg);
        }
    }

    if (esp8266_remote_card_ok_flag) {
        esp8266_remote_card_ok_flag = 0; g_wait_card_auth = 0;
        if (g_app.has_user == 0) { UID_HexStr_To_Bytes(esp8266_remote_uid, g_app.uid); g_app.has_user = 1; }
        g_app.used_seconds = esp8266_remote_restore_sec; 
        memset(g_app.user_name, 0, sizeof(g_app.user_name)); strcpy((char*)g_app.user_name, esp8266_remote_user_name);
        sscanf(esp8266_remote_balance_str, "%f", &bal_f); g_app.balance = (u32)(bal_f * 100); g_show_bind_msg = 0;

        if (g_app.state != STATE_INUSE) {
            g_app.state = STATE_INUSE; g_app.pc_on = 1; g_app.light_on = 1; Seat_PC_Set(1); Seat_Light_Set(1);
            g_screen = SCREEN_INUSE; g_confirm = CONFIRM_NONE; UI_DrawInuseStatic();
        }
        UI_UpdateStateLine(); UI_UpdateUserLine(); UI_UpdateCardLine(); UI_UpdateBalanceLine(); UI_UpdateRuntimeAndFee(); UI_UpdatePCButton(); UI_UpdateLightButton();
    }
    
    if (esp8266_remote_card_err_flag) {
        esp8266_remote_card_err_flag = 0; g_wait_card_auth = 0;
        if(esp8266_card_err_msg[0] != '\0') { UI_ShowServerMsg(esp8266_card_err_msg); g_server_msg_secs = 5; }
        g_app.has_user = 0; g_card_cooldown_tick = GetSysMs() + 3000;
    }
    if (g_wait_card_auth && GetSysMs() - g_card_auth_tick >= 5000) {
        g_wait_card_auth = 0; UI_ShowServerMsg("验证超时或网络不通"); g_server_msg_secs = 3; g_card_cooldown_tick = GetSysMs() + 3000;
    }

    if (g_server_msg_secs > 0) { g_server_msg_secs--; if (g_server_msg_secs == 0) UI_ClearServerMsg(); }
    if ((g_app.state == STATE_INUSE) && (g_screen == SCREEN_INUSE)) { g_app.used_seconds++; if (g_smoke_alarm == 0 && g_idle_occupy_alarm == 0) UI_UpdateRuntimeAndFee(); }

    human = Seat_Radar_Get(); smoke_adc = Seat_Smoke_GetRaw(); smoke_percent = Smoke_AdcToPercent(smoke_adc);

    if (g_app.maint_mode == 0) {
        if (smoke_percent >= 60) {
            if (g_smoke_alarm == 0) { g_smoke_alarm = 1; UI_DrawAlarmWindow("危险!", "烟雾报警"); }
        } else {
            if (g_smoke_alarm == 1) { g_smoke_alarm = 0; UI_ClearAlarmWindow(); }
        }

        if ((g_screen == SCREEN_WELCOME) && (g_app.state == STATE_IDLE)) {
            if (human) {
                if (g_idle_human_seconds < 0xFFFFFFFF) g_idle_human_seconds++;
                if ((g_idle_human_seconds >= 3) && (g_idle_occupy_alarm == 0) && (g_system_mode == 0)) {
                    g_idle_occupy_alarm = 1; sprintf(topic, "netbar/%s/alert", g_device_id); ESP8266_MQTT_Pub_Async(topic, "occupy_over_120s"); UI_DrawAlarmWindow("警告", "请勿占座!");
                }
            } else {
                if (g_idle_occupy_alarm == 1) { g_idle_occupy_alarm = 0; UI_ClearAlarmWindow(); }
                g_idle_human_seconds = 0;
            }
        } else { g_idle_occupy_alarm = 0; g_idle_human_seconds = 0; }
    } else { g_idle_occupy_alarm = 0; g_smoke_alarm = 0; }

    if (g_app.state == STATE_INUSE && g_system_mode == 0) {
        if (human) g_inuse_nohuman_seconds = 0;
        else {
            if (g_inuse_nohuman_seconds < 0xFFFFFFFF) g_inuse_nohuman_seconds++;
            if (g_inuse_nohuman_seconds >= 15 * 60 / 90) {
                sprintf(topic, "netbar/%s/alert", g_device_id); ESP8266_MQTT_Pub_Async(topic, "auto_checkout_nohuman_15min"); App_EndSession(); g_inuse_nohuman_seconds = 0;
            }
        }
    }

    if (g_app.maint_mode == 0 && g_smoke_alarm == 0 && g_idle_occupy_alarm == 0) {
        if (g_screen == SCREEN_WELCOME) UI_UpdateEnvWelcome(human, smoke_percent);
        else if (g_screen == SCREEN_INUSE) UI_UpdateEnvInuse(human, smoke_percent);
    }
    UI_UpdateWifiIcon();
}

static void App_InitContext(void) {
    g_app.state = STATE_IDLE; g_app.has_user = 0; memset(g_app.uid, 0, 4); strcpy((char *)g_app.user_name, "--");
    g_app.balance = 0; g_app.used_seconds = 0; g_app.pc_on = 0; g_app.light_on = 0; g_app.maint_mode = 0;        
    Seat_PC_Set(0); Seat_Light_Set(0); Seat_Buzzer_Set(0);
    g_idle_human_seconds = 0; g_idle_occupy_alarm = 0; g_smoke_alarm = 0; g_inuse_nohuman_seconds = 0; g_wait_card_auth = 0; g_server_msg_secs = 0; g_show_bind_msg = 0;
}

static void UI_DrawMaintenanceStatic(void) {
    LCD_Clear(C_BG);
    LCD_Fill(0, 0, lcddev.width - 1, 40, C_TOPBAR);
    POINT_COLOR = WHITE; BACK_COLOR  = C_TOPBAR;
    Show_Str(10, 8, lcddev.width - 90, 24, (u8 *)g_seat_name, 24, 0); // 显示云端下发的名字
    UI_UpdateWifiIcon();
    
    Draw_Card(30, 100, 290, 220);
    POINT_COLOR = C_DANGER; BACK_COLOR  = C_CARD;
    Show_Str(110, 130, 200, 24, (u8 *)"暂停服务", 24, 0);
    POINT_COLOR = C_TEXT;
    Show_Str(80, 170, 240, 16, (u8 *)"工程师正在维护设备...", 16, 0);
}

static void UI_DrawAlarmWindow(const char* title, const char* msg) {
    u16 x = 20, y = 80, w = 280, h = 120;
    LCD_Fill(x, y, x+w, y+h, C_DANGER);
    POINT_COLOR = WHITE;
    LCD_DrawRectangle(x, y, x+w, y+h);
    LCD_DrawRectangle(x+2, y+2, x+w-2, y+h-2); 
    BACK_COLOR  = C_DANGER;
    Show_Str(x+40, y+30, 200, 24, (u8 *)title, 24, 0);
    Show_Str(x+40, y+70, 220, 16, (u8 *)msg, 16, 0);
}

static void UI_ClearAlarmWindow(void) {
    if (g_screen == SCREEN_WELCOME) UI_DrawWelcomeStatic();
    else if (g_screen == SCREEN_INUSE) {
        UI_DrawInuseStatic(); UI_UpdateStateLine(); UI_UpdateUserLine(); UI_UpdateCardLine(); UI_UpdateBalanceLine(); UI_UpdateRuntimeAndFee(); UI_UpdatePCButton(); UI_UpdateLightButton();
    } else if (g_screen == SCREEN_MAINTENANCE) UI_DrawMaintenanceStatic();
}

static void UI_DrawWelcomeStatic(void) {
    char buf[64];
    LCD_Clear(C_BG);
    
    // 顶部状态栏
    LCD_Fill(0, 0, lcddev.width - 1, 35, C_TOPBAR);
    POINT_COLOR = WHITE; BACK_COLOR = C_TOPBAR;
    Show_Str(10, 8, lcddev.width - 90, 24, (u8 *)g_seat_name, 24, 0); // 显示云端下发的名字 
    UI_UpdateWifiIcon();   

    // 信息卡片
    Draw_Card(10, 45, lcddev.width - 10, 120);
    POINT_COLOR = C_TEXT; BACK_COLOR = C_CARD;
    
    if(g_system_mode == 1) { 
        Show_Str(20, 55, lcddev.width - 40, 16, (u8 *)"当前模式: [门禁安全模式]", 16, 0);
        Show_Str(20, 90, 280, 16, (u8 *)"请刷实名卡开门", 16, 0);
    } else {
        Show_Str(20, 55, lcddev.width - 40, 16, (u8 *)"当前模式: [上机计费模式]", 16, 0);
        sprintf(buf, "标准费率: %.2f元/分", g_price_per_min);
        Show_Str(20, 75, lcddev.width - 40, 16, (u8 *)buf, 16, 0);
        POINT_COLOR = C_PRIMARY;
        Show_Str(20, 95, lcddev.width - 40, 16, (u8 *)"请将IC卡放置在感应区上机", 16, 0);
    }
    
    // 环境监测卡片
    Draw_Card(10, 130, lcddev.width - 10, 190);
    POINT_COLOR = C_TEXT; BACK_COLOR = C_CARD;
    Show_Str(20, 142, 80, 16, (u8 *)"座位检测:", 16, 0);
    Show_Str(20, 162, 80, 16, (u8 *)"烟雾浓度:", 16, 0);
    
    // ★ 修复2：模式切换按钮移动至右下角，并采用低调的灰色
    Draw_Button(BTN_MODE_X1, BTN_MODE_Y1, BTN_MODE_X2, BTN_MODE_Y2, C_CARD, C_GRAYTEXT, g_system_mode == 0 ? "切门禁" : "切座位");

    if (g_show_bind_msg) UI_DrawBindMsg();
}

static void UI_DrawBindMsg(void) {
    char *code_ptr; if (!g_show_bind_msg) return;
    Draw_Card(BIND_BOX_X1, BIND_BOX_Y1, BIND_BOX_X2, BIND_BOX_Y2);
    POINT_COLOR = C_DANGER; BACK_COLOR = C_CARD;
    Show_Str(BIND_BOX_X1 + 10, BIND_BOX_Y1 + 10, 180, 16, (u8 *)"新卡未绑定!请去微信注册", 16, 0);
    code_ptr = strstr(g_bind_code_str, "绑定码");
    if (code_ptr) Show_Str(BIND_BOX_X1 + 10, BIND_BOX_Y1 + 30, 200, 16, (u8 *)code_ptr, 16, 0);
    else Show_Str(BIND_BOX_X1 + 10, BIND_BOX_Y1 + 30, 200, 16, (u8 *)g_bind_code_str, 16, 0);
    Draw_Button(BIND_CLOSE_X1, BIND_CLOSE_Y1, BIND_CLOSE_X2, BIND_CLOSE_Y2, C_DANGER, WHITE, "X");
}

static void UI_ClearBindMsg(void) {
    LCD_Fill(BIND_BOX_X1-1, BIND_BOX_Y1-1, BIND_BOX_X2+1, BIND_BOX_Y2+1, C_BG);
}

static void UI_DrawInuseStatic(void) {
    char buf[64];
    LCD_Clear(C_BG);
    
    // 顶部状态栏
    LCD_Fill(0, 0, lcddev.width - 1, 35, C_TOPBAR);
    POINT_COLOR = WHITE; BACK_COLOR  = C_TOPBAR;
    Show_Str(10, 8, lcddev.width - 90, 24, (u8 *)g_seat_name, 24, 0); // 显示云端下发的名字 
    UI_UpdateWifiIcon();
    
    // 核心信息卡片
    Draw_Card(10, 45, lcddev.width - 10, 160);
    POINT_COLOR = C_GRAYTEXT; BACK_COLOR  = C_CARD;
    Show_Str(20, 55,  70, 16, (u8 *)"当前状态:", 16, 0);
    Show_Str(20, 75,  70, 16, (u8 *)"上机用户:", 16, 0);
    Show_Str(20, 95,  70, 16, (u8 *)"实名卡号:", 16, 0);
    Show_Str(20, 115, 70, 16, (u8 *)"实时时长:", 16, 0);
    Show_Str(20, 135, 70, 16, (u8 *)"预估消费:", 16, 0);
    
    // 环境卡片
    Draw_Card(10, 280, lcddev.width - 10, 340);
    POINT_COLOR = C_TEXT; BACK_COLOR = C_CARD;
    Show_Str(20, 292, 80, 16, (u8 *)"座位状态:", 16, 0);
    Show_Str(20, 312, 80, 16, (u8 *)"消防烟雾:", 16, 0);
    
    // 初始化按钮外框
    Draw_Button(BTN_PC_X1, BTN_PC_Y1, BTN_PC_X2, BTN_PC_Y2, C_BG, C_TEXT, "");
    Draw_Button(BTN_LIGHT_X1, BTN_LIGHT_Y1, BTN_LIGHT_X2, BTN_LIGHT_Y2, C_BG, C_TEXT, "");
    Draw_Button(BTN_EXIT_X1, BTN_EXIT_Y1, BTN_EXIT_X2, BTN_EXIT_Y2, C_DANGER, WHITE, "结 算 下 机");
}

static void UI_ShowServerMsg(const char *msg) {
    u16 x = 10, y = lcddev.height - 24, w = lcddev.width - 20, h = 20;
    LCD_Fill(x, y, x + w, y + h, C_TOPBAR);
    POINT_COLOR = WHITE; BACK_COLOR  = C_TOPBAR;
    Show_Str(x + 4, y + 2, w - 8, 16, (u8 *)msg, 16, 0);
}

static void UI_ClearServerMsg(void) {
    LCD_Fill(0, lcddev.height - 30, lcddev.width, lcddev.height, C_BG);
}

// 刷新环境UI提取
static void update_env_ui(u16 y_offset, u8 human, u8 smoke_percent) {
    char buf[16]; u8 *level;
    
    // ★ 修复：强制将接下来的文字背景色设置为灰色的卡片颜色
    BACK_COLOR = C_CARD; 
    
    LCD_Fill(120, y_offset, 250, y_offset+16, C_CARD);
    if (human) { POINT_COLOR = C_DANGER; Show_Str(120, y_offset, 100, 16, (u8 *)"检测到有人", 16, 0); } 
    else { POINT_COLOR = C_SUCCESS; Show_Str(120, y_offset, 100, 16, (u8 *)"无人", 16, 0); } 
    
    LCD_Fill(120, y_offset+20, 280, y_offset+36, C_CARD);
    sprintf(buf, "%3d%%", smoke_percent); 
    POINT_COLOR = C_TEXT; 
    LCD_ShowString(120, y_offset+20, 80, 16, 16, (u8 *)buf); 
    
    level = Smoke_LevelText(smoke_percent); 
    if(smoke_percent > 60) POINT_COLOR = C_DANGER; else POINT_COLOR = C_SUCCESS;
    Show_Str(180, y_offset+20, 100, 16, level, 16, 0); 
    
    // ★ 修复完毕：用完后恢复全局背景色为纯白，防止影响其他界面的绘制
    BACK_COLOR = C_BG; 
}

static void UI_UpdateEnvWelcome(u8 human, u8 smoke_percent) { update_env_ui(142, human, smoke_percent); }
static void UI_UpdateEnvInuse(u8 human, u8 smoke_percent)   { update_env_ui(292, human, smoke_percent); }

static void UI_UpdateStateLine(void) { POINT_COLOR = C_PRIMARY; BACK_COLOR = C_CARD; LCD_Fill(100, 55, 300, 71, C_CARD); if (g_app.state == STATE_IDLE) Show_Str(100, 55, 200, 16, (u8*)"待机", 16, 0); else Show_Str(100, 55, 200, 16, (u8*)"正在上机中", 16, 0); }
static void UI_UpdateUserLine(void)  { POINT_COLOR = C_TEXT; BACK_COLOR = C_CARD; LCD_Fill(100, 75, 300, 91, C_CARD); if (g_app.state == STATE_IDLE || g_app.has_user == 0) Show_Str(100, 75, 200, 16, (u8 *)"--", 16, 0); else Show_Str(100, 75, 200, 16, g_app.user_name, 16, 0); }
static void UI_UpdateCardLine(void)  { char buf[24]; POINT_COLOR = C_TEXT; BACK_COLOR = C_CARD; LCD_Fill(100, 95, 300, 111, C_CARD); if (g_app.has_user) { sprintf(buf, "%02X %02X %02X %02X", g_app.uid[0], g_app.uid[1], g_app.uid[2], g_app.uid[3]); LCD_ShowString(100, 95, 200, 16, 16, (u8 *)buf); } else Show_Str(100, 95, 200, 16, (u8 *)"--", 16, 0); }
static void UI_UpdateBalanceLine(void) { char buf[32]; float bal = g_app.balance / 100.0f; POINT_COLOR = C_TEXT; BACK_COLOR = C_CARD; LCD_Fill(100, 115, 300, 131, C_CARD); sprintf(buf, "%.2f 元", bal); Show_Str(200, 55, 100, 16, (u8 *)buf, 16, 0); }

static void UI_UpdateRuntimeAndFee(void) { 
    u32 min = g_app.used_seconds / 60; u32 sec = g_app.used_seconds % 60; 
    float fee = (g_app.used_seconds / 60.0f) * g_price_per_min; char buf[32]; 
    BACK_COLOR = C_CARD;
    
    POINT_COLOR = C_TEXT; LCD_Fill(100, 115, 300, 131, C_CARD); 
    sprintf(buf, "%02lu:%02lu", min, sec); Show_Str(100, 115, 200, 16, (u8 *)buf, 16, 0); 
    
    POINT_COLOR = C_DANGER; LCD_Fill(100, 135, 300, 151, C_CARD); 
    sprintf(buf, "%.2f 元", fee); Show_Str(100, 135, 200, 16, (u8 *)buf, 16, 0); 
}

static void UI_UpdatePCButton(void) { if (g_app.pc_on) Draw_Button(BTN_PC_X1, BTN_PC_Y1, BTN_PC_X2, BTN_PC_Y2, C_SUCCESS, WHITE, "电脑已开启"); else Draw_Button(BTN_PC_X1, BTN_PC_Y1, BTN_PC_X2, BTN_PC_Y2, C_CARD, C_TEXT, "点击开电脑"); }
static void UI_UpdateLightButton(void) { if (g_app.light_on) Draw_Button(BTN_LIGHT_X1, BTN_LIGHT_Y1, BTN_LIGHT_X2, BTN_LIGHT_Y2, C_WARN, C_TEXT, "灯光已开启"); else Draw_Button(BTN_LIGHT_X1, BTN_LIGHT_Y1, BTN_LIGHT_X2, BTN_LIGHT_Y2, C_CARD, C_TEXT, "点击开灯光"); }

static void UI_UpdateWifiIcon(void) { WifiState_t cur = ESP8266_GetState(); u16 bx = lcddev.width - 40, by = 10; POINT_COLOR = WHITE; BACK_COLOR = C_TOPBAR; LCD_Fill(lcddev.width - 70, 0, lcddev.width - 1, 30, C_TOPBAR); if (cur == WIFI_STATE_RUNNING) { LCD_Fill(bx, by+8, bx+6, by+14, WHITE); LCD_Fill(bx+8, by+4, bx+14, by+14, WHITE); LCD_Fill(bx+16, by, bx+22, by+14, WHITE); } else if (cur > WIFI_STATE_INIT) { LCD_Fill(bx, by+8, bx+6, by+14, WHITE); LCD_Fill(bx+8, by+4, bx+14, by+14, WHITE); LCD_DrawRectangle(bx+16, by, bx+22, by+14); } else { POINT_COLOR = C_DANGER; LCD_DrawLine(bx, by, bx+15, by+15); LCD_DrawLine(bx, by+15, bx+15, by); } POINT_COLOR = C_TEXT; BACK_COLOR = C_BG; }
static u8 Smoke_AdcToPercent(u16 adc) { if (adc <= 500) return 0; if (adc >= 3500) return 100; return (u8)((adc - 500) * 100 / (3500 - 500)); }
static u8* Smoke_LevelText(u8 percent) { if (percent < 30) return (u8 *)"优"; else if (percent < 60) return (u8 *)"良"; else if (percent < 80) return (u8 *)"警告"; else return (u8 *)"危险"; }
static void UID_ToHex(const u8 *uid, char *out) { sprintf(out, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]); }

static void App_OnCard(u8 *uid, char *id_card) { 
    u8 i; char uid_hex[9]; char payload[64]; char topic[64]; 
    if (g_app.maint_mode) { UI_ShowServerMsg("维护中，暂不提供服务"); g_server_msg_secs = 5; return; } 
    if (g_app.state == STATE_INUSE && g_system_mode == 0) { UI_ShowServerMsg("已在上机中"); g_server_msg_secs = 3; return; } 

    for (i = 0; i < 4; i++) g_app.uid[i] = uid[i]; g_app.has_user = 1; UID_ToHex(uid, uid_hex); 
    sprintf(payload, "uid=%s;id=%s", uid_hex, id_card); 
    
    if (ESP8266_GetState() == WIFI_STATE_RUNNING) { 
        if(g_system_mode == 1) { sprintf(topic, "netbar/%s/door_card", g_device_id); ESP8266_MQTT_Pub_Async(topic, payload); UI_ShowServerMsg("验证门禁身份中..."); } 
        else { sprintf(topic, "netbar/%s/card", g_device_id); ESP8266_MQTT_Pub_Async(topic, payload); g_wait_card_auth = 1; g_card_auth_tick = GetSysMs(); UI_ShowServerMsg("云端鉴权中, 请稍候..."); }
        g_server_msg_secs = 6; 
    } else { UI_ShowServerMsg("网络未连接，无法验证"); g_server_msg_secs = 5; } 
}

static void App_EndSession(void) { char topic[64]; App_InitContext(); g_screen = SCREEN_WELCOME; g_confirm = CONFIRM_NONE; UI_DrawWelcomeStatic(); sprintf(topic, "netbar/%s/cmd", g_device_id); ESP8266_MQTT_Pub_Async(topic, "checkout"); }
static void UI_ShowConfirmDialog(confirm_type_t type) { Draw_Card(CONF_X1, CONF_Y1, CONF_X2, CONF_Y2); POINT_COLOR = C_TEXT; BACK_COLOR = C_CARD; if (type == CONFIRM_PC_OFF) Show_Str(CONF_X1 + 40, CONF_Y1 + 30, 200, 16, (u8 *)"确定要关闭电脑吗？", 16, 0); else if (type == CONFIRM_EXIT) Show_Str(CONF_X1 + 40, CONF_Y1 + 30, 200, 16, (u8 *)"确定要下机并结算吗？", 16, 0); Draw_Button(CONF_BTN_OK_X1, CONF_BTN_OK_Y1, CONF_BTN_OK_X2, CONF_BTN_OK_Y2, C_SUCCESS, WHITE, "确 认"); Draw_Button(CONF_BTN_CANCEL_X1, CONF_BTN_CANCEL_Y1, CONF_BTN_CANCEL_X2, CONF_BTN_CANCEL_Y2, C_BG, C_TEXT, "取 消"); POINT_COLOR = C_TEXT; BACK_COLOR = C_BG; }

static void App_HandleTouch(void) { 
    u8 is_down; u16 x, y; 
    if (tp_dev.scan(0)) { is_down = (tp_dev.sta & TP_PRES_DOWN) ? 1 : 0; x = tp_dev.x[0]; y = tp_dev.y[0]; } else { is_down = 0; x = 0; y = 0; } 
    if (is_down) { 
        if (!g_touch_down) { 
            g_touch_down = 1; 
            if (g_screen == SCREEN_WELCOME) {
                if ((x > BTN_MODE_X1) && (x < BTN_MODE_X2) && (y > BTN_MODE_Y1) && (y < BTN_MODE_Y2)) { g_system_mode = (g_system_mode == 0) ? 1 : 0; UI_DrawWelcomeStatic(); delay_ms(200); }
                else if (g_show_bind_msg && (x > BIND_CLOSE_X1) && (x < BIND_CLOSE_X2) && (y > BIND_CLOSE_Y1) && (y < BIND_CLOSE_Y2)) { g_show_bind_msg = 0; UI_ClearBindMsg(); delay_ms(200); }
            } else if (g_screen == SCREEN_INUSE) { 
                if ((x > BTN_PC_X1) && (x < BTN_PC_X2) && (y > BTN_PC_Y1) && (y < BTN_PC_Y2)) { if (!g_app.pc_on) { g_app.pc_on = 1; Seat_PC_Set(1); UI_UpdatePCButton(); } else { g_confirm = CONFIRM_PC_OFF; g_screen = SCREEN_CONFIRM; UI_ShowConfirmDialog(g_confirm); } } 
                else if ((x > BTN_LIGHT_X1) && (x < BTN_LIGHT_X2) && (y > BTN_LIGHT_Y1) && (y < BTN_LIGHT_Y2)) { g_app.light_on = g_app.light_on ? 0 : 1; Seat_Light_Set(g_app.light_on); UI_UpdateLightButton(); } 
                else if ((x > BTN_EXIT_X1) && (x < BTN_EXIT_X2) && (y > BTN_EXIT_Y1) && (y < BTN_EXIT_Y2)) { if (g_app.state == STATE_INUSE) { g_confirm = CONFIRM_EXIT; g_screen = SCREEN_CONFIRM; UI_ShowConfirmDialog(g_confirm); } } 
            } else if (g_screen == SCREEN_CONFIRM) { 
                if ((x > CONF_BTN_OK_X1) && (x < CONF_BTN_OK_X2) && (y > CONF_BTN_OK_Y1) && (y < CONF_BTN_OK_Y2)) { if (g_confirm == CONFIRM_PC_OFF) { g_app.pc_on = 0; Seat_PC_Set(0); g_screen = SCREEN_INUSE; g_confirm = CONFIRM_NONE; UI_DrawInuseStatic(); UI_UpdateStateLine(); UI_UpdateUserLine(); UI_UpdateCardLine(); UI_UpdateBalanceLine(); UI_UpdateRuntimeAndFee(); UI_UpdatePCButton(); UI_UpdateLightButton(); } else if (g_confirm == CONFIRM_EXIT) App_EndSession(); } 
                else if ((x > CONF_BTN_CANCEL_X1) && (x < CONF_BTN_CANCEL_X2) && (y > CONF_BTN_CANCEL_Y1) && (y < CONF_BTN_CANCEL_Y2)) { g_screen = SCREEN_INUSE; g_confirm = CONFIRM_NONE; UI_DrawInuseStatic(); UI_UpdateStateLine(); UI_UpdateUserLine(); UI_UpdateCardLine(); UI_UpdateBalanceLine(); UI_UpdateRuntimeAndFee(); UI_UpdatePCButton(); UI_UpdateLightButton(); } 
            } 
        } 
    } else g_touch_down = 0; 
}

static void Netbar_Publish_SeatState(void) { 
    char payload[96]; char topic[64]; 
    u8 human; u16 smoke_adc; u8 smoke_percent; u8 iu; u32 used_seconds; float fee; u8 alarm_active; 
    if (ESP8266_GetState() != WIFI_STATE_RUNNING) return; 
    human = Seat_Radar_Get(); smoke_adc = Seat_Smoke_GetRaw(); smoke_percent = Smoke_AdcToPercent(smoke_adc); 
    iu = (g_app.state == STATE_INUSE) ? 1 : 0; used_seconds = g_app.used_seconds; fee = (used_seconds / 60.0f) * g_price_per_min; alarm_active = g_idle_occupy_alarm || g_smoke_alarm; 
    sprintf(payload, "s=1;iu=%d;pc=%d;lt=%d;hm=%d;sm=%d;sec=%lu;fee=%.2f;al=%d", iu, g_app.pc_on, g_app.light_on, human, smoke_percent, used_seconds, fee, alarm_active); 
    publish_seq++; sprintf(topic, "netbar/%s/state", g_device_id); ESP8266_MQTT_Pub_Async(topic, payload); 
}

static void UID_HexStr_To_Bytes(const char *str, u8 *out_uid) {
    int i; unsigned int val;
    for (i = 0; i < 4; i++) { if (sscanf(str + i * 2, "%02x", &val) == 1) out_uid[i] = (u8)val; else out_uid[i] = 0; }
}
