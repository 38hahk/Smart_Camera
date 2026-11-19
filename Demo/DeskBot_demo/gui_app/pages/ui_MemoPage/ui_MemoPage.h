#ifndef _UI_MEMOPAGE_H
#define _UI_MEMOPAGE_H

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_MemoPage_init(void);
void ui_MemoPage_deinit(void);

/* 列表里每条备忘按钮的回调（user_data = 序号 + 1） */
void memo_event_row(lv_event_t *e);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
