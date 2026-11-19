#include "sys_manager.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h> // for fcntl
#include <json-c/json.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h> // For setting system time
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if LV_USE_SIMULATOR == 0
#include <alsa/asoundlib.h>
#define BRIGHTNESS_PATH "/sys/class/backlight/backlight/brightness"
// 给定的亮度级别数组
const int brightness_levels[] = {
    0,   3,   5,   8,   10,  13,  16,  18,  21,  24,  26,  29,  32,  34,  37,
    40,  42,  45,  48,  50,  52,  54,  56,  58,  61,  64,  66,  69,  72,  74,
    77,  80,  82,  85,  88,  90,  93,  96,  98,  101, 104, 106, 109, 112, 114,
    117, 120, 122, 125, 128, 130, 133, 136, 138, 141, 144, 146, 149, 152, 154,
    157, 160, 162, 165, 168, 170, 173, 176, 178, 180, 182, 184, 186, 188, 190,
    192, 194, 197, 200, 202, 205, 208, 210, 213, 216, 218, 221, 224, 226, 229,
    232, 234, 237, 240, 242, 245, 248, 250, 253, 255};
const int num_brightness_levels = 100;
#endif

#define NTP_PORT 123                      // NTP 协议使用的端口号
#define NTP_TIMESTAMP_DELTA 2208988800ull // 时间戳差值，从1900年到1970年的秒数

const char *sys_config_path = "./system_para.conf"; // 系统参数配置文件路径
const char *city_adcode_path =
    "./gaode_adcode.json"; // 城市adcode对应表文件路径

// 设置背光亮度
int sys_set_lcd_brightness(int brightness) {
#if LV_USE_SIMULATOR == 0
  if (brightness < 0 || brightness > 100)
    return -1;
  if (brightness < 10)
    brightness = 10; // 亮度太低可能导致屏幕无法显示
  if (brightness > 95)
    brightness = 95;
  int fd = open(BRIGHTNESS_PATH, O_WRONLY); // 打开亮度文件
  if (fd == -1) {
    perror("Failed to open brightness file for writing");
    return -1;
  }

  char buffer[8]; // 应该足够存储任何可能的亮度值
  int n = snprintf(buffer, sizeof(buffer), "%d",
                   brightness); // 将亮度值转换为字符串

  //调用驱动提供的亮度操作函数接口，控制背光亮度
  if (write(fd, buffer, n) == -1) {
    perror("Failed to write brightness value");
    close(fd);
    return -1;
  }

  close(fd);
#endif
  return 0;
}

// 设置音量
int sys_set_volume(int level) {
  if (level < 0 || level > 100)
    return -1; // 音量级别应在0到100之间
               // 这里可以添加实际设置硬件音量的代码
#if LV_USE_SIMULATOR == 0
  const char *card = "hw:0";              // 声卡名称
  const char *selem_name = "DAC LINEOUT"; // 控件名称
  snd_mixer_t *handle;
  snd_mixer_selem_id_t *sid;
  snd_mixer_elem_t *elem;
  long min, max, mapped_volume;

  // 打开混音器
  if (snd_mixer_open(&handle, 0) < 0) {
    fprintf(stderr, "Error: Unable to open mixer.\n");
    return -1;
  }

  // 加载指定声卡
  if (snd_mixer_attach(handle, card) < 0) {
    fprintf(stderr, "Error: Unable to attach to card '%s'.\n", card);
    snd_mixer_close(handle);
    return -1;
  }

  // 注册混音器
  if (snd_mixer_selem_register(handle, NULL, NULL) < 0) {
    fprintf(stderr, "Error: Unable to register mixer.\n");
    snd_mixer_close(handle);
    return -1;
  }

  // 加载混音器元素
  if (snd_mixer_load(handle) < 0) {
    fprintf(stderr, "Error: Unable to load mixer.\n");
    snd_mixer_close(handle);
    return -1;
  }

  // 创建混音器元素 ID
  snd_mixer_selem_id_alloca(&sid);
  snd_mixer_selem_id_set_index(sid, 0); // 默认索引为 0
  snd_mixer_selem_id_set_name(sid, selem_name);

  // 查找对应元素
  elem = snd_mixer_find_selem(handle, sid);
  if (!elem) {
    fprintf(stderr, "Error: Unable to find element '%s'.\n", selem_name);
    snd_mixer_close(handle);
    return -1;
  }

  // 获取音量范围
  snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
  printf("Volume range: %ld to %ld\n", min, max);

  // 映射音量值到实际范围
  mapped_volume = min + (long)((double)(max - min) * level / 100.0);

  // 设置音量
  if (snd_mixer_selem_set_playback_volume_all(elem, mapped_volume) < 0) {
    fprintf(stderr, "Error: Unable to set volume.\n");
    snd_mixer_close(handle);
    return -1;
  }

  printf("Set '%s' volume to %ld (mapped from %d%%)\n", selem_name,
         mapped_volume, level);

  // 关闭混音器
  snd_mixer_close(handle);
#endif
  return 0;
}

// 判断是否是闰年
int is_leap_year(int year) {
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

// 验证日期是否有效
int validate_date(int year, int month, int day) {
  // 每个月的最大天数
  int days_in_month[] = {
      31, 28 + is_leap_year(year), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if (year < 1900 || month < 1 || month > 12 || day < 1 ||
      day > days_in_month[month - 1]) {
    return -1;
  }
  return 0;
}

int sys_set_time(int year, int month, int day, int hour, int minute,
                 int second) {

#if LV_USE_SIMULATOR == 0
  struct timeval tv;
  struct tm tm_set;

  // 验证日期有效性
  if (validate_date(year, month, day) != 0) {
    fprintf(stderr, "Invalid date.\n");
    return -1;
  }

  // 初始化tm结构体
  memset(&tm_set, 0, sizeof(struct tm));
  tm_set.tm_year = year - 1900; // 年份从1900年起计算
  tm_set.tm_mon = month - 1;    // 月份从0起计算
  tm_set.tm_mday = day;
  tm_set.tm_hour = hour;
  tm_set.tm_min = minute;
  tm_set.tm_sec = second;

  // 将tm转换为time_t类型
  time_t t = mktime(&tm_set);
  if (t == -1) {
    perror("mktime failed");
    return -1;
  }

  // 初始化timeval结构体
  tv.tv_sec = t;
  tv.tv_usec = 0; // 设置微秒部分为0

  // 设置系统时间
  if (settimeofday(&tv, NULL) == -1) {
    // 输出具体的错误信息
    perror("settimeofday failed");
    return -1;
  }
#endif
  printf("System time has been successfully updated.\n");
  return 0;
}

void sys_get_time(int *year, int *month, int *day, int *hour, int *minute,
                  int *second) {
  time_t rawtime;
  struct tm *timeinfo;

  time(&rawtime);
  timeinfo = localtime(&rawtime);

  if (year)
    *year = timeinfo->tm_year + 1900;
  if (month)
    *month = timeinfo->tm_mon + 1;
  if (day)
    *day = timeinfo->tm_mday;
  if (hour)
    *hour = timeinfo->tm_hour;
  if (minute)
    *minute = timeinfo->tm_min;
  if (second)
    *second = timeinfo->tm_sec;
}

// 使用蔡勒公式计算星期几，0代表周日，1代表周一，...，6代表周六
int sys_get_day_of_week(int year, int month, int day) {
  if (month < 3) {
    month += 12;
    year -= 1;
  }

  int K = year % 100;
  int J = year / 100;

  // 蔡勒公式
  int f = day + ((13 * (month + 1)) / 5) + K + (K / 4) + (J / 4) + (5 * J);
  int dayOfWeek = f % 7;

  // 根据蔡勒公式的定义调整返回值以匹配常见的一周起始日(0=周日, 1=周一, ...,
  // 6=周六)
  return (dayOfWeek + 1) % 7;
}

//检测设备是否可以访问网络
bool is_internet_reachable(void) {
  int sockfd;
  //创建 网络目标地址的结构体
  struct sockaddr_in servaddr;

  // 创建 遵循IPv4网络协议 的
  // TCP流式套接字（应用程序和网卡之间的管道，用来收发网络数据包），创建失败
  // 返回失败
  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("Socket creation failed");
    return false;
  }

  // 设置socket为非阻塞，阻塞模式 connect()会卡死 一直等待连接成功或失败
  // 卡死线程
  //通过fcntl文件描述符函数
  //读取sockfd中的信息，想要修改必须先读取，直接修改会抹掉原来所有属性
  int flags = fcntl(sockfd, F_GETFL, 0);
  //在通过fcntl 对sockfd 进行修改 追加非阻塞标志 追加失败 返回失败
  if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("Failed to set socket as non-blocking");
    close(sockfd);
    return false;
  }

  //设置协议遵循 IPv4网络协议
  servaddr.sin_family = AF_INET;
  //设置目标端口为 53 `htons()`：主机字节序转网络大端字节序（网络协议必须大端)
  servaddr.sin_port = htons(53); // Google DNS服务端口
  //把点分十进制字符串IP转成32bit网络字节序IP
  servaddr.sin_addr.s_addr = inet_addr("8.8.8.8"); // Google Public DNS IP地址

  //进行网络连接（不阻塞） 连接失败 返回失败
  if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
    if (errno != EINPROGRESS) {
      close(sockfd);
      return false;
    }

    // 使用select等待连接完成或超时
    //创建文件描述符集合 用于监听可写事件
    fd_set writefds;
    //清空集合
    FD_ZERO(&writefds);
    //将sockfd 加入监听集合
    FD_SET(sockfd, &writefds);

    //创建超时时间为 2s
    struct timeval timeout;
    timeout.tv_sec = 2; // 超时时间为2秒
    timeout.tv_usec = 0;

    //监听socket，返回连接情况
    int ret = select(sockfd + 1, NULL, &writefds, NULL, &timeout);
    if (ret == 0) { // 超时
      fprintf(stderr, "Connection timed out\n");
      close(sockfd);
      return false;
    } else if (ret < 0) { // 错误发生
      perror("Select failed");
      close(sockfd);
      return false;
    }

    // 检查是否成功连接重点：select
    // 返回就绪不代表一定连接成功！连接失败（如拒绝、路由不可达）socket
    // 同样会触发可写事件。 所以必须调用`getsockopt`读取 socket 内部错误码
    // `so_error == 0`：连接成功，无错误`so_error !=0`：连接失败
    int so_error;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 ||
        so_error != 0) {
      if (so_error != 0) {
        errno = so_error;
      }
      perror("Connect failed");
      close(sockfd);
      return false;
    }
  }

  //关闭文件描述符，并返回成功
  close(sockfd);
  return true;
}

bool sys_get_wifi_status(void) {

  // 检查网络是否可达
  if (!is_internet_reachable()) {
    return false;
  }

  return true;
}

///////////////////// WiFi 开关 ////////////////////
/*
 * 主界面下拉菜单里的 WiFi 按钮：
 *   开 → 起一个 detached 线程反复重试：
 *        ifconfig wlan0 up → wpa_supplicant（没跑就重启）→ 等 wpa_state=COMPLETED
 *        → 没 IP 就重启 udhcpc（-b 后台 + -A 5 每 5 秒重试）→ 5 秒后复查
 *   关 → 置标志 + 起一个收尾线程（杀 udhcpc/wpa_supplicant、网卡 down），不阻塞 UI
 *
 * 用“代次 gen”防抖：关了马上又开时，旧线程看到 gen 变了就自己退出，不会误杀新线程拉起的进程。
 * 仿真机上没有 wlan0，只维护状态（方便验证按钮高亮），不执行任何命令。
 */
static volatile int s_wifi_want = 0;      /* 1 = 用户要求开启 */
static volatile int s_wifi_gen  = 0;

bool sys_wifi_power_is_on(void) { return s_wifi_want != 0; }

#if LV_USE_SIMULATOR == 0
/* wlan0 当前有没有 IPv4 地址 */
static bool _wifi_has_ip(void)
{
  FILE *fp = popen("ifconfig wlan0 2>/dev/null", "r");
  if (!fp) return false;

  char line[160];
  bool has = false;
  while (fgets(line, sizeof(line), fp)) {
    if (strstr(line, "inet addr:") || strstr(line, "inet ")) { has = true; break; }
  }
  pclose(fp);
  return has;
}
#endif

static void *_wifi_power_worker(void *arg)
{
  int gen = (int)(intptr_t)arg;

  while (s_wifi_want && gen == s_wifi_gen) {
#if LV_USE_SIMULATOR == 0
    /* 1) 网卡 up；驱动还没加载好时先等一会儿再试 */
    if (system("ifconfig wlan0 up 2>/dev/null") != 0) {
      printf("[wifi] wlan0 还没就绪，稍后重试\n");
      for (int i = 0; i < 5 && s_wifi_want && gen == s_wifi_gen; i++) sleep(1);
      continue;
    }

    /* 2) wpa_supplicant 没在跑就（重新）拉起 */
    if (system("pidof wpa_supplicant >/dev/null 2>&1") != 0) {
      system("killall wpa_supplicant 2>/dev/null");
      system("rm -rf /var/run/wpa_supplicant; mkdir -p /var/run/wpa_supplicant");
      system("wpa_supplicant -B -D nl80211 -c /etc/wpa_supplicant.conf -i wlan0 2>/dev/null");
      printf("[wifi] 启动 wpa_supplicant\n");
    }

    /* 3) 等关联（最多 20 秒） */
    int linked = 0;
    for (int i = 0; i < 20 && s_wifi_want && gen == s_wifi_gen; i++) {
      if (system("wpa_cli -i wlan0 status 2>/dev/null | grep -q wpa_state=COMPLETED") == 0) {
        linked = 1;
        break;
      }
      sleep(1);
    }

    /* 4) 关联上了但没 IP → 重启 DHCP */
    if (linked && !_wifi_has_ip()) {
      system("killall udhcpc 2>/dev/null");
      system("udhcpc -i wlan0 -b -t 5 -T 3 -A 5 >/dev/null 2>&1 &");
      printf("[wifi] 重试 DHCP\n");
    } else if (!linked) {
      printf("[wifi] 还没关联上 AP，继续重试\n");
    }
#else
    (void)gen;
#endif

    /* 5) 歇 5 秒再复查（掉线会自动重新走上面的流程） */
    for (int i = 0; i < 5 && s_wifi_want && gen == s_wifi_gen; i++) sleep(1);
  }

  return NULL;
}

static void *_wifi_teardown_worker(void *arg)
{
  (void)arg;
#if LV_USE_SIMULATOR == 0
  system("killall udhcpc 2>/dev/null");
  system("killall wpa_supplicant 2>/dev/null");
  system("rm -rf /var/run/wpa_supplicant");
  system("ifconfig wlan0 down 2>/dev/null");
#endif
  printf("[wifi] 已关闭\n");
  return NULL;
}

int sys_wifi_power_on(void)
{
  if (s_wifi_want) return 0;

  s_wifi_want = 1;
  s_wifi_gen++;

  pthread_t tid;
  if (pthread_create(&tid, NULL, _wifi_power_worker,
                     (void *)(intptr_t)s_wifi_gen) != 0) {
    s_wifi_want = 0;
    return -1;
  }
  pthread_detach(tid);
  printf("[wifi] 开启，后台开始重试连接\n");
  return 0;
}

void sys_wifi_power_off(void)
{
  if (!s_wifi_want) return;

  s_wifi_want = 0;
  s_wifi_gen++;                 /* 让旧的连接线程自己退出 */

  pthread_t tid;
  if (pthread_create(&tid, NULL, _wifi_teardown_worker, NULL) == 0) {
    pthread_detach(tid);
  }
  printf("[wifi] 关闭\n");
}

void sys_wifi_sync_state(void)
{
  /* 只在整个进程里同步一次：S99 开机时已经把 WiFi 拉起来了，
   * 这里跟着显示为“开”，首次按键就是关掉它 */
  static int done = 0;
  if (done) return;
  done = 1;

#if LV_USE_SIMULATOR == 0
  if (system("pidof wpa_supplicant >/dev/null 2>&1") == 0) {
    s_wifi_want = 1;
    printf("[wifi] 检测到 wpa_supplicant 已在运行 → 按钮显示为开\n");
  }
#endif
}

// 递归查找 adcode 对应的城市名称
//@param 一个JSON数组 和 要查找的目标行政编码
const char *find_city_name(struct json_object *districts,
                           const char *target_adcode) {
  //如果编码或目标行政编码为空 返回
  if (!districts || !json_object_is_type(districts, json_type_array)) {
    return NULL;
  }

  //获取行政编码的长度
  size_t array_len = json_object_array_length(districts);
  for (size_t i = 0; i < array_len; i++) {
    //从JSON数组中，按索引取出数组里面第i个元素
    struct json_object *district = json_object_array_get_idx(districts, i);
    //如果district为空 就跳过
    if (!district)
      continue;

    struct json_object *adcode_obj, *name_obj, *sub_districts_obj;

    // 获取 adcode 和 name
    if (json_object_object_get_ex(district, "adcode", &adcode_obj) &&
        json_object_object_get_ex(district, "name", &name_obj) &&
        json_object_is_type(adcode_obj, json_type_string) &&
        json_object_is_type(name_obj, json_type_string)) {

      const char *adcode_str = json_object_get_string(adcode_obj);
      if (strcmp(adcode_str, target_adcode) == 0) {
        return json_object_get_string(name_obj);
      }
    }

    // 递归查找子地区
    if (json_object_object_get_ex(district, "districts", &sub_districts_obj)) {
      const char *result = find_city_name(sub_districts_obj, target_adcode);
      if (result) {
        return result;
      }
    }
  }

  return NULL;
}

// 解析 JSON 文件并查找 adcode
//
// ⚠️
// 返回值指向函数内部的静态缓冲区，调用方必须立刻拷贝走，不能长期保存。
// （原来是直接返回 find_city_name() 拿到的 json 内部字符串指针，
//   后面 json_object_put(root) 已经把整棵 json 树释放掉，
//   调用方 strcpy() 读到的是已释放内存 → 进程段错误，
//   这就是开机 UI 起不来的根因。）
const char *sys_get_city_name_by_adcode(const char *filepath,
                                        const char *target_adcode) {
  static char city_name_buf[64];

  // adcode 为空/未初始化时直接返回，避免拿空串去遍历整棵 json
  if (target_adcode == NULL || target_adcode[0] == '\0') {
    return NULL;
  }

  FILE *fp = fopen(filepath, "r");
  if (!fp) {
    fprintf(stderr, "Error opening file: %s\n", filepath);
    return NULL;
  }

  // 读取整个 JSON 文件内容
  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  rewind(fp);

  char *json_data = (char *)malloc(file_size + 1);
  if (!json_data) {
    fclose(fp);
    fprintf(stderr, "Memory allocation failed!\n");
    return NULL;
  }

  fread(json_data, 1, file_size, fp);
  json_data[file_size] = '\0';
  fclose(fp);

  // 解析 JSON
  struct json_object *root = json_tokener_parse(json_data);
  free(json_data); // 释放 JSON 读取的内存
  if (!root) {
    fprintf(stderr, "Error parsing JSON file!\n");
    return NULL;
  }

  struct json_object *districts;
  if (!json_object_object_get_ex(root, "districts", &districts)) {
    fprintf(stderr, "Invalid JSON format: missing 'districts' array.\n");
    json_object_put(root); // 释放 JSON 对象
    return NULL;
  }

  // 递归查找
  const char *result = find_city_name(districts, target_adcode);

  // 在释放 json 树之前先把结果拷贝出来（result 指向 json 对象内部，
  // json_object_put() 之后就是野指针）
  city_name_buf[0] = '\0';
  if (result != NULL) {
    strncpy(city_name_buf, result, sizeof(city_name_buf) - 1);
    city_name_buf[sizeof(city_name_buf) - 1] = '\0';
  }

  // 释放 JSON 对象
  json_object_put(root);

  return (result != NULL) ? city_name_buf : NULL;
}

// 回调函数用于处理CURL接收到的数据，将收到的数据写入用户提供的缓冲区中，并确保缓冲区不会溢出。
/*
参数	        含义
contents	  curl 返回的数据缓冲区，本次收到的字节块
size	      单个元素字节大小，libcurl 固定传 1
nmemb	      收到元素个数；实际字节数 realsize = size * nmemb
userp	      用户自定义指针，通过 CURLOPT_WRITEDATA 设置，这里传 char**
*/
size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;             //收到的总字节数
  char **response_string = (char **)userp;    //用户缓冲区指针

  char *new_string = realloc(*response_string, realsize + 1);   //重新分配内存，+1是为了存放字符串结束符'\0'
  if (new_string == NULL) {
    // 内存分配失败
    fprintf(stderr, "Failed to allocate memory\n");
    return 0;
  }

  *response_string = new_string;  //更新用户缓冲区指针
  memcpy(*response_string + strlen(*response_string), contents, realsize);  //将新数据追加到缓冲区末尾
  (*response_string)[realsize] = '\0';

  return realsize;
}

// 使用高德地图API根据IP地址获取自动定位信息
int sys_get_auto_location_by_ip(LocationInfo_t *location, const char *api_key) {
  CURL *curl_handle;
  CURLcode res;
  char url[256];
  //拼接GET请求写入url，作为高德IP定位接口，直接把key放在url参数里
  snprintf(url, sizeof(url), "https://restapi.amap.com/v3/ip?key=%s", api_key);

  //申请1字节内存，作为接受json的字符串缓冲区起点
  char *response_string = malloc(1); // 初始化为空字符串
  if (!response_string) {
    fprintf(stderr, "Failed to allocate memory\n");
    return -1;
  }
  response_string[0] = '\0';

  //全局一次性初始化
  curl_global_init(CURL_GLOBAL_ALL);
  //创建一个easy会话句柄 单词HTTP请求使用
  curl_handle = curl_easy_init();

  //如果url句柄为空 写入错误 并释放内存 清除全局变量
  if (!curl_handle) {
    fprintf(stderr, "Failed to initialize CURL\n");
    free(response_string);
    curl_global_cleanup();
    return -1;
  }

  //给easy句柄设置各种请求的参数
  //设置请求地址的参数为url
  curl_easy_setopt(curl_handle, CURLOPT_URL, url);
  //设置收到http body数据时调用的回调函数 为 WriteCallback
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteCallback);
  //设置传给回调函数的自定义参数 为response_string的地址
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &response_string);
  //指定根证书文件`cacert.pem`，HTTPS 做 SSL
  //证书校验；嵌入式设备必须把这个证书文件放到文件系统，否则 SSL 握手失败。
  curl_easy_setopt(curl_handle, CURLOPT_CAINFO, "cacert.pem");

  // 设置超时时间为5秒
  curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 5L);

  //执行HTTP、HTTPS请求 通过请求结果返回
  res = curl_easy_perform(curl_handle);

  //请求未成功
  if (res != CURLE_OK) {
    fprintf(stderr, "curl_easy_perform() failed: %s\n",
            curl_easy_strerror(res));
    free(response_string);
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();
    return -1;
  }

  //`json_tokener_parse`：json-c 库接口，把完整 JSON 字符串解析成 json 对象树。
  // 解析失败返回 NULL（返回内容不是合法 JSON，比如接口报错）
  struct json_object *parsed_json = json_tokener_parse(response_string);
  if (!parsed_json) {
    printf("Failed to parse JSON\n");
    free(response_string);
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();
    return -1;
  }

  // json_object_object_get_ex()：从json根对象中查找相关数据，并存放到对应变量中
  struct json_object *province_obj, *city_obj, *adcode_obj;
  json_object_object_get_ex(parsed_json, "province", &province_obj);
  json_object_object_get_ex(parsed_json, "city", &city_obj);
  json_object_object_get_ex(parsed_json, "adcode", &adcode_obj);

  // 检查province, city, adcode是否为空数组
  if (json_object_is_type(province_obj, json_type_array) &&
      json_object_array_length(province_obj) == 0 &&
      json_object_is_type(city_obj, json_type_array) &&
      json_object_array_length(city_obj) == 0 &&
      json_object_is_type(adcode_obj, json_type_array) &&
      json_object_array_length(adcode_obj) == 0) {
    printf("Location information is empty. This might be due to an invalid or "
           "foreign IP address.\n");
    json_object_put(parsed_json); // 释放JSON对象
    free(response_string);
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();
    return -2; // 自定义错误码表示位置信息为空
  }

  //将获取到的数据复制到系统参数中
  strncpy(location->city, json_object_get_string(city_obj),
          sizeof(location->city) - 1);
  location->city[sizeof(location->city) - 1] = '\0'; // 确保字符串以null结尾
  strncpy(location->adcode, json_object_get_string(adcode_obj),
          sizeof(location->adcode) - 1);
  location->adcode[sizeof(location->adcode) - 1] = '\0'; // 确保字符串以null结尾

  printf("City: %s\n", location->city);
  printf("Adcode: %s\n", location->adcode);

  json_object_put(parsed_json); // 释放JSON对象

  //释放内存 清除句柄 清除全局变量
  free(response_string);
  curl_easy_cleanup(curl_handle);
  curl_global_cleanup();

  return 0;
}

//通过网络时间协议获取系统时间
int sys_get_time_from_ntp(const char *ntp_server, int *year, int *month,
                          int *day, int *hour, int *minute, int *second) {
  //创建目标地址信息结构体对象
  struct addrinfo hints, *res;
  //创建变量 存储文件描述符
  int sockfd;
  //初始化hints
  memset(&hints, 0, sizeof(hints));

  hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
  hints.ai_socktype = SOCK_DGRAM; // UDP
  hints.ai_protocol = IPPROTO_UDP;

  //通过域名解析函数 将域名解析成IP地址 并存放在res中
  if (getaddrinfo(ntp_server, "123", &hints, &res) != 0) {
    perror("getaddrinfo failed");
    return -1;
  }

  //创建UDP套接字，用于网络连接
  sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sockfd == -1) {
    perror("socket creation failed");
    //必须`freeaddrinfo(res)`释放，否则内存泄漏。
    freeaddrinfo(res);
    return -1;
  }

  //设置socket为非阻塞，阻塞模式 connect()会卡死 一直等待连接成功或失败 卡死线程
  //通过fcntl文件描述符函数
  //读取sockfd中的信息，想要修改必须先读取，直接修改会抹掉原来所有属性
  int flags = fcntl(sockfd, F_GETFL, 0);
  if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("Failed to set socket as non-blocking");
    close(sockfd);
    //必须`freeaddrinfo(res)`释放，否则内存泄漏。
    freeaddrinfo(res);
    return -1;
  }

  // NTP 客户端请求包 固定48字节 其余全部填0
  unsigned char buf[48] = {0};
  buf[0] = 0x1b; // 00 011 011 LI = 0, Version = 3, Mode = 3

  // 发送请求包 给NTP客户端
  ssize_t sent_bytes =
      sendto(sockfd, buf, sizeof(buf), 0, res->ai_addr, res->ai_addrlen);
  if (sent_bytes == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("sendto failed");
      close(sockfd);
      freeaddrinfo(res);
      return -1;
    }
  }

  // 使用select等待接收数据或超时
  //创建文件描述符集合
  fd_set readfds;
  //初始化集合
  FD_ZERO(&readfds);
  //加入监听集合
  FD_SET(sockfd, &readfds);

  struct timeval timeout;
  timeout.tv_sec = 2; // 超时时间为2秒
  timeout.tv_usec = 0;

  //监听sockfd 返回连接结果
  int ret = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
  if (ret == 0) { // 超时
    fprintf(stderr, "Receive from NTP server timed out\n");
    close(sockfd);
    freeaddrinfo(res);
    return -1;
  } else if (ret < 0) { // 错误发生
    perror("Select failed");
    close(sockfd);
    freeaddrinfo(res);
    return -1;
  }

  //接收NTP客户端的回复信息
  ssize_t received_bytes = recvfrom(sockfd, buf, sizeof(buf), 0, NULL, NULL);
  if (received_bytes == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("recvfrom failed");
      close(sockfd);
      freeaddrinfo(res);
      return -1;
    }
  }

  // 关闭文件描述符
  close(sockfd);
  //初始化res
  freeaddrinfo(res);

  // 创建变量 存储时间戳
  uint32_t secsSince1900;
  //提取时间戳
  memcpy(&secsSince1900, &buf[40], sizeof(secsSince1900));
  //将时间戳从网络大端序传承mcu主机小端序
  secsSince1900 = ntohl(secsSince1900);

  //获取标准时间戳 用NTP时间戳 - NTP起始时间戳（1900 -01- 01）
  //得到unix时间戳（NTC 起始时间为1970 -01- 01）
  time_t unixTime = (time_t)(secsSince1900 - NTP_TIMESTAMP_DELTA);

  //设置时区
  setenv("TZ", "CST-8", 1); // UTC+8
  tzset();                  // 应用新的时区设置

  // 将UTC时间 转为 北京时间
  struct tm *timeinfo = localtime(&unixTime);
  if (!timeinfo) {
    perror("localtime failed");
    return -1;
  }

  if (year)
    *year = timeinfo->tm_year + 1900;
  if (month)
    *month = timeinfo->tm_mon + 1;
  if (day)
    *day = timeinfo->tm_mday;
  if (hour)
    *hour = timeinfo->tm_hour;
  if (minute)
    *minute = timeinfo->tm_min;
  if (second)
    *second = timeinfo->tm_sec;

  return 0;
}

int sys_save_system_parameters(const char *filepath,
                               const system_para_t *params) {
  FILE *file = fopen(filepath, "w");
  if (!file)
    return -1;

  fprintf(file, "year=%d\n", params->year);
  fprintf(file, "month=%d\n", params->month);
  fprintf(file, "day=%d\n", params->day);
  fprintf(file, "hour=%u\n", params->hour);
  fprintf(file, "minute=%u\n", params->minute);
  fprintf(file, "brightness=%u\n", params->brightness);
  fprintf(file, "sound=%u\n", params->sound);
  fprintf(file, "wifi_connected=%s\n",
          params->wifi_connected ? "true" : "false");
  fprintf(file, "auto_time=%s\n", params->auto_time ? "true" : "false");
  fprintf(file, "auto_location=%s\n", params->auto_location ? "true" : "false");
  fprintf(file, "city=%s\n", params->location.city);
  fprintf(file, "adcode=%s\n", params->location.adcode);
  fprintf(file, "gaode_api_key=%s\n", params->gaode_api_key);
  fprintf(file, "wallpaper_index=%u\n", params->wallpaper_index);
  fprintf(file, "lock_wallpaper_index=%u\n", params->lock_wallpaper_index);

  fclose(file);
  return 0;
}

int sys_load_system_parameters(const char *filepath, system_para_t *params) {
  FILE *file = fopen(filepath, "r");
  if (!file)
    return -1;

  char line[256];
  while (fgets(line, sizeof(line), file)) {
    char key[128], value[128];
    if (sscanf(line, "%[^=]=%s", key, value) != 2)
      continue;

    if (strcmp(key, "year") == 0) {
      params->year = atoi(value);
    } else if (strcmp(key, "month") == 0) {
      params->month = atoi(value);
    } else if (strcmp(key, "day") == 0) {
      params->day = atoi(value);
    } else if (strcmp(key, "hour") == 0) {
      params->hour = (uint8_t)atoi(value);
    } else if (strcmp(key, "minute") == 0) {
      params->minute = (uint8_t)atoi(value);
    } else if (strcmp(key, "brightness") == 0) {
      params->brightness = (uint16_t)atoi(value);
    } else if (strcmp(key, "sound") == 0) {
      params->sound = (uint16_t)atoi(value);
    } else if (strcmp(key, "wifi_connected") == 0) {
      params->wifi_connected = strcmp(value, "true") == 0;
    } else if (strcmp(key, "auto_time") == 0) {
      params->auto_time = strcmp(value, "true") == 0;
    } else if (strcmp(key, "auto_location") == 0) {
      params->auto_location = strcmp(value, "true") == 0;
    } else if (strcmp(key, "city") == 0) {
      strncpy(params->location.city, value, sizeof(params->location.city) - 1);
      params->location.city[sizeof(params->location.city) - 1] = '\0';
    } else if (strcmp(key, "adcode") == 0) {
      strncpy(params->location.adcode, value,
              sizeof(params->location.adcode) - 1);
      params->location.adcode[sizeof(params->location.adcode) - 1] = '\0';
    } else if (strcmp(key, "gaode_api_key") == 0) {
      strncpy(params->gaode_api_key, value, sizeof(params->gaode_api_key) - 1);
      params->gaode_api_key[sizeof(params->gaode_api_key) - 1] = '\0';
    } else if (strcmp(key, "wallpaper_index") == 0) {
      params->wallpaper_index = (uint8_t)atoi(value);
    } else if (strcmp(key, "lock_wallpaper_index") == 0) {
      params->lock_wallpaper_index = (uint8_t)atoi(value);
    }
  }

  fclose(file);

  /*
   * 文件能打开但内容不全（例如上次保存被截断、或者残留了一个空文件）时，
   * 上面一个 key 都匹配不到，params 会保持全 0：
   *   - year=0 → sys_set_time() 打印 "Invalid date." 设不了时钟
   *   - adcode 为空 → sys_get_city_name_by_adcode() 查不到城市
   * 这里做一次基本校验，不合格就返回 -1，让调用方走"生成默认配置"的分支，
   * 避免带着全 0 的参数继续初始化。
   */
  if (params->year < 1970 || params->year > 2100 || params->month < 1 ||
      params->month > 12 || params->day < 1 || params->day > 31) {
    fprintf(
        stderr,
        "system_para.conf invalid (year=%d month=%d day=%d), use defaults.\n",
        params->year, params->month, params->day);
    return -1;
  }

  return 0;
}
