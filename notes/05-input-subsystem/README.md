# 输入子系统层次分析

## 1. 学习目的

这次学习的目标是把输入子系统的层次关系理清楚，重点关注三个核心结构体：

1. `struct input_dev`
2. `struct input_handler`
3. `struct input_handle`

同时完成一个实操作业：使用 GPIO 按键实现长按关机。由于当前开发板没有真正的电源关断能力，本实验用 `ctrl_alt_del()` 触发重启来替代关机。

参考资料：驱动大全的“输入子系统”章节。

## 2. 输入子系统解决什么问题

如果每个按键、触摸屏、鼠标、键盘驱动都自己注册字符设备，用户态就要面对很多私有接口：

```text
/dev/key0
/dev/touch0
/dev/my_mouse
```

每个设备的 `read()` 数据格式也可能不同。输入子系统的作用就是把这些输入设备统一起来：

```text
硬件驱动 -> input core -> handler -> 用户态接口
```

典型用户态接口是：

```text
/dev/input/event0
/dev/input/event1
...
```

应用程序只要理解标准 `struct input_event`，就能读取按键、鼠标、触摸屏等输入事件。

## 3. 输入子系统三层结构

可以把输入子系统理解成三类对象：

```text
input_dev      表示一个输入设备
input_handler  表示一种事件处理方式
input_handle   表示 input_dev 和 input_handler 的绑定关系
```

它们之间不是简单的一对一关系，而是通过 input core 做匹配和连接。

```text
                  input core
                      |
          +-----------+-----------+
          |                       |
      input_dev              input_handler
   "我能产生事件"          "我能处理事件"
          |                       |
          +----------+------------+
                     |
                input_handle
              "二者连接后的实例"
```

## 4. `struct input_dev`

`input_dev` 代表一个真实或虚拟的输入设备。

对 GPIO 按键来说，它表达的是：

1. 我是一个输入设备
2. 我能产生按键类事件 `EV_KEY`
3. 我支持某些按键码，比如 `KEY_POWER`
4. 当硬件中断到来时，我会通过 `input_report_key()` 上报事件

最小使用流程：

```c
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

这里的 `input_sync()` 很重要，它表示一组输入事件上报完成。

## 5. `struct input_handler`

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

## 6. `struct input_handle`

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

## 7. GPIO 长按重启设计

作业目标是 GPIO 长按关机。本开发板没有电源关断功能，所以用重启代替。

整体思路：

1. GPIO 配置为输入
2. GPIO 中断配置为双边沿触发
3. 按下时启动 delayed work
4. 松开时取消 delayed work
5. delayed work 到期后再次确认按键仍然按下
6. 上报一次 `KEY_POWER`
7. 调用 `ctrl_alt_del()` 触发系统重启流程

为什么不在中断处理函数里直接调用 `ctrl_alt_del()`？

中断处理函数里应该尽量短，只做状态采集和调度。长按确认、重启触发这类动作放到 workqueue 上更清晰，也更符合内核驱动的分层习惯。

## 8. 核心 API

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

长按检测：

```c
INIT_DELAYED_WORK()
schedule_delayed_work()
cancel_delayed_work_sync()
```

触发重启：

```c
ctrl_alt_del()
```

## 9. 实验源码

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

## 10. 编译和运行

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

长按指定 GPIO 按键超过 `press_ms` 后，驱动会调用 `ctrl_alt_del()`，最终效果应当是系统进入重启流程。

卸载：

```sh
rmmod gpio_longpress_reboot
```

## 11. 这次作业的关键理解

这份驱动不是再做一个私有字符设备，而是把 GPIO 按键放进 Linux 输入子系统：

1. GPIO 按键驱动注册 `input_dev`
2. 内核已有的 `evdev` 作为 `input_handler`
3. input core 自动建立 `input_handle`
4. 驱动通过 `input_report_key()` 上报标准输入事件
5. 长按业务逻辑确认后调用 `ctrl_alt_del()` 完成重启替代关机

一句话总结：

输入子系统把“硬件怎么产生输入”和“用户态怎么消费输入”解耦，`input_handle` 就是这两边被 input core 匹配后建立起来的连接。
