
# 1. 使用不同的开发板内核时, 一定要修改 KERN_DIR
# 2. KERN_DIR 中的内核要事先配置、编译。为了能编译内核, 要先设置:
#    export ARCH=arm
#    export CROSS_COMPILE=arm-linux-gnueabihf-
#    export PATH=$PATH:/path/to/toolchain/bin

KERN_DIR = /home/xuhao/work/100ask/buildroot_100ask_t113-pro/buildroot/output/build/linux-origin_master
PWD := $(shell pwd)
OUT_DIR := $(PWD)/out
CFLAGS := -Wall -Iinclude

all:
	mkdir -p $(OUT_DIR)
	$(CROSS_COMPILE)gcc $(CFLAGS) -o at24c02_app at24c02_test.c i2cbusses.c smbus.c
	cp at24c02_app $(OUT_DIR)

clean:
	rm -rf $(OUT_DIR) at24c02_app
