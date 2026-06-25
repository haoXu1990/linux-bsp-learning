KERN_DIR = /home/xuhao/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master
PWD := $(shell pwd)
OUT_DIR := $(PWD)/out

all:
	mkdir -p $(OUT_DIR)
	$(CROSS_COMPILE)gcc -o usb_hid_mouse usb_hid_mouse.c \
	-I$(PWD)/include \
	-L$(PWD)/lib \
	-lusb-1.0
	cp usb_hid_mouse $(OUT_DIR)
clean:
	rm -rf $(OUT_DIR) usb_hid_mouse
#obj-m += usb_hid_mouse.o
