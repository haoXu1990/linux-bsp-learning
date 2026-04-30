KERN_DIR = /home/xuhao/work/100ask/buildroot_100ask_t113-pro/buildroot/output/build/linux-origin_master
PWD := $(shell pwd)

obj-m += dht11_iio_triggered_drv.o

all:
	make -C $(KERN_DIR) M=$(PWD) modules

clean:
	make -C $(KERN_DIR) M=$(PWD) modules clean
