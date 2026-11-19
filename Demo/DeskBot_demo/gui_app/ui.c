#include "ui.h"
#include <stdlib.h>     // getenv
#include <string.h>     // strcmp
#include "./pages/ui_HomePage/ui_HomePage.h"
#include "./pages/ui_SettingPage/ui_SettingPage.h"
#include "./pages/ui_WeatherPage/ui_WeatherPage.h"
#include "./pages/ui_CalendarPage/ui_CalendarPage.h"
#include "./pages/ui_MemoPage/ui_MemoPage.h"
#include "./pages/ui_GameMuyuPage/ui_GameMuyuPage.h"
#include "./pages/ui_Game2048Page/ui_Game2048Page.h"
#include "./pages/ui_YOLOPage/ui_YOLOPage.h"
#include "./pages/ui_GameMemoryPage/ui_GameMemoryPage.h"
#include "./pages/ui_DrawPage/ui_DrawPage.h"
#include "./pages/ui_CalculatorPage/ui_CalculatorPage.h"
#include "./pages/ui_MyappPage/ui_MyappPage.h"
#include "./pages/ui_LockPage/ui_LockPage.h"
#include "./pages/ui_VideoPage/ui_VideoPage.h"
#include "./pages/ui_MusicPage/ui_MusicPage.h"
#include "./pages/ui_CameraPage/ui_CameraPage.h"
#include "./pages/ui_AlbumPage/ui_AlbumPage.h"
///////////////////// VARIABLES ////////////////////

lv_lib_pm_t page_manager;

//创建一个 系统配置参数类型 的变量
ui_system_para_t ui_system_para;

//创建一个动画 向右移动
lv_screen_load_anim_t ui_home_enter_anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT;

//存放壁纸的数组
const lv_image_dsc_t * const ui_wallpaper_list[UI_WALLPAPER_NUM] =
{
    &ui_img_bfgril,     // 0
    &ui_img_heidong,    // 1
    &ui_img_wtgril,     // 2
    &ui_img_yueguang,   // 3
};

//壁纸循环的函数 5 -> 1
int ui_wallpaper_clamp(int index)
{
    index %= UI_WALLPAPER_NUM;
    if(index < 0) index += UI_WALLPAPER_NUM;
    return index;
}

//获取当前壁纸索引
const lv_image_dsc_t * ui_wallpaper_get(int index)
{
    return ui_wallpaper_list[ui_wallpaper_clamp(index)];
}

///////////////////// all apps ////////////////////

//app的数量
#define _APP_NUMS 17 

//存放app信息的数组
ui_app_data_t ui_apps[_APP_NUMS] = 
{
    {
        .name = "LockPage",
        .init = ui_LockPage_init,
        .deinit= ui_LockPage_deinit,
        .page_obj = NULL
    }
    ,
    {
        .name = "HomePage",
        .init = ui_HomePage_init,
        .deinit = ui_HomePage_deinit,
        .page_obj = NULL
    },
    {
        .name = "SettingPage",
        .init = ui_SettingPage_init,
        .deinit = ui_SettingPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "WeatherPage",
        .init = ui_WeatherPage_init,
        .deinit = ui_WeatherPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "CalendarPage",
        .init = ui_CalendarPage_init,
        .deinit = ui_CalendarPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "MemoPage",
        .init = ui_MemoPage_init,
        .deinit = ui_MemoPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "GameMuyuPage",
        .init = ui_GameMuyuPage_init,
        .deinit = ui_GameMuyuPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "Game2048Page",
        .init = ui_Game2048Page_init,
        .deinit = ui_Game2048Page_deinit,
        .page_obj = NULL
    },
    {
        // 相机 App（桌面 «相机» 图标 → CameraPage）
        // 板端：编码器变焦 / 按下拍照，照片存 /mnt/media/photos
        .name = "CameraPage",
        .init = ui_CameraPage_init,
        .deinit = ui_CameraPage_deinit,
        .page_obj = NULL
    },
    {
        // 相册 App（桌面 «相册» 图标 → AlbumPage），读 SD 卡里的照片
        .name = "AlbumPage",
        .init = ui_AlbumPage_init,
        .deinit = ui_AlbumPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "YOLOPage",
        .init = ui_YOLOPage_init,
        .deinit = ui_YOLOPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "GameMemoryPage",
        .init = ui_GameMemoryPage_init,
        .deinit = ui_GameMemoryPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "DrawPage",
        .init = ui_DrawPage_init,
        .deinit = ui_DrawPage_deinit,
        .page_obj = NULL
    },
    {
        .name = "CalculatorPage",
        .init = ui_CalculatorPage_init,
        .deinit = ui_CalculatorPage_deinit,
        .page_obj = NULL
    }
    ,
    {
        .name = "MyappPage",
        .init = ui_MyappPage_init,
        .deinit = ui_MyappPage_deinit,
        .page_obj = NULL
    }
    ,
    {
        .name = "VideoPage",
        .init = ui_VideoPage_init,
        .deinit = ui_VideoPage_deinit,
        .page_obj = NULL
    }
    ,
    {
        .name = "MusicPage",
        .init = ui_MusicPage_init,
        .deinit = ui_MusicPage_deinit,
        .page_obj = NULL
    }

};

///////////////////// Function ////////////////////

//消息框关闭按钮被点击的回调函数
static void msgbox_close_click_event_cb(lv_event_t * e)
{
    lv_obj_t * mbox = lv_event_get_target(e);
    bool * mbox_exist = lv_event_get_user_data(e);
    //将消息框是否存在的标志 设置为否
    *mbox_exist = false;
}

//添加消息框函数
void ui_msgbox_info(const char * title, const char * text)
{
    static lv_obj_t * current_mbox;
    static bool mbox_exist = false;
    //如果消息框存在，关闭并将标志设置为 否
    if(mbox_exist)
    {
        lv_msgbox_close(current_mbox);
        mbox_exist = false;
    }
    // 创建新消息框
    current_mbox = lv_msgbox_create(NULL);
    mbox_exist = true;
    lv_obj_t * title_label = lv_msgbox_add_title(current_mbox, title);
    lv_obj_t * text_label  = lv_msgbox_add_text(current_mbox, text);
    // 默认主题字体是 montserrat（没有汉字），中文会变成方块 → 统一换中文字体
    lv_obj_set_style_text_font(title_label, &ui_font_app16, 0);
    lv_obj_set_style_text_font(text_label,  &ui_font_app16, 0);
    //添加消息框关闭按钮
    lv_obj_t * close_btn = lv_msgbox_add_close_button(current_mbox);
    //添加关闭回调函数
    lv_obj_add_event_cb(close_btn, msgbox_close_click_event_cb, LV_EVENT_PRESSED, &mbox_exist);
}

//全局变量初始化函数
static void _sys_para_init(void)
{
    //将壁纸索引设为0
    ui_system_para.wallpaper_index = 0;
    //将锁屏壁纸设置为1
    ui_system_para.lock_wallpaper_index = 1;

    //如果系统参数文件打不开 就初始化系统配置参数
    if(sys_load_system_parameters(sys_config_path, &ui_system_para)!=0)
    {
        LV_LOG_WARN("Load system parameters failed, create a new config file.");
        ui_system_para.year = 2025;
        ui_system_para.month = 1;
        ui_system_para.day = 1;
        ui_system_para.hour = 0;
        ui_system_para.minute = 0;
        ui_system_para.brightness = 50;
        ui_system_para.sound = 50;
        ui_system_para.wifi_connected = false;
        ui_system_para.auto_time = true;
        ui_system_para.auto_location = false;
        strcpy(ui_system_para.location.city, "东城区");
        strcpy(ui_system_para.location.adcode, "110101");
        strcpy(ui_system_para.gaode_api_key, "c93a6793c76bc0d04a931760393c840c");
        ui_system_para.wallpaper_index = 0;
        ui_system_para.lock_wallpaper_index = 1;
        // create a new config file and save
        sys_save_system_parameters(sys_config_path, &ui_system_para);
    }
    // 检测wifi是否可以连接
    ui_system_para.wifi_connected = sys_get_wifi_status();
    // 判断自动获取时间是否打开
    if(ui_system_para.auto_time == true)
    {
        //判断是否能从网络中获取时间 能就用获取的时间设置并日志输出时间 不能就警告
        if(sys_get_time_from_ntp("ntp.aliyun.com", &ui_system_para.year, &ui_system_para.month, &ui_system_para.day, &ui_system_para.hour, &ui_system_para.minute, NULL))
        {
            LV_LOG_WARN("Get time from NTP failed, use system time.");
        }
        else
        {
            sys_set_time(ui_system_para.year, ui_system_para.month, ui_system_para.day, ui_system_para.hour, ui_system_para.minute, 0);
            LV_LOG_USER("Auto NTP time year: %d, month: %d, day: %d, hour: %d, minute: %d", ui_system_para.year, ui_system_para.month, ui_system_para.day, ui_system_para.hour, ui_system_para.minute);
        }
    }
    else
    {
        //直接用系统默认参数时间设置并日志输出
        sys_set_time(ui_system_para.year, ui_system_para.month, ui_system_para.day, ui_system_para.hour, ui_system_para.minute, 0);
        LV_LOG_USER("Manual time year: %d, month: %d, day: %d, hour: %d, minute: %d", ui_system_para.year, ui_system_para.month, ui_system_para.day, ui_system_para.hour, ui_system_para.minute);
    }
    //判断是否打开自动获取位置
    if(ui_system_para.auto_location == true)
    {
        //判断使用高德api通过IP是否能获得定位
        if(sys_get_auto_location_by_ip(&ui_system_para.location, ui_system_para.gaode_api_key) == 0)
        {
            LV_LOG_USER("Auto location city: %s, adcode: %s", ui_system_para.location.city, ui_system_para.location.adcode);
        }
        else
        {
            LV_LOG_WARN("Get location by IP failed, use system location.");
            //通过地域编码获取城市名称
            const char *city_name = sys_get_city_name_by_adcode(city_adcode_path, ui_system_para.location.adcode);
            //复制到系统参数中
            strcpy(ui_system_para.location.city, city_name);
            LV_LOG_USER("Manual location city: %s, adcode: %s", ui_system_para.location.city, ui_system_para.location.adcode);
        }
    }
    else
    {
        const char *city_name = sys_get_city_name_by_adcode(city_adcode_path, ui_system_para.location.adcode);
        strcpy(ui_system_para.location.city, city_name);
        LV_LOG_USER("Manual location city: %s, adcode: %s", ui_system_para.location.city, ui_system_para.location.adcode);
    }
    LV_LOG_USER("System para init done.");
}

//gpio初始化
static void _gpios_init(void)
{
    // GPIO
    gpio_init(LED_BLUE, OUT_DIRECTION);
    // set default value
    gpio_set_value(LED_BLUE, 0);
}

///////////////////// timer //////////////////////

/* 主界面（桌面）无操作多久后自动锁屏，可用环境变量 ECHO_IDLE_LOCK_S 覆盖（单位：秒） */
#define UI_IDLE_LOCK_MS   (60 * 1000)

/* 取当前页面名（无页面时返回空串） */
static const char * _cur_page_name(void)
{
    lv_lib_pm_page_t * p = lv_lib_pm_GetCurrentPage(&page_manager);
    return (p != NULL && p->name != NULL) ? p->name : "";
}

/* 锁屏空闲时间：默认 60 秒，环境变量 ECHO_IDLE_LOCK_S 可改（<=0 表示关闭该功能） */
static uint32_t _idle_lock_ms(void)
{
    static uint32_t cached = 0;
    if(cached == 0)
    {
        const char * env = getenv("ECHO_IDLE_LOCK_S");
        long sec = (env != NULL) ? atol(env) : (long)(UI_IDLE_LOCK_MS / 1000);
        cached = (sec > 0) ? (uint32_t)sec * 1000u : UINT32_MAX;
        LV_LOG_USER("idle lock timeout: %u ms", (unsigned)cached);
    }
    return cached;
}

/*
 * 主界面空闲检测：只在 HomePage 上计时，触碰屏幕或转动编码器都会
 * 刷新 LVGL 的活动时间（见 lv_indev.c），因此无需额外处理输入。
 */
static void _idle_lock_check(void)
{
    static char lock_page_name[] = "LockPage";
    static bool was_on_home = false;

    uint32_t need = _idle_lock_ms();
    bool on_home = (strcmp(_cur_page_name(), "HomePage") == 0);

    if(on_home && need != UINT32_MAX)
    {
        if(!was_on_home)
        {
            /* 刚回到主界面（开机解锁、从别的 app 返回），重新开始计时 */
            lv_display_trigger_activity(NULL);
        }
        else if(lv_obj_get_child_count(lv_layer_top()) > 0)
        {
            /* 有弹窗（msgbox 等挂在顶层）时不锁屏，避免弹窗浮在锁屏上面 */
            lv_display_trigger_activity(NULL);
        }
        else if(lv_display_get_inactive_time(NULL) >= need)
        {
            LV_LOG_USER("HomePage 空闲 %u 秒，自动锁屏", (unsigned)(need / 1000));
            lv_lib_pm_OpenPage(&page_manager, NULL, lock_page_name);
            was_on_home = false;
            return;
        }
    }

    was_on_home = on_home;
}

//主线程时间回调函数
void _maintimer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    static uint16_t time_count2 = 299;
    time_count2++;
    // 每2秒闪烁一次蓝色LED
    if(time_count2 % 2 == 0)
    {
        gpio_set_value(LED_BLUE, 1);
    }
    else
    {
        gpio_set_value(LED_BLUE, 0);
    }
    // 主界面无操作 1 分钟自动锁屏
    _idle_lock_check();
    // 每5分钟保存一次系统参数
    if(time_count2 >= 300)
    {
        ui_system_para.wifi_connected = sys_get_wifi_status();
        if(ui_system_para.auto_time == true)
        {
            if(sys_get_time_from_ntp("ntp.aliyun.com", &ui_system_para.year, &ui_system_para.month, &ui_system_para.day, &ui_system_para.hour, &ui_system_para.minute, NULL))
            {
                LV_LOG_WARN("Get time from NTP failed, use system time.");
            }
            else
            {
                sys_set_time(ui_system_para.year, ui_system_para.month, ui_system_para.day, ui_system_para.hour, ui_system_para.minute, 0);
                LV_LOG_USER("Auto NTP time year: %d, month: %d, day: %d, hour: %d, minute: %d", ui_system_para.year, ui_system_para.month, ui_system_para.day, ui_system_para.hour, ui_system_para.minute);
            }
        }
        else
        {
            sys_set_time(ui_system_para.year, ui_system_para.month, ui_system_para.day, ui_system_para.hour, ui_system_para.minute, 0);
        }
        sys_save_system_parameters(sys_config_path, &ui_system_para);
        time_count2 = 0; 
    }
}

///////////////////// SCREENS ////////////////////

void ui_init(void)
{
    _sys_para_init();
    _gpios_init();
    //获取lvgl全局默认显示屏队形指针
    lv_disp_t * dispp = lv_display_get_default();
    //创建并初始化默认主题
    lv_theme_t * theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),true, LV_FONT_DEFAULT);
    //使用默认主题
    lv_disp_set_theme(dispp, theme);

    lv_lib_pm_Init(&page_manager);
    lv_lib_pm_page_t *pm_page[_APP_NUMS];
    for(int i = 0; i < _APP_NUMS; i++)
    {
        pm_page[i] = lv_lib_pm_CreatePage(&page_manager, ui_apps[i].name, ui_apps[i].init, ui_apps[i].deinit, NULL);
    }
    //开机默认打开锁屏界面
    //调试钩子：ECHO_PAGE=<页面名> 开机直达某一页（无屏/仿真单测某页时最省事）
    static char echo_page_buf[64];
    const char *echo_page = getenv("ECHO_PAGE");
    if(echo_page != NULL && echo_page[0] != 0)
    {
        snprintf(echo_page_buf, sizeof(echo_page_buf), "%s", echo_page);
        LV_LOG_USER("ECHO_PAGE -> %s", echo_page_buf);
        lv_lib_pm_OpenPage(&page_manager, NULL, echo_page_buf);
    }
    else
    {
        lv_lib_pm_OpenPage(&page_manager, NULL, "LockPage");
    }
    //lv_lib_pm_OpenPrePage(&page_manager);
    lv_timer_create(_maintimer_cb, 1000, NULL);
}