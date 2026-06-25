
# 1. 使用不同的开发板内核时, 一定要修改 KERN_DIR
# 2. KERN_DIR 中的内核要事先配置、编译。为了能编译内核, 要先设置:
#    export ARCH=arm
#    export CROSS_COMPILE=arm-linux-gnueabihf-
#    export PATH=$PATH:/path/to/toolchain/bin

KERN_DIR = /home/xuhao/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master
PWD := $(shell pwd)
OUT_DIR := $(PWD)/out
CFLAGS := -Wall -Iinclude

all:
	mkdir -p $(OUT_DIR)
	$(CROSS_COMPILE)gcc $(CFLAGS) -o dac_test dac_test.c
	cp dac_test $(OUT_DIR)

clean:
	rm -rf $(OUT_DIR) dac_test
