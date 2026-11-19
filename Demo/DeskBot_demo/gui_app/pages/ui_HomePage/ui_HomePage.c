#include "ui_HomePage.h"

#include <stdio.h>
#include <stdlib.h>

///////////////////// VARIABLES ////////////////////

#define _APP_CONTAINER_MAX_PAGES 4

//桌面页面的数据（结构体）
ui_desktop_data_t ui_desktop_data = {
    .witdh = 320,                                       //桌面UI宽度
    .height = 240,                                      //桌面UI高度
    .container_total_pages = _APP_CONTAINER_MAX_PAGES,  //应用图标容器总页数
    .app_container_index = 0,                           //当前应用容器所在页码（索引）
    .show_dropdown = false,                             //是否显示下拉菜单
    .scroll_busy = false                                //滚动是都正在进行（滚动忙标志）
};
//桌面页面小圆点的数量
lv_obj_t * ui_ScrollDots[_APP_CONTAINER_MAX_PAGES];

//桌面的时间  wifi
lv_timer_t * ui_home_timer;
lv_obj_t * ui_WifiLabel;
lv_obj_t * ui_NoWifiLabel;

//滑动看门狗：动画被打断时 scroll_busy 可能永远为 true，必须超时强制复位
#define UI_SCROLL_BUSY_TIMEOUT_MS 500
static lv_obj_t * ui_app_container = NULL;
static lv_timer_t * ui_scroll_wd_timer = NULL;

//下拉面板里的 WiFi 按钮（开/关时换背景色表示高亮）
static lv_obj_t * ui_WifiBtnRef = NULL;
static void _wifi_btn_set_highlight(bool on)
{
    if(ui_WifiBtnRef == NULL) return;
    lv_obj_set_style_bg_color(ui_WifiBtnRef,
                              lv_color_hex(on ? 0x2F7BEA : 0x808080),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
}


//////////////////////// Timer //////////////////////

//检测wifi是否连接成功，从而显示不同的wifi图标
static void _wifi_connected_icon_set(bool connected)
{
    //成功显示wifi成功图标
    if(connected)
    {
        lv_obj_remove_flag(ui_WifiLabel, LV_OBJ_FLAG_HIDDEN);     /// Flags
        lv_obj_add_flag(ui_NoWifiLabel, LV_OBJ_FLAG_HIDDEN);     /// Flags
    }
    //失败显示wifi失败图标
    else
    {
        lv_obj_remove_flag(ui_NoWifiLabel, LV_OBJ_FLAG_HIDDEN);     /// Flags
        lv_obj_add_flag(ui_WifiLabel, LV_OBJ_FLAG_HIDDEN);     /// Flags
    }
}

//主页面时间展示函数
static void ui_home_timer_cb(lv_timer_t * timer)
{
    //获取用户数据中的时间，用变量存储
    lv_obj_t * timelabel = lv_timer_get_user_data(timer);
    int year; int month; int day; int hour; int minute; int second;
    //获取系统时间
    sys_get_time(&year, &month, &day, &hour, &minute, &second);
    //如果获取的时间与系统时间不同，就重新将时间写入系统（做到分钟时间更新）
    if(minute!=ui_system_para.minute)
    {
        char time_str[6];
        sprintf(time_str, "%02d:%02d", hour, minute);
        lv_label_set_text(timelabel, time_str);
        ui_system_para.year = year;
        ui_system_para.month = month;
        ui_system_para.day = day;
        ui_system_para.hour = hour;
        ui_system_para.minute = minute;
    }
    //通过wifi连接情况，显示wifi图标
    _wifi_connected_icon_set(ui_system_para.wifi_connected);
}

///////////////////// ANIMATIONS ////////////////////

//ui动画完成后的回调函数，是否允许滑动
static void _ui_anim_completed_cb(lv_anim_t * anim)
{
    (void)anim;
    ui_desktop_data.scroll_busy = false;
    ui_desktop_data.scroll_busy_since = 0;
}

//scroll_busy 看门狗：动画完成回调不一定总是能触发（页面切换/对象删除会
//直接删掉动画），标志卡住会导致之后所有手势和点击都失效，这里超时强制复位
static void ui_scroll_watchdog_cb(lv_timer_t * timer)
{
    (void)timer;
    if(!ui_desktop_data.scroll_busy || ui_desktop_data.scroll_busy_since == 0) return;
    if(lv_tick_elaps(ui_desktop_data.scroll_busy_since) < UI_SCROLL_BUSY_TIMEOUT_MS) return;

    LV_LOG_WARN("scroll_busy timeout, force reset");
    ui_desktop_data.scroll_busy = false;
    ui_desktop_data.scroll_busy_since = 0;

    //把没跑完的动画停掉，并把容器对齐到当前页，避免半页错位
    if(ui_app_container != NULL)
    {
        lv_anim_delete(ui_app_container, lv_lib_anim_callback_set_x);
        lv_obj_set_x(ui_app_container,
                     -(int32_t)ui_desktop_data.app_container_index * ui_desktop_data.witdh);
    }
}

//向左移动动画，
static void AppContLeft_Animation(lv_obj_t * TargetObject, int delay)
{
    int32_t x_pos_now = lv_obj_get_x(TargetObject);
    lv_lib_anim_user_animation(TargetObject, 0, 100, x_pos_now, x_pos_now - ui_desktop_data.witdh, 0, 0, 0, 0, lv_anim_path_ease_out, lv_lib_anim_callback_set_x, _ui_anim_completed_cb);
}

//向右移动动画
static void AppContRight_Animation(lv_obj_t * TargetObject, int delay)
{
    int32_t x_pos_now = lv_obj_get_x(TargetObject);
    lv_lib_anim_user_animation(TargetObject, 0, 100, x_pos_now, x_pos_now + ui_desktop_data.witdh, 0, 0, 0, 0, lv_anim_path_ease_out, lv_lib_anim_callback_set_x, _ui_anim_completed_cb);
}

///////////////////// FUNCTIONS ////////////////////

//下滑页面展示函数
static void ui_event_TopDrag(lv_event_t * e)
{
    //获取触发当前函数的事件
    lv_event_code_t event_code = lv_event_get_code(e);
    //获取是什么组件触发了回调函数
    lv_obj_t * obj = lv_event_get_target(e);
    //获取触发回调函数传入的参数
    lv_obj_t * dropdown_panel = lv_event_get_user_data(e);

    //判断触发事件的方式是不是按下
    if(event_code == LV_EVENT_PRESSING) {
        //获取当前正在处理的输入设备
        lv_indev_t * indev = lv_indev_active();
        if(indev == NULL)  return;

        //获取当前触摸相对于上一次位移的值
        lv_point_t vect;
        lv_indev_get_vect(indev, &vect);

        //获取当前高度的坐标
        int32_t y = lv_obj_get_y_aligned(obj) + vect.y;
        //设置下拉页面的位置跟随下滑位置
        lv_obj_set_y(obj, y); 
        //设置下滑页面的内容下滑位置
        lv_obj_set_y(dropdown_panel, y-ui_desktop_data.height); // set dorp down panel
    }
//判断触发事件是否为释放事件
    else if(event_code == LV_EVENT_RELEASED) {
        //获取页面设置的y的位置
        int32_t y = lv_obj_get_y_aligned(obj);
        //判断下滑页面是否展示，下滑位移是否大于屏幕三分之一，从而判断是否允许展示下滑页面
        if(!ui_desktop_data.show_dropdown && y >= ui_desktop_data.height/3)
        {
            lv_obj_set_y(dropdown_panel, 0);
            lv_obj_set_y(obj, ui_desktop_data.height-lv_obj_get_height(obj));
            ui_desktop_data.show_dropdown = true;
        }
        else if(ui_desktop_data.show_dropdown && y <= ui_desktop_data.height * 2/3)
        {
            lv_obj_set_y(dropdown_panel, -ui_desktop_data.height);
            lv_obj_set_y(obj, 0);
            ui_desktop_data.show_dropdown = false;
        }
    }
}

//实现桌面页面滑动的函数
static void ui_event_scroll_gesture(lv_event_t * e)
{
    //获取事件触发的代码
    lv_event_code_t event_code = lv_event_get_code(e);
    //获取触发回调函数传入的参数
    lv_obj_t * AppContainer = lv_event_get_user_data(e);
    //判断触发是否由手势事件触发，桌面滑动是否繁忙，桌面是否展示下滑页面
    if(event_code == LV_EVENT_GESTURE && !ui_desktop_data.scroll_busy && !ui_desktop_data.show_dropdown) {
        //判断手势事件是否是左滑事件
        if(lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_LEFT)
        {   
            //判断当前桌面页面是否小于桌面页面规定最大索引
            if(ui_desktop_data.app_container_index < ui_desktop_data.container_total_pages-1)
            {
                //将桌面设置为滑动繁忙，不能滑动
                ui_desktop_data.scroll_busy = true;
                ui_desktop_data.scroll_busy_since = lv_tick_get();
                //调用页面左移动画
                AppContLeft_Animation(AppContainer, 0);
                //设置当前桌面页面对应的小圆点和上一个小圆点的透明度
                lv_obj_set_style_bg_opa(ui_ScrollDots[ui_desktop_data.app_container_index], 96, LV_PART_MAIN | LV_STATE_DEFAULT);
                ui_desktop_data.app_container_index++;
                lv_obj_set_style_bg_opa(ui_ScrollDots[ui_desktop_data.app_container_index], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }
        //判断滑动事件是否为右滑时间
        else if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_RIGHT)
        {
            // not in the first page
            if(ui_desktop_data.app_container_index > 0)
            {
                ui_desktop_data.scroll_busy = true;
                ui_desktop_data.scroll_busy_since = lv_tick_get();
                AppContRight_Animation(AppContainer, 0);
                lv_obj_set_style_bg_opa(ui_ScrollDots[ui_desktop_data.app_container_index], 96, LV_PART_MAIN | LV_STATE_DEFAULT);
                ui_desktop_data.app_container_index--;
                lv_obj_set_style_bg_opa(ui_ScrollDots[ui_desktop_data.app_container_index], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }
    }
}

//app按下后的回调函数
static void ui_event_AppsBtn(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    char * pagename = lv_event_get_user_data(e);
    if(event_code == LV_EVENT_CLICKED && !ui_desktop_data.scroll_busy) 
    {
        //打开app对应的页面
        lv_lib_pm_OpenPage(&page_manager, NULL, pagename);
    }
}

// 下拉面板里的 WiFi 按钮：点一下开始（后台反复重试）连接 WiFi，再点一下关闭
static void ui_event_WifiBtn(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if(sys_wifi_power_is_on())
    {
        sys_wifi_power_off();
        _wifi_btn_set_highlight(false);
        printf("[HomePage] WiFi 关闭\n");
    }
    else
    {
        sys_wifi_power_on();
        _wifi_btn_set_highlight(true);
        printf("[HomePage] WiFi 开启，后台开始重试连接\n");
    }
}

// 无屏自检：ECHO_WIFI_SELFTEST=1 时 2 秒后点一次（开）、6 秒后再点一次（关）
static void _wifi_selftest_cb(lv_timer_t * t)
{
    static int step = 0;
    if(step == 0)
    {
        printf("[selftest] 点 WiFi（开启）\n");
        if(ui_WifiBtnRef) lv_obj_send_event(ui_WifiBtnRef, LV_EVENT_CLICKED, NULL);
    }
    else if(step == 2)
    {
        printf("[selftest] 再点 WiFi（关闭）\n");
        if(ui_WifiBtnRef) lv_obj_send_event(ui_WifiBtnRef, LV_EVENT_CLICKED, NULL);
    }
    else
    {
        printf("[selftest] 结束\n");
        lv_timer_delete(t);
        return;
    }
    step += 2;                  /* 0 → 2 → 结束（2 秒一步） */
}

// 音量滚动条回调函数
static void ui_event_SoundSlider(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    if(event_code == LV_EVENT_RELEASED)
    {
        //获取当前滚动条的值
        ui_system_para.sound = lv_slider_get_value(slider);
        //设置音量为当前值
        sys_set_volume(ui_system_para.sound);
    }
}

//亮度滚动条回调函数（与音量滚动条大差不差）
static void ui_event_LightSlider(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    if(event_code == LV_EVENT_VALUE_CHANGED)
    {
        ui_system_para.brightness = lv_slider_get_value(slider);
        sys_set_lcd_brightness(ui_system_para.brightness);
    }
}

// 下滑页面的设置按钮的回调函数
static void ui_event_SetBtn(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if(event_code == LV_EVENT_CLICKED)
    {
        lv_lib_pm_OpenPage(&page_manager, NULL, "SettingPage");   
    }
}

///////////////////// app 名字标签 ////////////////////

// 在 app 图标正下方加一个名字标签（与图标同宽、居中）
static void _app_name_label_create(lv_obj_t * btn, const char * name)
{
    lv_obj_t * label = lv_label_create(lv_obj_get_parent(btn));
    lv_label_set_text(label, name);
    lv_obj_set_width(label, 70);            // 和图标一样宽，文字居中
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &ui_font_app16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    // 半透明黑底：壁纸是照片，纯白字在浅色区域会看不清
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(label, 100, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(label, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_hor(label, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    // 贴在图标下方 1px（两行图标之间有 30px 空隙，放得下 16px 的字）
    lv_obj_align_to(label, btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 1);
}

///////////////////// SCREEN init ////////////////////

//主界面初始化函数
void ui_HomePage_init(void)
{
    // 主界面基本设置，时间获取
    ui_desktop_data.scroll_busy = false;
    ui_desktop_data.scroll_busy_since = 0;
    ui_desktop_data.show_dropdown = false;
    int year; int month; int day; int hour; int minute; int second;
    sys_get_time(&year, &month, &day, &hour, &minute, &second);
    ui_system_para.hour = hour;
    ui_system_para.minute = minute;
    ui_system_para.year = year;
    ui_system_para.month = month;
    ui_system_para.day = day;
    // ui_system_para.wifi_connected = sys_get_wifi_status();

    // 创建活动屏幕
    lv_obj_t * ui_HomeScreen = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_HomeScreen, LV_OBJ_FLAG_SCROLLABLE);      /// Flags

    // 创建ui应用图标容器
    lv_obj_t * ui_AppIconContainer = lv_obj_create(ui_HomeScreen);
    ui_app_container = ui_AppIconContainer;
    //设置容器宽度为 桌面单个页面的宽度 * 桌面页面数量
    lv_obj_set_width(ui_AppIconContainer, ui_desktop_data.witdh * ui_desktop_data.container_total_pages);
    //设置容器高度为 桌面的高度
    lv_obj_set_height(ui_AppIconContainer, ui_desktop_data.height);
    //设置容器的水平位置为 app所在页码索引 * 桌面UI宽度
    lv_obj_set_x(ui_AppIconContainer, -ui_desktop_data.app_container_index * ui_desktop_data.witdh);
    //设置容器位置为 左边居中
    lv_obj_set_align(ui_AppIconContainer, LV_ALIGN_LEFT_MID);
    //移除应用图标容器对象的可滚动标志
    lv_obj_remove_flag(ui_AppIconContainer, LV_OBJ_FLAG_SCROLLABLE);
    //设置容器背景颜色为白色(主体和默认状态)
    lv_obj_set_style_bg_color(ui_AppIconContainer, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    //设置容器背景透明度为0
    lv_obj_set_style_bg_opa(ui_AppIconContainer, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    //设置应用图标容器主部件的边框颜色为黑色（主体和默认状态）
    lv_obj_set_style_border_color(ui_AppIconContainer, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    //设置应用图标容器主部件的边框透明度为0（主体和默认状态）
    lv_obj_set_style_border_opa(ui_AppIconContainer, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // 设置桌面容器的背景图片为 壁纸app中所选中的壁纸（主体和默认状态）
    lv_obj_set_style_bg_image_src(ui_HomeScreen, ui_wallpaper_get(ui_system_para.wallpaper_index), LV_PART_MAIN | LV_STATE_DEFAULT);
    // 添加回调函数 滑动触发页面切换
    lv_obj_add_event_cb(ui_HomeScreen, ui_event_scroll_gesture, LV_EVENT_GESTURE, ui_AppIconContainer);

    // 时间文本
    lv_obj_t * ui_TimeLabel = lv_label_create(ui_HomeScreen);
    //设置时间标签宽度为内容自适应大小（根据文字内容自动适配宽度）
    lv_obj_set_width(ui_TimeLabel, LV_SIZE_CONTENT);
    //设置时间标签高度为内容自适应
    lv_obj_set_height(ui_TimeLabel, LV_SIZE_CONTENT);
    //设置时间文本的水平位置为0
    lv_obj_set_x(ui_TimeLabel, 0);
    //设置时间文本的垂直位置为5
    lv_obj_set_y(ui_TimeLabel, 5);
    //设置时间文本的位置为 顶部居中
    lv_obj_set_align(ui_TimeLabel, LV_ALIGN_TOP_MID);
    //设置时间文本内容为 11：59（无网模式）
    lv_label_set_text(ui_TimeLabel, "11:59");
    //设置时间文本的字体
    lv_obj_set_style_text_font(ui_TimeLabel, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    //将系统的时间数据写入 变量time_str 并重设时间文本内容（有网模式）
    char time_str[6];
    sprintf(time_str, "%02d:%02d", ui_system_para.hour, ui_system_para.minute);
    lv_label_set_text(ui_TimeLabel, time_str);

    //wifi链接成功图标
    ui_WifiLabel = lv_label_create(ui_HomeScreen);
    lv_obj_set_width(ui_WifiLabel, LV_SIZE_CONTENT);   
    lv_obj_set_height(ui_WifiLabel, LV_SIZE_CONTENT);  
    lv_obj_set_x(ui_WifiLabel, 130);
    lv_obj_set_y(ui_WifiLabel, 7);
    lv_obj_set_align(ui_WifiLabel, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_WifiLabel, "");
    lv_obj_set_style_text_font(ui_WifiLabel, &ui_font_iconfont26, LV_PART_MAIN | LV_STATE_DEFAULT);

    // wifi连接失败图标
    ui_NoWifiLabel = lv_label_create(ui_HomeScreen);
    lv_obj_set_width(ui_NoWifiLabel, LV_SIZE_CONTENT);   
    lv_obj_set_height(ui_NoWifiLabel, LV_SIZE_CONTENT);  
    lv_obj_set_x(ui_NoWifiLabel, 130);
    lv_obj_set_y(ui_NoWifiLabel, 7);
    lv_obj_set_align(ui_NoWifiLabel, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_NoWifiLabel, "");
    lv_obj_set_style_text_font(ui_NoWifiLabel, &ui_font_iconfont26, LV_PART_MAIN | LV_STATE_DEFAULT);
    if(ui_system_para.wifi_connected == true)
    {
        lv_obj_remove_flag(ui_WifiLabel, LV_OBJ_FLAG_HIDDEN); 
        lv_obj_add_flag(ui_NoWifiLabel, LV_OBJ_FLAG_HIDDEN);  
    }
    else
    {
        lv_obj_remove_flag(ui_NoWifiLabel, LV_OBJ_FLAG_HIDDEN); 
        lv_obj_add_flag(ui_WifiLabel, LV_OBJ_FLAG_HIDDEN);     
    }

    // 小圆点创建（根据app页面容器数量来创建个数）
    for(int i = 0; i < _APP_CONTAINER_MAX_PAGES; i++)
    {
        int16_t start_pos;
        //通过判断app页面容器的最大页面数量的奇偶数 来设置起始小圆点的位置
        if(_APP_CONTAINER_MAX_PAGES%2==0)
        {
            start_pos = -20*(_APP_CONTAINER_MAX_PAGES/2) + 10;
        }
        else
        {
            start_pos = -20*(_APP_CONTAINER_MAX_PAGES/2 + 1) + 20;
        }
        ui_ScrollDots[i] = lv_obj_create(ui_HomeScreen);
        lv_obj_set_width(ui_ScrollDots[i], 8);
        lv_obj_set_height(ui_ScrollDots[i], 8);
        //设置小圆点的水平位置 为 起始位置 + 索引 * 20
        lv_obj_set_x(ui_ScrollDots[i], start_pos + 20 * i);
        // 贴到屏幕最下边，给第二行 app 的名字标签让位
        lv_obj_set_y(ui_ScrollDots[i], -2);
        lv_obj_set_align(ui_ScrollDots[i], LV_ALIGN_BOTTOM_MID);
        lv_obj_remove_flag(ui_ScrollDots[i], LV_OBJ_FLAG_SCROLLABLE);
        //设置小圆点的圆度
        lv_obj_set_style_radius(ui_ScrollDots[i], 20, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_ScrollDots[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_ScrollDots[i], 96, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_ScrollDots[i], lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(ui_ScrollDots[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    //设置所在app页面的小圆点的透明度
    lv_obj_set_style_bg_opa(ui_ScrollDots[ui_desktop_data.app_container_index], 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 下滑页面创建
    lv_obj_t * ui_DropdownPanel = lv_obj_create(ui_HomeScreen);
    lv_obj_set_width(ui_DropdownPanel, ui_desktop_data.witdh);
    lv_obj_set_height(ui_DropdownPanel, ui_desktop_data.height);
    lv_obj_set_x(ui_DropdownPanel, 0);
    //如果允许展示下拉菜单就将下拉菜单垂直位置设为桌面UI高度
    if( ui_desktop_data.show_dropdown == true)
    {
        lv_obj_set_y(ui_DropdownPanel, ui_desktop_data.height);
    }
    lv_obj_set_y(ui_DropdownPanel, -ui_desktop_data.height);
    lv_obj_set_align(ui_DropdownPanel, LV_ALIGN_TOP_MID);
    // 移除下拉面板对象的：可滚动、弹性滚动、滚动链式传递标记
    lv_obj_remove_flag(ui_DropdownPanel,LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_CHAIN);
    //设置下拉菜单的滚动条模式为 始终显示滚动条
    lv_obj_set_scrollbar_mode(ui_DropdownPanel, LV_SCROLLBAR_MODE_ON);
    //设置下拉菜单仅支持垂直方向滚动
    lv_obj_set_scroll_dir(ui_DropdownPanel, LV_DIR_VER);
    lv_obj_set_style_bg_color(ui_DropdownPanel, lv_color_hex(0x323232), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_DropdownPanel, 242, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_DropdownPanel, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_DropdownPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    //亮度滚动条创建
    lv_obj_t * ui_LightSlider = lv_slider_create(ui_DropdownPanel);
    //设置滚动条的初始值为 50
    lv_slider_set_value(ui_LightSlider, 50, LV_ANIM_OFF);
    // 如果亮度滑块是范围选择模式，则将滑块左游标的值设置为0，关闭动画
    if(lv_slider_get_mode(ui_LightSlider) == LV_SLIDER_MODE_RANGE) lv_slider_set_left_value(ui_LightSlider, 0, LV_ANIM_OFF);
    //设置滚动条的值为 系统配置的亮度 关闭动画
    lv_slider_set_value(ui_LightSlider, ui_system_para.brightness, LV_ANIM_OFF);
    lv_obj_set_width(ui_LightSlider, 50);
    lv_obj_set_height(ui_LightSlider, 150);
    lv_obj_set_x(ui_LightSlider, 50);
    lv_obj_set_y(ui_LightSlider, -10);
    lv_obj_set_align(ui_LightSlider, LV_ALIGN_CENTER);
    //亮度滚动条主体样式
    lv_obj_set_style_radius(ui_LightSlider, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_LightSlider, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_LightSlider, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    //亮度滚动条 圆点样式
    lv_obj_set_style_radius(ui_LightSlider, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_LightSlider, lv_color_hex(0x3264C8), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_LightSlider, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    //亮度滚动条 滚动条样式
    lv_obj_set_style_radius(ui_LightSlider, 0, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_LightSlider, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_LightSlider, 0, LV_PART_KNOB | LV_STATE_DEFAULT);
    //添加回调函数
    lv_obj_add_event_cb(ui_LightSlider, ui_event_LightSlider, LV_EVENT_VALUE_CHANGED, NULL);
    //Compensating for LVGL9.1 draw crash with bar/slider max value when top-padding is nonzero and right-padding is 0
    if(lv_obj_get_style_pad_top(ui_LightSlider, LV_PART_MAIN) > 0) lv_obj_set_style_pad_right(ui_LightSlider,lv_obj_get_style_pad_right(ui_LightSlider, LV_PART_MAIN) + 1, LV_PART_MAIN);
    //亮度图标设置
    lv_obj_t * ui_LightIcon = lv_label_create(ui_LightSlider);
    lv_obj_set_width(ui_LightIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_LightIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_LightIcon, 1);
    lv_obj_set_y(ui_LightIcon, 50);
    lv_obj_set_align(ui_LightIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_LightIcon, "");
    lv_obj_set_style_text_color(ui_LightIcon, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_LightIcon, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_LightIcon, &ui_font_iconfont44, LV_PART_MAIN | LV_STATE_DEFAULT);

    //音量滚动条创建
    lv_obj_t * ui_SoundSlider = lv_slider_create(ui_DropdownPanel);
    lv_slider_set_value(ui_SoundSlider, 50, LV_ANIM_OFF);
    if(lv_slider_get_mode(ui_SoundSlider) == LV_SLIDER_MODE_RANGE) lv_slider_set_left_value(ui_SoundSlider, 0, LV_ANIM_OFF);
    lv_slider_set_value(ui_SoundSlider, ui_system_para.sound, LV_ANIM_OFF);
    lv_obj_set_width(ui_SoundSlider, 50);
    lv_obj_set_height(ui_SoundSlider, 150);
    lv_obj_set_x(ui_SoundSlider, 115);
    lv_obj_set_y(ui_SoundSlider, -10);
    lv_obj_set_align(ui_SoundSlider, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(ui_SoundSlider, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_SoundSlider, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_SoundSlider, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_radius(ui_SoundSlider, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_SoundSlider, lv_color_hex(0x3264C8), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_SoundSlider, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_obj_set_style_radius(ui_SoundSlider, 0, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_SoundSlider, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_SoundSlider, 0, LV_PART_KNOB | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_SoundSlider, ui_event_SoundSlider, LV_EVENT_RELEASED, NULL);

    //Compensating for LVGL9.1 draw crash with bar/slider max value when top-padding is nonzero and right-padding is 0
    if(lv_obj_get_style_pad_top(ui_SoundSlider, LV_PART_MAIN) > 0) lv_obj_set_style_pad_right(ui_SoundSlider,
                                                                                                  lv_obj_get_style_pad_right(ui_SoundSlider, LV_PART_MAIN) + 1, LV_PART_MAIN);
    lv_obj_t * ui_SoundIcon = lv_label_create(ui_SoundSlider);
    lv_obj_set_width(ui_SoundIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_SoundIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_SoundIcon, 1);
    lv_obj_set_y(ui_SoundIcon, 50);
    lv_obj_set_align(ui_SoundIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_SoundIcon, "");
    lv_obj_set_style_text_color(ui_SoundIcon, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_SoundIcon, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_SoundIcon, &ui_font_iconfont36, LV_PART_MAIN | LV_STATE_DEFAULT);

    //wifi按钮创建
    lv_obj_t * ui_WifiBtn = lv_button_create(ui_DropdownPanel);
    lv_obj_set_width(ui_WifiBtn, 60);
    lv_obj_set_height(ui_WifiBtn, 60);
    lv_obj_set_x(ui_WifiBtn, -100);
    lv_obj_set_y(ui_WifiBtn, -50);
    lv_obj_set_align(ui_WifiBtn, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_WifiBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_WifiBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_WifiBtn, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_WifiBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_DropWifiIcon = lv_label_create(ui_WifiBtn);
    lv_obj_set_width(ui_DropWifiIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_DropWifiIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_DropWifiIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_DropWifiIcon, "");
    lv_obj_set_style_text_font(ui_DropWifiIcon, &ui_font_iconfont36, LV_PART_MAIN | LV_STATE_DEFAULT);

    // WiFi 按钮：按一下开始（后台反复重试）连接，再按一下关闭；开着时按钮高亮
    ui_WifiBtnRef = ui_WifiBtn;
    sys_wifi_sync_state();                      /* S99 已拉起 WiFi 的话，显示为“开” */
    _wifi_btn_set_highlight(sys_wifi_power_is_on());
    {
        const char * _wst = getenv("ECHO_WIFI_SELFTEST");
        if(_wst && _wst[0] == '1')
        {
            printf("[HomePage] WiFi 自检开始\n");
            lv_timer_create(_wifi_selftest_cb, 2000, NULL);
        }
    }
    lv_obj_add_event_cb(ui_WifiBtn, ui_event_WifiBtn, LV_EVENT_CLICKED, NULL);
    
    //蓝牙按钮创建
    lv_obj_t * ui_BLEBtn = lv_button_create(ui_DropdownPanel);
    lv_obj_set_width(ui_BLEBtn, 60);
    lv_obj_set_height(ui_BLEBtn, 60);
    lv_obj_set_x(ui_BLEBtn, -25);
    lv_obj_set_y(ui_BLEBtn, -50);
    lv_obj_set_align(ui_BLEBtn, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_BLEBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_BLEBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_BLEBtn, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_BLEBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_DropBLEIcon = lv_label_create(ui_BLEBtn);
    lv_obj_set_width(ui_DropBLEIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_DropBLEIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_DropBLEIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_DropBLEIcon, "");
    lv_obj_set_style_text_font(ui_DropBLEIcon, &ui_font_iconfont36, LV_PART_MAIN | LV_STATE_DEFAULT);

    //设置按钮创建
    lv_obj_t * ui_DropSetBtn = lv_button_create(ui_DropdownPanel);
    lv_obj_set_width(ui_DropSetBtn, 60);
    lv_obj_set_height(ui_DropSetBtn, 60);
    lv_obj_set_x(ui_DropSetBtn, -100);
    lv_obj_set_y(ui_DropSetBtn, 30);
    lv_obj_set_align(ui_DropSetBtn, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_DropSetBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_DropSetBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_DropSetBtn, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_DropSetBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_DropSetBtn, ui_event_SetBtn, LV_EVENT_CLICKED, "SettingPage");

    lv_obj_t * ui_DropSetIcon = lv_label_create(ui_DropSetBtn);
    lv_obj_set_width(ui_DropSetIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_DropSetIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_DropSetIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_DropSetIcon, "");
    lv_obj_set_style_text_font(ui_DropSetIcon, &ui_font_iconfont36, LV_PART_MAIN | LV_STATE_DEFAULT);

    //主界面按钮创建
    lv_obj_t * ui_HomeBtn = lv_button_create(ui_DropdownPanel);
    lv_obj_set_width(ui_HomeBtn, 60);
    lv_obj_set_height(ui_HomeBtn, 60);
    lv_obj_set_x(ui_HomeBtn, -25);
    lv_obj_set_y(ui_HomeBtn, 30);
    lv_obj_set_align(ui_HomeBtn, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_HomeBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_HomeBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_HomeBtn, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_HomeBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_DropHomeIcon = lv_label_create(ui_HomeBtn);
    lv_obj_set_width(ui_DropHomeIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_DropHomeIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_DropHomeIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_DropHomeIcon, "");
    lv_obj_set_style_text_font(ui_DropHomeIcon, &ui_font_iconfont36, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 顶部下拉页面创建
    lv_obj_t * ui_TopDragPanel = lv_obj_create(ui_HomeScreen);
    lv_obj_set_width(ui_TopDragPanel, ui_desktop_data.witdh);
    lv_obj_set_height(ui_TopDragPanel, 30);
    lv_obj_set_align(ui_TopDragPanel, LV_ALIGN_TOP_MID);
    lv_obj_remove_flag(ui_TopDragPanel, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_TopDragPanel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_TopDragPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_TopDragPanel, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_TopDragPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // top drag event
    lv_obj_add_event_cb(ui_TopDragPanel, ui_event_TopDrag, LV_EVENT_PRESSING, ui_DropdownPanel);
    lv_obj_add_event_cb(ui_TopDragPanel, ui_event_TopDrag, LV_EVENT_RELEASED, ui_DropdownPanel);


    // setting app
    lv_obj_t * ui_SettingBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_SettingBtn, 70);
    lv_obj_set_height(ui_SettingBtn, 70);
    lv_obj_set_x(ui_SettingBtn, 15);
    lv_obj_set_y(ui_SettingBtn, -45);
    lv_obj_set_align(ui_SettingBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_SettingBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_SettingBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_SettingBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_SettingBtn, lv_color_hex(0x00FFDE), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_SettingIcon = lv_label_create(ui_SettingBtn);
    lv_obj_set_width(ui_SettingIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_SettingIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_SettingIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_SettingIcon, "");
    lv_obj_set_style_text_font(ui_SettingIcon, &ui_font_iconfont48, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_SettingBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "SettingPage");
    _app_name_label_create(ui_SettingBtn, "设置");


    // weather app
    lv_obj_t * ui_WeatherBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_WeatherBtn, 70);
    lv_obj_set_height(ui_WeatherBtn, 70);
    lv_obj_set_x(ui_WeatherBtn, 110);
    lv_obj_set_y(ui_WeatherBtn, -45);
    lv_obj_set_align(ui_WeatherBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_WeatherBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_WeatherBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_WeatherBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_WeatherBtn, &ui_img_weather64_png, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_WeatherBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "WeatherPage");
    _app_name_label_create(ui_WeatherBtn, "天气");


    // calendar app
    lv_obj_t * ui_CalendarBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_CalendarBtn, 70);
    lv_obj_set_height(ui_CalendarBtn, 70);
    lv_obj_set_x(ui_CalendarBtn, 205);
    lv_obj_set_y(ui_CalendarBtn, -45);
    lv_obj_set_align(ui_CalendarBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_CalendarBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_CalendarBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_CalendarBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_CalendarBtn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_CalendarBtn, 224, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_CalendarBtn, &ui_img_calendar64_png, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_DateLabel = lv_label_create(ui_CalendarBtn);
    lv_obj_set_width(ui_DateLabel, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_DateLabel, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_DateLabel, 0);
    lv_obj_set_y(ui_DateLabel, 8);
    lv_obj_set_align(ui_DateLabel, LV_ALIGN_CENTER);
    lv_label_set_text(ui_DateLabel, "7");
    char date_str[2];
    sprintf(date_str, "%d", ui_system_para.day);
    lv_label_set_text(ui_DateLabel, date_str);
    lv_obj_set_style_text_color(ui_DateLabel, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_DateLabel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_DateLabel, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    // calendar event
    lv_obj_add_event_cb(ui_CalendarBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "CalendarPage");
    _app_name_label_create(ui_CalendarBtn, "日历");

    // Camera app
    lv_obj_t * ui_CameraBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_CameraBtn, 70);
    lv_obj_set_height(ui_CameraBtn, 70);
    lv_obj_set_x(ui_CameraBtn, 15);
    lv_obj_set_y(ui_CameraBtn, 55);
    lv_obj_set_align(ui_CameraBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_CameraBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_CameraBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_CameraBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_CameraBtn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_CameraBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_CameraBtn, &ui_img_carame_data, LV_PART_MAIN | LV_STATE_DEFAULT);

    // event
    lv_obj_add_event_cb(ui_CameraBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "CameraPage");
    _app_name_label_create(ui_CameraBtn, "相机");

    /* 相册 App：第 3 页第 2 行（x=655, y=+55），读 SD 卡里的照片 */
    lv_obj_t * ui_AlbumBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_AlbumBtn, 70);
    lv_obj_set_height(ui_AlbumBtn, 70);
    lv_obj_set_x(ui_AlbumBtn, 110);
    lv_obj_set_y(ui_AlbumBtn, 55);
    lv_obj_set_align(ui_AlbumBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_AlbumBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_AlbumBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_AlbumBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_AlbumBtn, lv_color_hex(0x2F4858), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_AlbumBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_AlbumBtn, &ui_img_album_data, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_AlbumBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "AlbumPage");
    _app_name_label_create(ui_AlbumBtn, "相册");


        // YOLO app
    lv_obj_t * ui_YOLOBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_YOLOBtn, 70);
    lv_obj_set_height(ui_YOLOBtn, 70);
    lv_obj_set_x(ui_YOLOBtn, 205);
    lv_obj_set_y(ui_YOLOBtn, 55);
    lv_obj_set_align(ui_YOLOBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_YOLOBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_YOLOBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_YOLOBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_YOLOBtn, lv_color_hex(0xEEEF48), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_YOLOBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_YOLOLabel = lv_label_create(ui_YOLOBtn);
    lv_obj_set_width(ui_YOLOLabel, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_YOLOLabel, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_YOLOLabel, LV_ALIGN_CENTER);
    lv_label_set_text(ui_YOLOLabel, "YOLO");
    lv_obj_set_style_text_color(ui_YOLOLabel, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_YOLOLabel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_YOLOLabel, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_YOLOBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "YOLOPage");
    _app_name_label_create(ui_YOLOBtn, "YOLO");

        // 视频 app（播放卡里的 mp4）—— 第 3 页第 1 行第 2 列
    lv_obj_t * ui_VideoBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_VideoBtn, 70);
    lv_obj_set_height(ui_VideoBtn, 70);
    lv_obj_set_x(ui_VideoBtn, 430);
    lv_obj_set_y(ui_VideoBtn, -45);
    lv_obj_set_align(ui_VideoBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_VideoBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_VideoBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_VideoBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_VideoBtn, lv_color_hex(0xFF9A00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_VideoBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
   
    lv_obj_t * ui_VideoIcon = lv_label_create(ui_VideoBtn);
    lv_obj_set_width(ui_VideoIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_VideoIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_VideoIcon, 0);
    lv_obj_set_y(ui_VideoIcon, 0);
    lv_obj_set_align(ui_VideoIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_VideoIcon, "");
    lv_obj_set_style_text_color(ui_VideoIcon, lv_color_hex(0xF6F7D7), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_VideoIcon, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_VideoIcon, &ui_font_iconfont48, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_VideoBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "VideoPage");
    _app_name_label_create(ui_VideoBtn, "视频");

    /* 音乐 App：第 3 页第 2 行（x=750, y=+55），和视频同一列 */
    lv_obj_t * ui_MusicBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_MusicBtn, 70);
    lv_obj_set_height(ui_MusicBtn, 70);
    lv_obj_set_x(ui_MusicBtn, 335);
    lv_obj_set_y(ui_MusicBtn, -45);
    lv_obj_set_align(ui_MusicBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_MusicBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_MusicBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_MusicBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_MusicBtn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_MusicBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_MusicBtn, &ui_img_music_data, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_MusicBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "MusicPage");
    _app_name_label_create(ui_MusicBtn, "音乐");

        //壁纸app
    lv_obj_t * ui_MyappBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_MyappBtn, 70);
    lv_obj_set_height(ui_MyappBtn, 70);
    lv_obj_set_x(ui_MyappBtn, 525);
    lv_obj_set_y(ui_MyappBtn, -45);
    lv_obj_set_align(ui_MyappBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_MyappBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_MyappBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_MyappBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_MyappBtn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_MyappBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_MyappBtn, &ui_img_837466338, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_MyappBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "MyappPage");
    _app_name_label_create(ui_MyappBtn, "壁纸");

    // Game Memory app
    lv_obj_t * ui_GameMemBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_GameMemBtn, 70);
    lv_obj_set_height(ui_GameMemBtn, 70);
    lv_obj_set_x(ui_GameMemBtn, 335);
    lv_obj_set_y(ui_GameMemBtn, 55);
    lv_obj_set_align(ui_GameMemBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_GameMemBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_GameMemBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_GameMemBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_GameMemBtn, lv_color_hex(0xDBE2EF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_GameMemBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_GameMemBtn, &ui_img_gamememory64_png, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_GameMemBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "GameMemoryPage");
    _app_name_label_create(ui_GameMemBtn, "记忆");

    // Draw app
    lv_obj_t * ui_DrawBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_DrawBtn, 70);
    lv_obj_set_height(ui_DrawBtn, 70);
    lv_obj_set_x(ui_DrawBtn, 430);
    lv_obj_set_y(ui_DrawBtn, 55);
    lv_obj_set_align(ui_DrawBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_DrawBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_DrawBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_DrawBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_DrawBtn, lv_color_hex(0x86A8E5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_DrawBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_DrawBtn, &ui_img_paint60_png, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_DrawBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "DrawPage");
    _app_name_label_create(ui_DrawBtn, "画画");

    // Calculator app
    lv_obj_t * ui_CalculatorBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_CalculatorBtn, 70);
    lv_obj_set_height(ui_CalculatorBtn, 70);
    lv_obj_set_x(ui_CalculatorBtn, 525);
    lv_obj_set_y(ui_CalculatorBtn, 55);
    lv_obj_set_align(ui_CalculatorBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_CalculatorBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_CalculatorBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_CalculatorBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_CalculatorBtn, lv_color_hex(0x707070), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_CalculatorBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_CalculatorIcon = lv_label_create(ui_CalculatorBtn);
    lv_obj_set_width(ui_CalculatorIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_CalculatorIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_CalculatorIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_CalculatorIcon, "");
    lv_obj_set_style_text_color(ui_CalculatorIcon, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_CalculatorIcon, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_CalculatorIcon, &ui_font_iconfont48, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_CalculatorBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "CalculatorPage");
    _app_name_label_create(ui_CalculatorBtn, "计算器");

    // memo app
    lv_obj_t * ui_MemoBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_MemoBtn, 70);
    lv_obj_set_height(ui_MemoBtn, 70);
    lv_obj_set_x(ui_MemoBtn, 655);
    lv_obj_set_y(ui_MemoBtn, -45);
    lv_obj_set_align(ui_MemoBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_MemoBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_MemoBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_MemoBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_MemoBtn, lv_color_hex(0xB83B5E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_MemoBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui_MemoBtn, &ui_img_memo64_png, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_MemoBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "MemoPage");
    _app_name_label_create(ui_MemoBtn, "备忘录");

        // game 2048 app
    lv_obj_t * ui_Game2048Btn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_Game2048Btn, 70);
    lv_obj_set_height(ui_Game2048Btn, 70);
    lv_obj_set_x(ui_Game2048Btn, 750);
    lv_obj_set_y(ui_Game2048Btn, -45);
    lv_obj_set_align(ui_Game2048Btn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_Game2048Btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_Game2048Btn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_Game2048Btn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Game2048Btn, lv_color_hex(0x11999E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Game2048Btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Icon2048 = lv_label_create(ui_Game2048Btn);
    lv_obj_set_width(ui_Icon2048, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Icon2048, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Icon2048, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Icon2048, "");
    lv_obj_set_style_text_color(ui_Icon2048, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Icon2048, 196, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Icon2048, &ui_font_iconfont48, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_Game2048Btn, ui_event_AppsBtn, LV_EVENT_CLICKED, "Game2048Page");
    _app_name_label_create(ui_Game2048Btn, "2048");

    // game muyu app
    lv_obj_t * ui_GameMuyuBtn = lv_button_create(ui_AppIconContainer);
    lv_obj_set_width(ui_GameMuyuBtn, 70);
    lv_obj_set_height(ui_GameMuyuBtn, 70);
    lv_obj_set_x(ui_GameMuyuBtn, 845);
    lv_obj_set_y(ui_GameMuyuBtn, -45);
    lv_obj_set_align(ui_GameMuyuBtn, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_GameMuyuBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_remove_flag(ui_GameMuyuBtn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(ui_GameMuyuBtn, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_GameMuyuBtn, lv_color_hex(0x355C7D), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_GameMuyuBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    // event
    lv_obj_add_event_cb(ui_GameMuyuBtn, ui_event_AppsBtn, LV_EVENT_CLICKED, "GameMuyuPage");
    _app_name_label_create(ui_GameMuyuBtn, "木鱼");

    lv_obj_t * ui_MuyuIcon = lv_label_create(ui_GameMuyuBtn);
    lv_obj_set_width(ui_MuyuIcon, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_MuyuIcon, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_MuyuIcon, LV_ALIGN_CENTER);
    lv_label_set_text(ui_MuyuIcon, "");
    lv_obj_set_style_text_color(ui_MuyuIcon, lv_color_hex(0xF6AB75), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_MuyuIcon, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_MuyuIcon, &ui_font_iconfont36, LV_PART_MAIN | LV_STATE_DEFAULT);


    // 定时器函数，每个5000ms 刷新一遍时间文本
    ui_home_timer = lv_timer_create(ui_home_timer_cb, 5000, ui_TimeLabel);
    // scroll_busy 超时看门狗（200ms 检查一次，500ms 未完成就强制复位）
    ui_scroll_wd_timer = lv_timer_create(ui_scroll_watchdog_cb, 200, NULL);
    //带动画从锁屏界面切入主界面
    lv_scr_load_anim(ui_HomeScreen, ui_home_enter_anim, 100, 0, true);
    //将动画从开始的向上移动 变为 向右移动
    ui_home_enter_anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT;   // 恢复默认，避免影响下次进入

}

///////////////////// SCREEN deinit ////////////////////

void ui_HomePage_deinit(void)
{
    lv_timer_delete(ui_home_timer);
    if(ui_scroll_wd_timer != NULL)
    {
        lv_timer_delete(ui_scroll_wd_timer);
        ui_scroll_wd_timer = NULL;
    }
    ui_desktop_data.scroll_busy = false;
    ui_desktop_data.scroll_busy_since = 0;
    ui_app_container = NULL;
    ui_WifiBtnRef = NULL;
    return;
}