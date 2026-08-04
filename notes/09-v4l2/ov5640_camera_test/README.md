# OV5640 CSI capture diagnostic

这个程序只验证 `/dev/video1` 的 V4L2/CSI 原始采集，不操作 framebuffer，避免把采集问题和 LCD 显示问题混在一起。

## 功能

- 打印 V4L2 驱动、设备名和总线信息。
- 枚举支持的像素格式与帧尺寸。
- 请求指定格式，打印驱动最终返回的宽高、`bytesperline` 和 `sizeimage`。
- 使用 MMAP 队列采集若干帧。
- 打印最后一帧的 `bytesused`、前 64 字节以及四字节通道统计。
- 把最后一帧保存为原始文件，不做任何颜色转换。

## 编译

使用当前 Buildroot 交叉工具链，例如：

```sh
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
```

如果已经在环境变量中设置了 `CROSS_COMPILE`，直接执行：

```sh
make
```

## 开发板运行

默认测试 `/dev/video1`，请求 `640x480 YUYV`，丢弃前 9 帧并保存第 10 帧：

```sh
./ov5640_camera_test
```

完整参数：

```text
./ov5640_camera_test [video] [output.raw] [width] [height] [YUYV] [frames]
```

示例：

```sh
./ov5640_camera_test /dev/video1 ov5640_640x480_yuyv.raw 640 480 YUYV 10
```

当前旧版 OV5640 Sensor 驱动只声明 YUYV，因此采集时不要请求 UYVY。字节顺序的判断应对同一个原始文件使用不同的查看方式，不要切换驱动采集格式。

## 在 PC 上查看原始帧

按 YUYV 查看：

```sh
ffplay -f rawvideo -pixel_format yuyv422 -video_size 640x480 ov5640_640x480_yuyv.raw
```

按 UYVY 查看：

```sh
ffplay -f rawvideo -pixel_format uyvy422 -video_size 640x480 ov5640_640x480_yuyv.raw
```

如果同一个原始文件在其中一种字节顺序下正常，另一种不正常，就能确定实际的 YUV422 字节顺序。
