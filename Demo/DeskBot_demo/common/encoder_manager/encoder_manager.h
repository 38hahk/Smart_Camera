#ifndef ENCODER_MANAGER_H
#define ENCODER_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../../conf/dev_conf.h"
#include <stdbool.h>
#include <time.h>

/*
 * 旋转编码器（带按压开关）驱动
 *
 * 默认接线（扩展排针 H1，PZ254V-12-12P）：
 *   A 相/CLK -> pin 8  = GPIO1_B0 = sysfs 40
 *   B 相/DT  -> pin 6  = GPIO1_B1 = sysfs 41
 *   按键/SW  -> pin 10 = GPIO1_A2 = sysfs 34   （低电平有效，需外部上拉或编码器板自带）
 *   VCC -> pin 3 (VCC_3V3)，GND -> pin 1
 *
 * 换接法就改这三个宏（GPIOx_yz = x*32 + y*8 + z，y: A=0,B=1,C=2,D=3）：
 */
#define ENCODER_GPIO_A   (1 * 32 + 1 * 8 + 0)   /* GPIO1_B0 */
#define ENCODER_GPIO_B   (1 * 32 + 1 * 8 + 1)   /* GPIO1_B1 */
#define ENCODER_GPIO_SW  (1 * 32 + 0 * 8 + 2)   /* GPIO1_A2 */

/* dir: +1 = 顺时针，-1 = 逆时针。回调在编码器线程里执行，不要碰 LVGL 对象 */
typedef void (*encoder_rotate_cb_t)(int dir);
typedef void (*encoder_press_cb_t)(void);

/* 初始化并启动监听线程；引脚不可用（仿真/被蓝牙占用）时返回 false */
bool encoder_init(encoder_rotate_cb_t on_rotate, encoder_press_cb_t on_press);

/* 停止监听线程并释放引脚 */
void encoder_deinit(void);

/* 是否已成功启动 */
bool encoder_available(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
