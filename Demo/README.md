<h1 align="center">Demos for Echo</h2>

这个仓库存放着Echo开发板的各种例程, 包括LVGL、机器视觉等独立的demo。

[`AI桌面助手机器人Echo`](./DeskBot_demo/README.md)就是把他们融合起来做成的, LVGL作为UI界面, 这些高级demo作为子APP～

<h2 align="center">0. AI桌面机器人 Echo-Mate</h2>

### 📒 Overview

详见DeskBot_demo文件夹，这个文件夹包含了LVGL和其他demo的内容, 相当于是多个分离demo的融合，组成了Echo-Mate桌面AI助手, 详细如何编译和使用见[DsekBot_demo](./DeskBot_demo/README.md).

### 📑 AI桌面机器人整体框架：

实现的功能如下：

<p align="center">
	<img border="1px" width="60%" src="./assets/func_map.png">
</p>

软件框架如下：

<p align="center">
	<img border="1px" width="60%" src="./assets/software_diagram.png">
</p>

项目的文件目录如下:

```
DeskBot_demo/
├── bin/                   # 可执行文件
├── build/                 # build缓存
├── common/                # 通用层
│   ├── sys_manager/       # 开发板硬件对应的管理
│   └── xxx_manager/       # xxx对应的管理
├── conf/                  # 系统设置
├── gui_app/               # UI层的软件
│   ├── common/            # UI层扩展lib
│   ├── font/              # UI字体
│   ├── images/            # UI图片
│   ├── pages/             # UI层主要pages
│   └── ui.c/h             # 
├── lvgl/                  # lvgl核心组件
├── utils                  # 其他
├── lv_conf.h              # lvgl设置
└── main.c
```

<h2 align="center">1. yolov5</h2>

使用了yolov5s预训练模型，详见文件夹中的源代码进行学习，可以看yolov5 demo 的 [readme](./yolov5_demo/README.md)

<h2 align="center">2. rkmpi_demos</h2>

这个是luckfox写的关于rkmpi的相关demo, 有VI RTSP等使用的例程，可以参考luckfox的文档进行学习 [readme](./rkmpi_demos/README_CN.md)