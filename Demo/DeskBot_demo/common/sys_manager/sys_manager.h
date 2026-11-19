#ifndef _SYS_MANAGER_H
#define _SYS_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../../conf/dev_conf.h"
#include <stdint.h>
#include <stdbool.h>

extern const char * sys_config_path;
extern const char * city_adcode_path;

//城市行政编码结构体
typedef struct {
    char city[36];
    char adcode[16]; // 'gao de' Amap adcode
} LocationInfo_t;

//系统配置参数结构体
typedef struct {
    int year;                           // system time year             (need store)
    int month;                          // system time month            (need store)
    int day;                            // system time day              (need store)
    int hour;                           // system time hour             (need store)
    int minute;                         // system time minute           (need store)
    uint16_t brightness;                // system brightness            (need store)
    uint16_t sound;                     // system sound volume(0-30)    (need store)
    bool wifi_connected;                // wifi connected or not
    bool auto_time;                     // auto update time or not      (need store)
    bool auto_location;                 // auto update location or not  (need store)
    LocationInfo_t location;            // location info                (need store)
    char gaode_api_key[64];             // gaode api key for amap api   (need store)，高德 key 固定 32 字符，留余量
    uint8_t wallpaper_index;            // 主屏壁纸索引(见 ui_wallpaper_list) (need store)
    uint8_t lock_wallpaper_index;       // 锁屏壁纸索引(见 ui_wallpaper_list) (need store)
}system_para_t;

// 设置LCD背光亮度（0-100之间）
int sys_set_lcd_brightness(int brightness);

// 设置系统音量（0到100之间）
int sys_set_volume(int level);

// 设置系统时间
int sys_set_time(int year, int month, int day, int hour, int minute, int second);

// 获取系统时间
void sys_get_time(int *year, int *month, int *day, int *hour, int *minute, int *second);

// 获取指定日期是星期几
int sys_get_day_of_week(int year, int month, int day);

// 获取wifi连接状态
bool sys_get_wifi_status(void);

// 获取city name by adcode
const char* sys_get_city_name_by_adcode(const char *filepath, const char *target_adcode);

// 通过IP获取自动定位信息
int sys_get_auto_location_by_ip(LocationInfo_t* location, const char *api_key);

// 通过NTP服务器获取时间
int sys_get_time_from_ntp(const char* ntp_server, int *year, int *month, int *day, int *hour, int *minute, int *second);

// ---------------- WiFi 开关（主界面下拉菜单里的 WiFi 按钮用） ----------------
// 开启：起后台线程反复重试（网卡 up → wpa_supplicant → 等关联 → udhcpc 拿 IP）
// 关闭：杀掉 wpa_supplicant/udhcpc、网卡 down（也是在后台线程里做，不卡 UI）
int  sys_wifi_power_on(void);
void sys_wifi_power_off(void);
bool sys_wifi_power_is_on(void);        // 用户是否要求开启（按钮高亮用）
void sys_wifi_sync_state(void);         // 开机时同步一次：S99 已经拉起 WiFi 就显示为“开”

// 保存系统参数到文件
int sys_save_system_parameters(const char *filepath, const system_para_t *params);
// 从文件加载系统参数
int sys_load_system_parameters(const char *filepath, system_para_t *params);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif