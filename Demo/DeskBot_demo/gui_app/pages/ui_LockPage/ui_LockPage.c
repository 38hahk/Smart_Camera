#include "ui_LockPage.h"

///////////////////// 变量 ////////////////////

static lv_obj_t * ui_LockScreen;
static lv_obj_t * ui_LockTimeLabel;
static lv_obj_t * ui_LockDateLabel;
static lv_timer_t * ui_lock_timer;

///////////////////// FUNCTIONS ////////////////////

// 刷新锁屏上的时间/日期
static void _lock_time_update(void)
{
    int year; int month; int day; int hour; int minute; int second;
    //获取系统时间并保存在临时变量中
    sys_get_time(&year, &month, &day, &hour, &minute, &second);

    char time_str[8];
    //将时 分 写入字符串中
    sprintf(time_str, "%02d:%02d", hour, minute);
    //比较文本是否相同 将字符串设置为时间文本中
    if(strcmp(lv_label_get_text(ui_LockTimeLabel), time_str) != 0)
    {
        lv_label_set_text(ui_LockTimeLabel, time_str);
    }

    char date_str[16];
    sprintf(date_str, "%04d-%02d-%02d", year, month, day);
    if(strcmp(lv_label_get_text(ui_LockDateLabel), date_str) != 0)
    {
        lv_label_set_text(ui_LockDateLabel, date_str);
    }
}

//刷新时间
static void ui_lock_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    _lock_time_update();
}

// 上滑解锁：切到 HomePage，并让它从屏幕底部向上滑入
static void ui_event_LockScreen_gesture(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if(event_code != LV_EVENT_GESTURE)  return;

    lv_indev_t * indev = lv_indev_active();
    if(indev == NULL)   return;

    if(lv_indev_get_gesture_dir(indev) != LV_DIR_TOP)   return;

    ui_home_enter_anim = LV_SCR_LOAD_ANIM_MOVE_TOP;
    lv_lib_pm_OpenPage(&page_manager, NULL, "HomePage");
}

///////////////////// SCREEN init ////////////////////

void ui_LockPage_init(void)
{
    // lock screen
    ui_LockScreen = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_LockScreen, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_LockScreen, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_LockScreen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    // 锁屏壁纸（在 MyappPage 里选择，索引存在系统参数中，下次开机锁屏生效）
    lv_obj_set_style_bg_image_src(ui_LockScreen, ui_wallpaper_get(ui_system_para.lock_wallpaper_index), LV_PART_MAIN | LV_STATE_DEFAULT);
    // 手势事件。label 默认没有 CLICKABLE 标志，所以点在时间/日期上也照样能触发
    lv_obj_add_event_cb(ui_LockScreen, ui_event_LockScreen_gesture, LV_EVENT_GESTURE, NULL);

    // time
    ui_LockTimeLabel = lv_label_create(ui_LockScreen);
    lv_obj_set_align(ui_LockTimeLabel, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_LockTimeLabel, 30);
    lv_obj_set_style_text_color(ui_LockTimeLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_LockTimeLabel, &ui_font_NuberBig90, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_LockTimeLabel, "00:00");

    // date
    ui_LockDateLabel = lv_label_create(ui_LockScreen);
    lv_obj_set_align(ui_LockDateLabel, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_LockDateLabel, 135);
    lv_obj_set_style_text_color(ui_LockDateLabel, lv_color_hex(0xC0C0C0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_LockDateLabel, &ui_font_heiti22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_LockDateLabel, "2025-01-01");

    // swipe up hint
    lv_obj_t * ui_LockHintLabel = lv_label_create(ui_LockScreen);
    lv_obj_set_align(ui_LockHintLabel, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(ui_LockHintLabel, -25);
    lv_obj_set_style_text_color(ui_LockHintLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_LockHintLabel, &ui_font_heiti22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_LockHintLabel, "上滑开启");

    // time refresh
    _lock_time_update();
    ui_lock_timer = lv_timer_create(ui_lock_timer_cb, 1000, NULL);

    // load page (开机直接显示，不需要动画)
    lv_scr_load_anim(ui_LockScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

void ui_LockPage_deinit(void)
{
    // 必须先删掉定时器，否则解锁后它还会去访问已被销毁的 label
    if(ui_lock_timer != NULL)
    {
        lv_timer_delete(ui_lock_timer);
        ui_lock_timer = NULL;
    }
    ui_LockScreen = NULL;
    ui_LockTimeLabel = NULL;
    ui_LockDateLabel = NULL;
}
