#include "app_WeatherPage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

// libcurl回调函数，curl 收到 HTTP 响应分片数据时，反复调用这个函数，把分片拼接到一块动态内存里，最后拿到完整响应字符串
//@param contents 收到的数据块起始地址，服务器刚发过来的一小段数据。不是字符串，是原始二进制
//@param size 代表单个数据单元字节数
//@param size_t nmemb 本次回调收到多少个单元
//@param userp 用户自定义指针，由CURLOPT_WRITEDATA设置，用来把外部变量传入回调
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    // 1. 计算本次收到多少字节
    size_t realsize = size * nmemb;
    // 2. userp是 char**，强制转换。response 是外部的 char* 的地址
    char** response = (char**)userp;

    // 3. 获取当前已保存字符串的长度，然后扩容(重新分配内存)：原有长度 + 新数据 + 1(\0)
    *response = realloc(*response, strlen(*response) + realsize + 1);
    if (*response == NULL) {
        fprintf(stderr, "Failed to allocate memory\n");
        return 0;
    }

    // 5. 把本次收到的数据追加到缓冲区末尾
    strncat(*response, (char*)contents, realsize);
    // 6. 返回本次处理字节数，告诉libcurl正常接收 
    return realsize;
}

//通过行政编码获取天气信息
int get_weather_info_by_adcode(const char* adcode, WeatherInfo_t* weather_info) {
    //curl_handle 是一个句柄，代表一次独立的 HTTP 请求会话
    CURL* curl_handle;
    //接收 `curl_easy_perform()`、`curl_easy_setopt()` 等函数的返回结果，判断本次 HTTP 请求成功还是失败，以及失败原因
    CURLcode res;
    char url[256];
    snprintf(url, sizeof(url), "https://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s", adcode, ui_system_para.gaode_api_key);

    char* response_string = malloc(1); // 初始化为空字符串
    if (!response_string) {
        fprintf(stderr, "Failed to allocate memory\n");
        return -1;
    }
    response_string[0] = '\0';

    //初始化整个 libcurl 库的全局资源
    curl_global_init(CURL_GLOBAL_ALL);
    //创建一个easy 会话句柄（CURL *)，用来做一次（或多次复用）HTTP 请求
    curl_handle = curl_easy_init();

    if (!curl_handle) {
        fprintf(stderr, "Failed to initialize CURL\n");
        free(response_string);
        curl_global_cleanup();
        return -1;
    }

    //设置HTTP请求的参数url
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    //设置HTTP请求的参数回调函数 
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteCallback);
    //设置HTTP请求的参数 传给回调函数的自定义参数
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &response_string);
    //设置HTTP请求的参数 指定根证书文件
    curl_easy_setopt(curl_handle, CURLOPT_CAINFO, "cacert.pem");

    // 设置超时时间为5秒（5000毫秒）
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, 5000L);
    //根据你之前 `curl_easy_setopt` 设置好的参数，发起网络连接、收发数据；收到数据分片就反复调用你的 WriteCallback。
    // 函数直到请求完成/失败才返回。
    res = curl_easy_perform(curl_handle);

    if(res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
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
    //lives_array 用来保存 JSON 里lives这个数组节点的指针
    //live_obj 后面循环用，保存数组里单个 live 对象
    struct json_object *lives_array, *live_obj;
    //根据目标名称从json 对象树中获取对应数据并保存到新地址
    json_object_object_get_ex(parsed_json, "lives", &lives_array);
    //如果类型正确且内容不为空
    if(json_object_get_type(lives_array) == json_type_array && json_object_array_length(lives_array) > 0) {

        //根据下标索引，取出 JSON 数组里元素的 json_object 指针
        live_obj = json_object_array_get_idx(lives_array, 0);
        //将获取的天气信息临时存储
        struct json_object *weather, *temperature, *humidity, *windpower;
        json_object_object_get_ex(live_obj, "weather", &weather);
        json_object_object_get_ex(live_obj, "temperature", &temperature);
        json_object_object_get_ex(live_obj, "humidity", &humidity);
        json_object_object_get_ex(live_obj, "windpower", &windpower);

        //从类型为json_type_string的 json 节点取出 C 字符串
        const char* weather_str = json_object_get_string(weather);
        const char* temperature_str = json_object_get_string(temperature);
        const char* humidity_str = json_object_get_string(humidity);
        const char* windpower_str = json_object_get_string(windpower);

        //将数据复制到天气信息结构体属性中
        strncpy(weather_info->weather, weather_str ? weather_str : "N/A", sizeof(weather_info->weather) - 1);
        weather_info->weather[sizeof(weather_info->weather) - 1] = '\0';
        strncpy(weather_info->temperature, temperature_str ? temperature_str : "N/A", sizeof(weather_info->temperature) - 1);
        weather_info->temperature[sizeof(weather_info->temperature) - 1] = '\0';
        strncpy(weather_info->humidity, humidity_str ? humidity_str : "N/A", sizeof(weather_info->humidity) - 1);
        weather_info->humidity[sizeof(weather_info->humidity) - 1] = '\0';
        strncpy(weather_info->windpower, windpower_str ? windpower_str : "N/A", sizeof(weather_info->windpower) - 1);
        weather_info->windpower[sizeof(weather_info->windpower) - 1] = '\0';

        json_object_put(parsed_json); // 释放JSON对象

        free(response_string);
        curl_easy_cleanup(curl_handle);
        curl_global_cleanup();

        return 0;
    } else {
        json_object_put(parsed_json); // 释放JSON对象
        free(response_string);
        curl_easy_cleanup(curl_handle);
        curl_global_cleanup();
        return -1;
    }
}