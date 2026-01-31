# 1. 使用不同的开发板内核时, 一定要修改 KERN_DIR
# 2. KERN_DIR 对应的内核源码/构建目录必须已配置并完成编译（至少生成了构建产物和头文件）
# 3. 若为交叉编译内核模块，通常需要在命令行额外指定/导出：
#    - ARCH          (例如：export ARCH=arm64)
#    - CROSS_COMPILE (例如：export CROSS_COMPILE=aarch64-linux-gnu-)
#
# 说明：
# - 内核模块源码（包含 <linux/module.h>）只能通过 Kbuild 编译；
#   不能用普通 gcc 直接编译成用户态可执行文件，否则就会出现 “linux/module.h: No such file”。

# 可在命令行覆盖：make KERN_DIR=/path/to/kernel
KERN_DIR = /home/xuhao/work/100ask/eLinuxCore_100ask-t113-pro/linux
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) modules clean
	rm -rf modules.order

obj-m += hello_drv.o
