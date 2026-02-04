# 02 GPIO 控制方式
> 目标板  openvela-t113s3
> 目的 控制目标板 USER_LED1 与 USER_LED2

## 1. 实现流程
基于原始的裸GPIO驱动方式，控制 `USER_LED1` 和 `USER_LED2` 的状态。
流程 echo 1 > /dev/gpiodrv

## 2. 信息确认
根据硬件电路，确认芯片引脚，GPIO 组，GPIO 号，引脚方向，引脚模式，引脚值。
原理图: USER_LED1
芯片引脚：PD21/VSYNC
```shell
在确认GPIO number 时，需要用到 debufs， 这个fs 如果系统中没有，需要先检查
cat /proc/filesystems
nodev   sysfs
nodev   tmpfs
nodev   bdev
nodev   proc
nodev   devtmpfs
nodev   configfs
nodev   debugfs
nodev   sockfs
nodev   pipefs
nodev   ramfs
nodev   rpc_pipefs
nodev   devpts
        ext3
        ext2
        ext4
        vfat
        msdos
nodev   nfs
nodev   nfsd
        fuseblk
nodev   fuse
nodev   fusectl
nodev   ubifs
nodev   functionfs
// 然后挂载
# mount -t debugfs none /sys/kernel/debug/

// 打印所有引脚信息
cat /sys/kernel/debug/pinctrl/pio/pinconf-pins

PD21： 117,
PD22： 118

// 如果发现驱动中的 printk 没输出可以先设置
echo "7 4 1 7" > /proc/sys/kernel/printk
``

## 3. 实现初步控制
代码参考 source/gpio_led.c

## 4. 测试
由于我的主要目的是驱动，当前业务比较简单，app 比较熟悉，我就懒得写app 了，直接用下面的方式测试
```
shell
printf '\x00\x01' > /dev/gpio_led
[  462.124430] gpio_led_open
[  462.127455] gpio_led_write
[  462.130501] gpio_led_release
# printf '\x00\x00' > /dev/gpio_led
[  469.394441] gpio_led_open
[  469.397470] gpio_led_write
[  469.400515] gpio_led_release

观察到LED 灯在正常的亮灭，证明实验成功
其实看打印信息，感觉在代码里面的猜想是对的， 在 open 中初始化 GPIO 可能更合理；

```

## FQA
1.  安装驱动失败，PD22 引脚已经在使用
``` 
shell

insmod gpio_led.ko
[88614.725957] sun8iw20-pinctrl pio: pin PD22 already requested by 2000c17.pwm7; cannot claim for pio:118
[88614.736605] sun8iw20-pinctrl pio: pin-118 (pio:118) status -22
[88614.743868] gpio_request failed
[88614.843416] sun8iw20-pinctrl pio: pin PD22 already requested by 2000c17.pwm7; cannot claim for pio:118
[88614.853949] sun8iw20-pinctrl pio: pin-118 (pio:118) status -22
[88614.860539] gpio_request failed
insmod: can't insert 'gpio_led.ko': Invalid argument

```
解决方案 替换引脚为 PD21， PIN117，还是报错，信息如下：
```
shell

insmod gpio_led.ko
[88742.296039] gpio_request failed
[88742.396433] gpio_request failed
insmod: can't insert 'gpio_led.ko': Device or resource busy

```
排查方向，计软 gipo 请求失败，并且提示 busy 那么先检查gpio 使用情况：
```
shell
# 根据前面的只知识点，执行一下命名
cat /sys/kernel/debug/gpio
gpiochip0: GPIOs 0-223, parent: platform/pio, pio:
 gpio-35  (                    |usb1-vbus           ) out lo
 gpio-117 (                    |gpio_led_1          ) out lo
 gpio-138 (                    |phy-rst             ) in  lo
 gpio-166 (                    |cd                  ) in  lo IRQ ACTIVE LOW
 gpio-202 (                    |wlan_hostwake       ) in  hi
 gpio-204 (                    |wlan_regon          ) out lo
 gpio-205 (                    |bt_wake             ) out lo
 gpio-206 (                    |bt_hostwake         ) in  hi
 gpio-207 (                    |bt_rst              ) out lo

```
由以上信息可以看出 gpio-117 已经被占用，所以需要先释放 gpio-117，然后再尝试加载 gpio_led.ko 模块。
又出现新的问题， /dev 下面没有创建的驱动节点，lsmod | grep gpio_led 也查找不到，
感觉可能是第一次运行insmod gpio_led.ko 时控制的2个 GPIO， 117控制成功了，而118控制失败了，导致驱动加载失败？。
先重启，重新安装驱动试试
重启后，安装驱动还是报错，信息如下：
```
shell

insmod gpio_led.ko
[   73.953083] gpio_led: loading out-of-tree module taints kernel.
[   73.960733] gpio_direction_output failed
[   74.054944] gpio_request failed
insmod: can't insert 'gpio_led.ko': Device or resource busy

```

观察到现象，安装驱动虽然失败，但是LED2亮了，直觉判断代码有问题，根据前面的失败打印信息 `gpio_request`这个出现2次就感觉有问题，所以再次检查代码发下如下问题：

```
shell
static struct gpio_desc gpios[2] = {
    {117, 0 , "gpio_led_1"}
};
```
破案了，由于 gpios 里面老师的教程有2条数据，而我的板子 LED1 已经有模块在使用了，我就干掉了一个LED 的数据，那么初始化代码就会重复初始化 117 这个引脚，这也解释了出错信息为什么会有 busys
