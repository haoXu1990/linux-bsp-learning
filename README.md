# Linux BSP 学习记录仓库

这个仓库保存Linux BSP各模块的学习笔记、实验源码、板卡适配记录和硬件参考资料。

## 从这里开始

- [文档入口与统一存放规范](docs/README.md)
- [V4L2、Camera与PXP学习入口](notes/09-v4l2/README.md)
- [i.MX6ULL + OV5640 DVP实战](notes/09-v4l2/cases/imx6ull-ov5640/README.md)
- [当前PXP源码课程](notes/09-v4l2/cases/imx6ull-ov5640/04-PXP_V4L2源码与摄像头显示.md)

## 目录职责

```text
linux-bsp-learning/
├─ docs/                  全仓库导航和文档规范
├─ notes/                 按学习模块保存原理、案例和实验程序
│  └─ <模块>/
│     ├─ README.md        当前模块入口
│     ├─ architecture/    通用原理
│     └─ cases/           具体板卡和器件实战
└─ hardware/              芯片、模块及厂商源码资料
```

具体文件应该放在哪里，以[文档入口与统一存放规范](docs/README.md)为准。
