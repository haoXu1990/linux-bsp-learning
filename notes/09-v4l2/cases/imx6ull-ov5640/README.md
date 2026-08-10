# i.MX6ULL + OV5640 DVP 实战入口

## 当前项目边界

- 开发板：100ASK i.MX6ULL
- 内核：Linux 4.9.88
- Sensor：OV5640
- 图像接口：8位DVP并口
- Sensor驱动：当前实验使用 `ov5640_v3.c`
- Capture驱动：`mx6s_capture.c`
- 显示优化：使用PXP完成YUV到RGB转换并输出到Framebuffer

## 推荐阅读顺序

1. [DVP适配与调试记录](01-DVP适配与调试记录.md)  
   解决供电、I²C、MCLK、pinctrl、设备树、驱动选择和分层验证，属于“如何把摄像头跑起来”。

2. [内核配置与编译部署](02-内核配置与编译部署.md)  
   解释修改config、DTB、zImage和模块后分别需要编译及部署什么。

3. [V4L2、Media与CSI源码分析](03-V4L2_Media与CSI源码分析.md)  
   从 `ov5640_v3.c`、`mx6s_capture.c`、设备树endpoint、V4L2 async和Buffer调用链理解“为什么能产生视频节点并收到一帧”。

4. [PXP V4L2源码与摄像头显示](04-PXP_V4L2源码与摄像头显示.md)  
   当前应优先阅读这一篇。它解释Camera数据怎样进入PXP、PXP怎样通过DMAEngine工作，以及RGB结果怎样交给Framebuffer。

5. [常见问题FAQ](FAQ.md)  
   按概念、设备树、驱动、Buffer、图像格式、PXP和实测结论归档已经确认的问题，避免重要结论只留在会话中。

## 这些文档为什么不直接合并

四份核心课程虽然都出现OV5640和V4L2，但解决的问题不同；FAQ只负责汇总结论并链接回主文档：

| 文档 | 核心问题 | 阅读时机 |
|---|---|---|
| DVP适配记录 | 硬件和驱动怎样跑通 | 摄像头没有节点、没有数据或花屏 |
| 编译部署 | 修改后到底生成、更新什么 | 驱动或配置没有生效 |
| V4L2/Media源码 | Sensor、CSI、V4L2怎样连接 | 建立源码架构思维 |
| PXP源码 | 怎样用硬件完成YUV转RGB和显示 | 解决软件转换造成的卡顿 |

因此当前先保留为四个边界清楚的章节，不再复制相同配置。后续发现重复段落时，以最匹配主题的文档为主，其他文档改成链接。

## 已确认的关键实验结论

OV5640 V3的DVP配置：

```c
{0x4740, 0x23, 0, 0}
```

改为：

```c
{0x4740, 0x21, 0, 0}
```

后花屏恢复正常。这属于Sensor DVP同步输出与i.MX6ULL CSI采样条件之间的接口契约问题。

## 当前下一步

先阅读并按照[PXP文档](04-PXP_V4L2源码与摄像头显示.md)的第5节和第20节检查：

1. PXP硬件节点是否保留 `fsl,imx6ull-pxp-dma` compatible。
2. PXP V4L2客户端是否使用独立节点。
3. 底层 `pxp_dma_v3.c` 和上层 `mxc_pxp_v4l2.c` 是否都probe成功。
4. Camera Capture Buffer是否由应用正确地转交给PXP VIDEO_OUTPUT队列。
