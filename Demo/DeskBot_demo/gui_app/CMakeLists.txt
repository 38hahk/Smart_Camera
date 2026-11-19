
file(GLOB_RECURSE GUI_APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"  # 查找当前目录及子目录中的所有 .c 文件
)

# 定义 gui_app 模块为一个静态库
add_library(gui_app STATIC ${GUI_APP_SOURCES})

# 设置 gui_app 的头文件路径
target_include_directories(gui_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/common/stack
    ${CMAKE_CURRENT_SOURCE_DIR}/common/page_manager
)

# 查找包
find_package(json-c REQUIRED)
find_package(CURL REQUIRED)
# 将库链接到 gui_app
target_link_libraries(gui_app PRIVATE json-c)
target_include_directories(gui_app PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(gui_app PRIVATE CURL::libcurl)

# ---- 视频 App（ui_VideoPage）：需要线程 + ffmpeg ----
find_package(Threads REQUIRED)
target_link_libraries(gui_app PRIVATE Threads::Threads)

find_package(PkgConfig QUIET)

# ⚠️ 交叉编译的坑（踩过一次，ARM 整个编不过）：pkg-config 给的 INCLUDE_DIRS 里会有
#    `-I/usr/include` 或者 `-I/usr/include/x86_64-linux-gnu`，那是**主机**的路径，
#    而 `-I` 的搜索优先级高于 `--sysroot` 的默认路径 → 编译器会去读主机的 glibc 头文件
#    （/usr/include/x86_64-linux-gnu/sys/cdefs.h），报
#    `missing binary operator before token "("` 这种莫名其妙的话。
#    根治在 toolchain.cmake（给 pkg-config 设了 sysroot），这里再加一层保险：
#    交叉编译时把 /usr/include* 全部剔掉（portaudio.h / libav*.h 都在 sysroot 里）。
#    本机（x86）编译不受影响，因为那时 /usr/include 就是对的。
function(_ui_filter_host_includes out_var)
    set(_clean "")
    foreach(_d ${ARGN})
        if(CMAKE_CROSSCOMPILING AND _d MATCHES "^/usr/include")
            message(STATUS "gui_app: 剔掉主机 include 路径 ${_d}")
        else()
            list(APPEND _clean ${_d})
        endif()
    endforeach()
    set(${out_var} "${_clean}" PARENT_SCOPE)
endfunction()

if(PkgConfig_FOUND)
    # swresample：音频要重采样成设备能吃的格式（S16 交织，必要时换采样率）
    pkg_check_modules(FFMPEG QUIET libavformat libavcodec libavutil libswscale libswresample)
endif()
if(FFMPEG_FOUND)
    message(STATUS "gui_app: 找到 ffmpeg(${FFMPEG_VERSION})，VideoPage 启用真实解码")
    target_compile_definitions(gui_app PRIVATE HAVE_FFMPEG=1)
    _ui_filter_host_includes(_FF_INCS ${FFMPEG_INCLUDE_DIRS})
    target_include_directories(gui_app PRIVATE ${_FF_INCS})
    target_link_directories(gui_app PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(gui_app PRIVATE ${FFMPEG_LIBRARIES})
else()
    message(STATUS "gui_app: 没找到 ffmpeg -> VideoPage 退化成 stub（仿真机上先装 libav*-dev）")
endif()

# ---- 视频/音乐的声音输出：PortAudio ----
# 注：板端 sysroot 里本来就有 PortAudio
# libportaudio.so / libasound.so，所以这里只是把它也链给 gui_app。
if(PkgConfig_FOUND)
    pkg_check_modules(PORTAUDIO QUIET portaudio-2.0)
endif()
if(PORTAUDIO_FOUND)
    message(STATUS "gui_app: 找到 portaudio(${PORTAUDIO_VERSION})，VideoPage 有声音")
    target_compile_definitions(gui_app PRIVATE HAVE_PORTAUDIO=1)
    _ui_filter_host_includes(_PA_INCS ${PORTAUDIO_INCLUDE_DIRS})
    target_include_directories(gui_app PRIVATE ${_PA_INCS})
    target_link_libraries(gui_app PRIVATE ${PORTAUDIO_LIBRARIES})
else()
    message(STATUS "gui_app: 没找到 portaudio -> VideoPage 静音（仿真机上装 portaudio19-dev）")
endif()

# ---- 歌词：下载来的 .lrc 不少是 GBK 编码，用 iconv 转成 UTF-8 ----
# 注：x86（glibc）的 iconv 就在 libc 里；板端是 buildroot 的 libiconv（rootfs 里本来就有
#     /usr/lib/libiconv.so.2），FindIconv 会把 Iconv_LIBRARIES 设成 "iconv"。
find_package(Iconv QUIET)
if(Iconv_FOUND)
    message(STATUS "gui_app: 找到 iconv(${Iconv_LIBRARIES})，歌词支持 GBK/GB18030")
    target_compile_definitions(gui_app PRIVATE HAVE_ICONV=1)
    target_include_directories(gui_app PRIVATE ${Iconv_INCLUDE_DIRS})
    target_link_libraries(gui_app PRIVATE ${Iconv_LIBRARIES})
else()
    message(STATUS "gui_app: 没找到 iconv -> 歌词只认 UTF-8 编码")
endif()