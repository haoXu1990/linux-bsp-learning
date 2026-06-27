# V4L2 学习与实验

本目录包含 V4L2 应用、摄像头显示实验和一套系统化架构笔记。

## 架构文档

建议按顺序阅读：

1. [V4L2 学习路线与全景图](architecture/00-V4L2学习路线与全景图.md)
2. [APP 接口与采集流程](architecture/01-APP接口与采集流程.md)
3. [V4L2 Core 设备模型与 ioctl 调用链](architecture/02-V4L2-Core设备模型与ioctl调用链.md)
4. [videobuf2 缓冲区管理](architecture/03-videobuf2缓冲区管理.md)
5. [Controls 参数控制框架](architecture/04-Controls参数控制框架.md)
6. [简单 V4L2 设备驱动编写](architecture/05-简单V4L2设备驱动编写.md)
7. [Media Controller 与 V4L2 Subdev](architecture/06-Media-Controller与V4L2-Subdev.md)
8. [MIPI 摄像头 Pipeline 与驱动编写](architecture/07-MIPI摄像头Pipeline与驱动编写.md)
9. [完整数据流与典型调用链](architecture/08-完整数据流与典型调用链.md)
10. [调试工具与常见问题](architecture/09-调试工具与常见问题.md)
11. [V4L2 理解练习题](architecture/10-V4L2理解练习题.md)

这套文档以 Linux 4.9 为主。V4L2 的总体模型在后续内核中仍然成立，但异步 subdev、固件节点、部分回调签名和 helper API 有演进；遇到这些位置，文中会给出简短提醒。

## 现有实验

- `v4l2-app/`：基础采集程序。
- `v4l2_camera_view/`：摄像头采集、像素转换和 framebuffer 显示。
- `video2lcd/`：较完整的视频采集与显示示例。
- `virtual_driver/`：虚拟摄像头驱动实验目录。

架构文档重点解释“为什么这样写、每层负责什么、调用怎样穿过各层”；实验代码用于观察这些概念如何落地。

## 主要参考

- 百问网 V4L2 课程资料：APP、V4L2 Core、videobuf2、虚拟摄像头、UVC、MIPI、Media Controller 与 subdev。
- 用户提供的知乎参考文章：<https://zhuanlan.zhihu.com/p/610873219>。
- Linux 内核 media/V4L2 API 和 Linux 4.9 源码。
- `v4l2-utils`：`v4l2-ctl`、`media-ctl`、`v4l2-compliance`。

> 知乎页面可能受登录或反爬限制；文档中的内核接口结论以 Linux 内核 API/源码和实际课程源码为准。
