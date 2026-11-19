#include "ui_MyappPage.h"

///////////////////// VARIABLES ////////////////////

static lv_obj_t * ui_MyappScreen;       // 整页
static lv_obj_t * ui_WallpaperView;     // 全屏壁纸视口：尺寸/位置随壁纸图片大小变化，铺满整屏
static lv_obj_t * ui_PageNumLabel;      // 页码 / 滑动提示，如 "< 2/5 >"
static lv_obj_t * ui_HomeSetBtn;        // 设为主屏壁纸
static lv_obj_t * ui_LockSetBtn;        // 设为锁屏壁纸

static int      ui_preview_index = 0;   // 当前预览的壁纸索引
static int32_t  ui_view_base_x = 0;     // 居中裁剪时视口应有的 x（动画的起止位置）
static int8_t   ui_slide_dir = 1;       // +1: 下一张(左滑)；-1: 上一张(右滑)
static bool     ui_sliding = false;     // 滑动动画进行中，期间忽略新手势
static bool     ui_gesture_fired = false;   // 本次按下是否已识别成滑动（用来忽略滑动末尾附带的那次 CLICKED）

// 滑动的单程时长(ms)：滑出 170ms + 滑入 170ms
#define _WALLPAPER_SLIDE_MS     170

// 按钮颜色：绿色 = 当前预览的这张已经是主屏/锁屏壁纸
#define _WALLPAPER_BTN_SET      lv_color_hex(0x1890FF)
#define _WALLPAPER_BTN_UNSET    lv_color_hex(0xD5DDE7)

typedef enum {
    _WALLPAPER_TARGET_HOME = 0,
    _WALLPAPER_TARGET_LOCK
} _wallpaper_target_t;

///////////////////// FUNCTIONS ////////////////////

static void _wallpaper_anim_x_cb(void * var, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)var, value);
}

// 把预览刷新到指定索引（不做动画）
static void _wallpaper_view_update(int index)
{
    ui_preview_index = ui_wallpaper_clamp(index);

    const lv_image_dsc_t * img = ui_wallpaper_get(ui_preview_index);
    int32_t img_w = (int32_t)img->header.w;
    int32_t img_h = (int32_t)img->header.h;

    // 背景图是从对象左上角开始画的：把对象左上角移到“图片中心对准屏幕中心”的位置，
    // 就得到“铺满全屏 + 居中裁剪”的效果（对象比图片还大一点，保证四个方向都不露黑边）
    lv_obj_set_size(ui_WallpaperView, LV_MAX(img_w, UI_SCREEN_WIDTH), LV_MAX(img_h, UI_SCREEN_HEIGHT));
    lv_obj_set_style_bg_image_src(ui_WallpaperView, img, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_view_base_x = (UI_SCREEN_WIDTH - img_w) / 2;
    lv_obj_set_pos(ui_WallpaperView, ui_view_base_x, (UI_SCREEN_HEIGHT - img_h) / 2);

    lv_label_set_text_fmt(ui_PageNumLabel, "< %d/%d >", ui_preview_index + 1, UI_WALLPAPER_NUM);

    bool is_home = (ui_preview_index == ui_wallpaper_clamp(ui_system_para.wallpaper_index));
    bool is_lock = (ui_preview_index == ui_wallpaper_clamp(ui_system_para.lock_wallpaper_index));
    lv_obj_set_style_bg_color(ui_HomeSetBtn, is_home ? _WALLPAPER_BTN_SET : _WALLPAPER_BTN_UNSET, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_LockSetBtn, is_lock ? _WALLPAPER_BTN_SET : _WALLPAPER_BTN_UNSET, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void _wallpaper_slide_in_ready(lv_anim_t * a)
{
    LV_UNUSED(a);
    ui_sliding = false;
}

// 滑出结束：换成新的壁纸，把它放到另一侧，然后再滑入
static void _wallpaper_slide_out_ready(lv_anim_t * a)
{
    LV_UNUSED(a);

    _wallpaper_view_update(ui_preview_index + ui_slide_dir);
    lv_obj_set_x(ui_WallpaperView, ui_view_base_x + ui_slide_dir * UI_SCREEN_WIDTH);   // 新壁纸从另一侧进入

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ui_WallpaperView);
    lv_anim_set_exec_cb(&anim, _wallpaper_anim_x_cb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_values(&anim, lv_obj_get_x(ui_WallpaperView), ui_view_base_x);
    lv_anim_set_time(&anim, _WALLPAPER_SLIDE_MS);
    lv_anim_set_ready_cb(&anim, _wallpaper_slide_in_ready);
    lv_anim_start(&anim);
}

// 切换壁纸：dir > 0 看下一张，dir < 0 看上一张
static void _wallpaper_slide_to(int8_t dir)
{
    if(ui_sliding || ui_WallpaperView == NULL) return;

    ui_sliding = true;
    ui_slide_dir = (dir >= 0) ? 1 : -1;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ui_WallpaperView);
    lv_anim_set_exec_cb(&anim, _wallpaper_anim_x_cb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_values(&anim, lv_obj_get_x(ui_WallpaperView), ui_view_base_x - ui_slide_dir * UI_SCREEN_WIDTH);
    lv_anim_set_time(&anim, _WALLPAPER_SLIDE_MS);
    lv_anim_set_ready_cb(&anim, _wallpaper_slide_out_ready);
    lv_anim_start(&anim);
}

// 左右滑动选择壁纸
static void ui_event_gesture(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_GESTURE) return;

    lv_indev_t * indev = lv_indev_active();
    if(indev == NULL) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if(dir == LV_DIR_LEFT)
    {
        ui_gesture_fired = true;
        _wallpaper_slide_to(1);     // 向左滑 -> 下一张
    }
    else if(dir == LV_DIR_RIGHT)
    {
        ui_gesture_fired = true;
        _wallpaper_slide_to(-1);    // 向右滑 -> 上一张
    }
}

// 每次按下按钮时清除滑动标记：LVGL 在抬手时不管有没有滑动都会发一次 CLICKED，
// 滑完正好松手就会误触发按钮，这里把它挡掉
static void ui_event_btn_pressed(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_PRESSED) ui_gesture_fired = false;
}

static void ui_event_back_btn(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(ui_gesture_fired) return;
    lv_lib_pm_OpenPrePage(&page_manager);
}

// 两个按钮共用：把当前预览的壁纸设为主屏 / 锁屏壁纸
static void ui_event_set_btn(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(ui_gesture_fired) return;    // 这次是滑动而不是点击

    _wallpaper_target_t target = (_wallpaper_target_t)(intptr_t)lv_event_get_user_data(e);

    if(target == _WALLPAPER_TARGET_LOCK)
    {
        ui_system_para.lock_wallpaper_index = (uint8_t)ui_preview_index;
    }
    else
    {
        ui_system_para.wallpaper_index = (uint8_t)ui_preview_index;
    }
    // 写回配置文件：主屏壁纸返回 HomePage 时生效，锁屏壁纸下次开机时锁屏生效
    sys_save_system_parameters(sys_config_path, &ui_system_para);

    _wallpaper_view_update(ui_preview_index);   // 刷新按钮高亮

    ui_msgbox_info("Wallpaper",
                   (target == _WALLPAPER_TARGET_LOCK) ? "Lock screen wallpaper set"
                                                      : "Home screen wallpaper set");
}

///////////////////// SCREEN init ////////////////////

void ui_MyappPage_init(void)
{
    // 从当前主屏壁纸开始预览
    ui_preview_index = ui_wallpaper_clamp(ui_system_para.wallpaper_index);
    ui_sliding = false;

    ui_MyappScreen = lv_obj_create(NULL);
    // 屏幕必须不可滚动，否则 LVGL 会把水平滑动当成滚动，手势收不到
    lv_obj_remove_flag(ui_MyappScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_MyappScreen, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_MyappScreen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 全屏壁纸视口：实际尺寸/位置由 _wallpaper_view_update() 按当前壁纸设置
    ui_WallpaperView = lv_obj_create(ui_MyappScreen);
    lv_obj_remove_flag(ui_WallpaperView, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ui_WallpaperView, LV_OBJ_FLAG_CLICKABLE);    // 不拦截点击，手势直接落到屏幕上
    lv_obj_set_style_bg_color(ui_WallpaperView, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_WallpaperView, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_WallpaperView, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_WallpaperView, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_WallpaperView, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_MyappScreen, ui_event_gesture, LV_EVENT_GESTURE, NULL);

    // 顶部栏：返回 + 页码
    lv_obj_t * ui_TopBar = lv_obj_create(ui_MyappScreen);
    lv_obj_set_width(ui_TopBar, UI_SCREEN_WIDTH);
    lv_obj_set_height(ui_TopBar, 34);
    lv_obj_set_align(ui_TopBar, LV_ALIGN_TOP_MID);
    lv_obj_remove_flag(ui_TopBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_TopBar, lv_color_hex(0xEFF2F6), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_TopBar, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_TopBar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_TopBar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_TopBar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_BackBtn = lv_button_create(ui_TopBar);
    lv_obj_set_size(ui_BackBtn, 40, 26);
    lv_obj_set_align(ui_BackBtn, LV_ALIGN_LEFT_MID);
    lv_obj_set_x(ui_BackBtn, 2);
    lv_obj_remove_flag(ui_BackBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_BackBtn, 0, 0);
    lv_obj_set_style_bg_opa(ui_BackBtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_BackBtn, 0, 0);
    lv_obj_set_style_shadow_width(ui_BackBtn, 0, 0);
    lv_obj_set_style_pad_all(ui_BackBtn, 0, 0);

    lv_obj_t * l = lv_label_create(ui_BackBtn);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(ui_BackBtn, ui_event_back_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_BackBtn, ui_event_btn_pressed, LV_EVENT_PRESSED, NULL);

    ui_PageNumLabel = lv_label_create(ui_TopBar);
    lv_obj_set_align(ui_PageNumLabel, LV_ALIGN_RIGHT_MID);
    lv_obj_set_x(ui_PageNumLabel, -8);
    lv_obj_set_style_text_color(ui_PageNumLabel, lv_color_hex(0x3A3F46), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_PageNumLabel, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 底部栏：两个设置按钮
    lv_obj_t * ui_BottomBar = lv_obj_create(ui_MyappScreen);
    lv_obj_set_width(ui_BottomBar, UI_SCREEN_WIDTH);
    lv_obj_set_height(ui_BottomBar, 46);
    lv_obj_set_align(ui_BottomBar, LV_ALIGN_BOTTOM_MID);
    lv_obj_remove_flag(ui_BottomBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_BottomBar, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_BottomBar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_BottomBar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_BottomBar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_BottomBar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_HomeSetBtn = lv_button_create(ui_BottomBar);
    lv_obj_set_size(ui_HomeSetBtn, 143, 30);
    lv_obj_set_align(ui_HomeSetBtn, LV_ALIGN_LEFT_MID);
    lv_obj_set_x(ui_HomeSetBtn, 8);
    lv_obj_remove_flag(ui_HomeSetBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_HomeSetBtn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_HomeSetBtn, 170, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * ui_HomeSetLabel = lv_label_create(ui_HomeSetBtn);
    lv_label_set_text(ui_HomeSetLabel, "主壁纸");
    lv_obj_set_style_text_font(ui_HomeSetLabel, &ui_font_app16, 0);
    lv_obj_center(ui_HomeSetLabel);
    lv_obj_add_event_cb(ui_HomeSetBtn, ui_event_set_btn, LV_EVENT_CLICKED, (void *)(intptr_t)_WALLPAPER_TARGET_HOME);
    lv_obj_add_event_cb(ui_HomeSetBtn, ui_event_btn_pressed, LV_EVENT_PRESSED, NULL);

    ui_LockSetBtn = lv_button_create(ui_BottomBar);
    lv_obj_set_size(ui_LockSetBtn, 143, 30);
    lv_obj_set_align(ui_LockSetBtn, LV_ALIGN_RIGHT_MID);
    lv_obj_set_x(ui_LockSetBtn, -8);
    lv_obj_remove_flag(ui_LockSetBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_LockSetBtn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_LockSetBtn, 170, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * ui_LockSetLabel = lv_label_create(ui_LockSetBtn);
    lv_label_set_text(ui_LockSetLabel, "锁屏壁纸");
    lv_obj_set_style_text_font(ui_LockSetLabel, &ui_font_app16, 0);
    lv_obj_center(ui_LockSetLabel);
    lv_obj_add_event_cb(ui_LockSetBtn, ui_event_set_btn, LV_EVENT_CLICKED, (void *)(intptr_t)_WALLPAPER_TARGET_LOCK);
    lv_obj_add_event_cb(ui_LockSetBtn, ui_event_btn_pressed, LV_EVENT_PRESSED, NULL);

    // 显示当前壁纸 / 按钮状态
    _wallpaper_view_update(ui_preview_index);

    // load page
    lv_scr_load_anim(ui_MyappScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

void ui_MyappPage_deinit(void)
{
    // 页面销毁前必须停掉滑动动画，否则动画回调会访问已经被删除的对象
    if(ui_WallpaperView != NULL)
    {
        lv_anim_delete(ui_WallpaperView, _wallpaper_anim_x_cb);
    }

    ui_MyappScreen = NULL;
    ui_WallpaperView = NULL;
    ui_PageNumLabel = NULL;
    ui_HomeSetBtn = NULL;
    ui_LockSetBtn = NULL;
    ui_sliding = false;
    ui_gesture_fired = false;
}
