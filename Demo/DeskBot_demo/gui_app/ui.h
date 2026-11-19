#ifndef _UI_H
#define _UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../lvgl/lvgl.h"
#include "./common/lv_lib.h"
#include "../common/sys_manager/sys_manager.h"
#include "../common/gpio_manager/gpio_manager.h"

#define UI_SCREEN_WIDTH 320         //屏幕宽度
#define UI_SCREEN_HEIGHT 240        //屏幕高度

typedef system_para_t ui_system_para_t;     //给system_para_t 起别名为 ui_system_para_t

// extern variables
extern lv_lib_pm_t page_manager;            //声明了一个指针 指向 页面管理器结构体
extern ui_system_para_t ui_system_para;     //声明了一个指针 指向 系统参数结构体

//声明 从锁屏进入主界面动画
extern lv_screen_load_anim_t ui_home_enter_anim;

//声明 需要用到的图片数组
LV_IMG_DECLARE(ui_img_weather64_png);    // assets/weather64.png
LV_IMG_DECLARE(ui_img_calendar64_png);    // assets/calendar64.png
LV_IMG_DECLARE(ui_img_memo64_png);    // assets/Memo64.png
LV_IMG_DECLARE(ui_img_gamememory64_png);    // assets/GameMemory64.png
LV_IMG_DECLARE(ui_img_paint60_png);    // assets/paint60.png
LV_IMG_DECLARE(ui_img_question60_png);    // assets/question60.png
LV_IMG_DECLARE(ui_img_think60_png);    // assets/think60.png
LV_IMG_DECLARE(ui_img_hand60_png);    // assets/hand60.png
LV_IMG_DECLARE(ui_img_muyu128_png);    // assets/muyu128.png
LV_IMG_DECLARE(ui_img_sun_png);    // assets/sun.png
LV_IMG_DECLARE(ui_img_clouds_png);    // assets/clouds.png
LV_IMG_DECLARE(ui_img_837466338);
LV_IMG_DECLARE(ui_img_music_data);    // 自绘：视频 App 图标（播放三角 + 圆角框）
LV_IMG_DECLARE(ui_img_carame_data);    // 自绘：音乐 App 图标（音符 + 圆角框）
LV_IMG_DECLARE(ui_img_album_data);     // 自绘：相册 App 图标（照片框 + 山 + 太阳）
LV_IMG_DECLARE(ui_img_mylogo);    // 自绘：桌面壁纸 1（我的 Logo）cd 
LV_IMG_DECLARE(ui_img_bfgril);
LV_IMG_DECLARE(ui_img_wtgril);
LV_IMG_DECLARE(ui_img_heidong);
LV_IMG_DECLARE(ui_img_yueguang);


// 声明 需要用到的字体
LV_FONT_DECLARE(ui_font_iconfont20);
LV_FONT_DECLARE(ui_font_iconfont26);
LV_FONT_DECLARE(ui_font_iconfont30);
LV_FONT_DECLARE(ui_font_iconfont36);
LV_FONT_DECLARE(ui_font_iconfont44);
LV_FONT_DECLARE(ui_font_iconfont48);
LV_FONT_DECLARE(ui_font_heiti14);
LV_FONT_DECLARE(ui_font_heiti22);
LV_FONT_DECLARE(ui_font_heiti24);
LV_FONT_DECLARE(ui_font_shuhei22);
LV_FONT_DECLARE(ui_font_NuberBig90);
LV_FONT_DECLARE(ui_font_iconfont24);
LV_FONT_DECLARE(ui_font_app16);

//定义 壁纸页面 壁纸的总数量
#define UI_WALLPAPER_NUM 4

// 可选壁纸图片列表（顺序即 ui_system_para.wallpaper_index / lock_wallpaper_index 的取值）
extern const lv_image_dsc_t * const ui_wallpaper_list[UI_WALLPAPER_NUM];

// 把任意整数索引归一化到 [0, UI_WALLPAPER_NUM)
int ui_wallpaper_clamp(int index);

// 按索引取壁纸图片描述符，索引越界时自动回绕
const lv_image_dsc_t * ui_wallpaper_get(int index);

// ui apps data
typedef lv_lib_pm_page_t ui_app_data_t; //给 lv_lib_pm_page_t 起别名为 ui_app_data_t

// UI INIT
void ui_init(void);

// UI INFO MSGBOX
void ui_msgbox_info(const char * title, const char * text);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif