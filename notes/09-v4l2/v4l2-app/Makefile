KERN_DIR = /home/xuhao/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master
PWD := $(shell pwd)
OUT_DIR := $(PWD)/out

all:
	mkdir -p $(OUT_DIR)
	$(CROSS_COMPILE)gcc -o v4l2_app v4l2_app.c
	cp v4l2_app $(OUT_DIR)
clean:
	rm -rf $(OUT_DIR) v4l2_app
