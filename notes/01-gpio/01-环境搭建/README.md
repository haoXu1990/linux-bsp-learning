# 环境搭建与HelloWord

> 编译服务器: Ubuntu 20.04 LTS
> 参考文档: <嵌入式Linux应用开发完全手册V5.1_T113开发板.pdf>
> 参考资料：百问网仓库[ https://e.coding.net/weidongshan/01_all_series_quickstart.git]

## 1 源码下载
### 1.1 下载BSP 源码
```
cd ~/work/100ask
git clone  https://gitee.com/weidongshan//eLinuxCore_100ask-t113-pro.git
cd eLinuxCore_100ask-t113-pro
git submodule update --init --recursive

```

### 1.2 配置交叉编译工具链
```

cd ~/work/100ask/eLinuxCore_100ask-t113-pro
# 创建一个build.sh 脚本文件
vim build.sh
# 添加以下内容到 build.sh 文件中
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabi-
export PATH=$PATH:/home/xuhao/work/100ask/eLinuxCore_100ask-t113-pro/toolchain/gcc-linaro-7.2.1-2017.11-x86_64_arm-linux-gnueabi/bin

# 生效配置文件
source build.sh

```

## 2 第一个驱动实验
### 2.1 前提
在编译驱动程序只要需要优先编译内核、Moduels, dts 等，目的是保证开发板上程序与当前编译环境一致；
### 2.2 编译内核
对于不同的平台应该有不同的配置文件，配置文件位于内核源码目录下面: `arch/arm/configs/`。 kernel 的编译过程如下：
```
cd home/xuhao/work/100ask/eLinuxCore_100ask-t113-pro
make sun8iw20p1smp_t113_auto_defconfig # 这个就是配置文件
make zImage # 编译image 文件, 编译成功后会有提示并打印出了编译产物在 arch/arm/boot/zImage 目录下面
make dtbs   # 编译设备树(为什么要编译设备树？)， 编译成功也有响应的提示，并打印出了产物路径
# 拷贝编译产物到固定目录下面备用，我这里就在项目下面创建了一个 `eLinuxCore_100ask-t113-pro/out` 目录
cp arch/arm/boot/zImage out/
cp arch/arm/boot/dts/sun8iw20p1smp-t113-100ask-t113-pro.dtb out/
```
在上一步已经编译出了内核文件`zImage` 和设备树文件, 由于开发板不直接使用 zImage，需要打包为 boot.img文件， 命名如下：
```
cd arch/arm/boot
touch ramdisk.img
mkbootimg --kernel zImage --ramdisk ramdisk.img --board sun8iw20p1 --base 0x40200000 --kernel_offset 0x0 --ramdisk_offset 0x01000000 -o boot.img
# 同样的，拷贝这个 boot.img 到 out 目录备用
cp boot.img ~/work/100ask/eLinuxCore_100ask-t113-pro/linux/out/

# 这里有几个疑问
1. 为什么需要编译设备树？
2. 为什么需要吧 zImage 打包为 boot.img 文件？, ramdisk 得作用是什么, 根据 mkbootimg 后面的参数，感觉像是在设置启动引导的地址
3. mkbootimg 这个打包工具的参数得需要去学习下

```
### 2.3 编译模块
进入内核源码目录，编译内核模块：
```
cd ~/work/100ask/eLinuxCore_100ask-t113-pro/linux
make modules
# 编译完成后需要 install 到目标目录，在这里就是我们的 out 目录
make modules_install INSTALL_MOD_PATH=~/work/100ask/eLinuxCore_100ask-t113-pro/linux/out/

```

### 2.4 推送编译产物到目标板

## 3 创建 hello 驱动
程序界万年不变的hello word。
源码文件在`source/01_hello_drv/hello_drv.c`， 这里不详细介绍。

## 4. 总结
1. 驱动编译前需要统一板载环境和本地编译环境
2. 驱动编译与传统应用程序不一样， 驱动依赖的应该是`kbuild`构建系统，单纯只要gcc不行的，所以在编译时`$(MAKE) -C $(KERN_DIR) M=$(PWD) modules` , 这里的 `modules`就是触发`kbuild` 的模块构建流程，最后通过 `obj-m` 来指定编译目标(这里搞了半个小时才编译成功)
