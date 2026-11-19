#ifndef _UI_VIDEOPAGE_H
#define _UI_VIDEOPAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../../ui.h"

void ui_VideoPage_init(void);
void ui_VideoPage_deinit(void);

// 运行时可改：默认播放的文件。返回 0 表示成功
int  ui_VideoPage_set_file(const char *path);
const char *ui_VideoPage_get_file(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
