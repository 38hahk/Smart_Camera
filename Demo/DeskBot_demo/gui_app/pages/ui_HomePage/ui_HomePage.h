#ifndef _UI_HOMEPAGE_H
#define _UI_HOMEPAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../../ui.h"

typedef struct {
    uint16_t witdh;                 // 屏幕宽度
    uint16_t height;                // 屏幕高度
    uint8_t container_total_pages;  // app页面数量
    uint8_t app_container_index;    // app页面容器的索引
    bool show_dropdown;             // 是否展示下滑页面
    bool scroll_busy;               // 是否允许滑动和点击事件同时进行
    uint32_t scroll_busy_since;     // scroll_busy 置位时刻（看门狗超时复位用）
}ui_desktop_data_t;

void ui_HomePage_init(void);
void ui_HomePage_deinit(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif