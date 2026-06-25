# 输入子系统层次分析

## 1. 学习目的

这次学习的目标是把输入子系统的层次关系理清楚，重点关注三个核心结构体：

1. `struct input_dev`
2. `struct input_handler`
3. `struct input_handle`

同时完成一个实操作业：使用 GPIO 按键实现长按关机。由于当前开发板没有真正的电源关断能力，本实验用重启来替代关机。

驱动只上报 input 事件，APP 负责业务处理：

1. 驱动负责把 GPIO 按键注册成 input 设备
2. 驱动只上报 `KEY_POWER` 按下和松开
3. APP 监听 `/dev/input/eventX`
4. APP 判断是否长按
5. APP 调用 `reboot()` 执行重启

参考资料：驱动大全的“输入子系统”章节。

## 2. 输入子系统三层结构

可以把输入子系统理解成三类对象：

```text
input_dev      表示一个输入设备
input_handler  表示一种事件处理方式
input_handle   表示 input_dev 和 input_handler 的绑定关系
```

它们之间不是简单的一对一关系，而是通过 input core 做匹配和连接，这个和前面IIO 一样的，内核这种设计估计很多。

```text
                  input core
                      |          
          |                                    |
      input_dev                         input_handler
     "产生事件(维护一个handle list)"             "处理事件(同样维护一个 handle list)"
          |                                    |          
                     |
                input_handle
              "二者连接后的实例"
```

## 3. `struct input_dev`

`input_dev` 代表一个真实或虚拟的输入设备。

对 GPIO 按键来说，它表达的是：

1. 我是一个输入设备
2. 我能产生按键类事件 `EV_KEY`
3. 我支持某些按键码，比如 `KEY_POWER`
4. 当硬件中断到来时，我会通过 `input_report_key()` 上报事件

使用流程：

```shell
input = input_allocate_device();

input->name = "gpio-longpress-power";
input_set_capability(input, EV_KEY, KEY_POWER);

ret = input_register_device(input);
```

事件上报流程：

```c
input_report_key(input, KEY_POWER, 1);
input_sync(input);

input_report_key(input, KEY_POWER, 0);
input_sync(input);
```

这里的 `input_sync()` 很重要，它表示一组输入事件上报完成，其实也是一个特殊的事件上报。

## 4. `struct input_handler`

`input_handler` 代表一种处理输入事件的方式。

常见 handler 包括：

1. `evdev`：创建 `/dev/input/eventX`
2. `mousedev`：兼容老式鼠标接口
3. `kbd`：处理键盘类输入

普通设备驱动通常不用自己写 `input_handler`。我们写 GPIO 按键驱动时，只需要注册 `input_dev`，然后内核里已有的 `evdev` handler 会自动匹配它，并创建 `/dev/input/eventX`。

也就是说：

```text
GPIO 按键驱动负责 input_dev
evdev 负责 input_handler
input core 负责匹配二者
```

## 5. `struct input_handle`

`input_handle` 是 `input_dev` 和 `input_handler` 连接后的对象。

一个 `input_dev` 可以被多个 `input_handler` 处理，一个 `input_handler` 也可以处理多个 `input_dev`。所以内核需要一个中间对象记录“谁连接了谁”，这就是 `input_handle`。

关系可以这样理解：

```text
input_dev:     gpio-key
input_handler: evdev
input_handle:  evdev 和 gpio-key 建立连接后的实例
```

当驱动调用：

```c
input_report_key(input, KEY_POWER, 1);
input_sync(input);
```

input core 会把事件分发给已经连接上的 handler。对于 `evdev` 来说，最终用户态就可以从 `/dev/input/eventX` 读到标准 `input_event`。

## 6. GPIO 长按重启设计

作业目标是 GPIO 长按关机。本开发板没有电源关断功能，所以用重启代替。

整体思路：

1. GPIO 配置为输入
2. GPIO 中断配置为双边沿触发
3. 按下时启动 delayed work
4. 松开时取消 delayed work
5. APP 计算按下到松开的时间
6. 超过 3 秒后，APP 调用 `reboot()` 触发系统重启流程

为什么不在驱动里直接触发重启？

驱动层最好只负责硬件事件上报，不直接决定“长按以后做什么”。这样后面要改成关机、重启、弹提示框，或者忽略这个按键，都可以只改 APP，不用重新编译内核模块。

## 7. 核心 API

GPIO 和中断：

```c
gpio_request()
gpio_direction_input()
gpio_to_irq()
request_irq()
free_irq()
```

input 子系统：

```c
input_allocate_device()
input_set_capability()
input_register_device()
input_report_key()
input_sync()
input_unregister_device()
```

用户态监听：

```c
read(event_fd, &ev, sizeof(ev))
reboot(RB_AUTOBOOT)
```

## 8. 实验源码

源码目录：

- [source/gpio_longpress_reboot.c](</\\?\UNC\192.168.10.208\xuhao\work\100ask\linux-bsp-learning\notes\05-input-subsystem\source\gpio_longpress_reboot.c>)
- [source/Makefile](</\\?\UNC\192.168.10.208\xuhao\work\100ask\linux-bsp-learning\notes\05-input-subsystem\source\Makefile>)

默认参数：

```text
key_gpio = 103
active_low = 0
press_ms = 3000
```

如果实际板子的按键是低电平按下，可以加载模块时改为：

```sh
insmod gpio_longpress_reboot.ko active_low=1
```

如果想把长按时间改为 5 秒：

```sh
insmod gpio_longpress_reboot.ko press_ms=5000
```

## 9. 编译和运行

编译：

```sh
make
```

加载：

```sh
insmod gpio_longpress_reboot.ko
```

查看输入设备：

```sh
cat /proc/bus/input/devices
ls /dev/input/event*
```

可以用 `hexdump` 粗略观察事件：

```sh
hexdump /dev/input/event0
```

启动 APP：

```sh
./input_power_reboot_app /dev/input/eventX
```

长按指定 GPIO 按键超过 3 秒后，APP 会调用 `reboot()`，最终效果应当是系统进入重启流程。

卸载：

```sh
rmmod gpio_longpress_reboot
```

## 10. 这次作业的关键理解

这份驱动不是再做一个私有字符设备，而是把 GPIO 按键放进 Linux 输入子系统：

1. GPIO 按键驱动注册 `input_dev`
2. 内核已有的 `evdev` 作为 `input_handler`
3. input core 自动建立 `input_handle`
4. 驱动通过 `input_report_key()` 上报标准输入事件
5. APP 监听 input 事件并完成长按判断
6. APP 调用 `reboot()` 完成重启替代关机

一句话总结：

输入子系统把“硬件怎么产生输入”和“用户态怎么消费输入”解耦，`input_handle` 就是这两边被 input core 匹配后建立起来的连接。
