/*
 * Simple synchronous userspace interface to SPI devices
 *
 * Copyright (C) 2006 SWAPP
 *	Andrea Paterniani <a.paterniani@swapp-eng.it>
 * Copyright (C) 2007 David Brownell (simplification, cleanup)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/acpi.h>

#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>

#include <linux/uaccess.h>
#include <linux/gpio/consumer.h>

#include <linux/fb.h>
#include <linux/dma-mapping.h>
#include <linux/kthread.h>


struct oled_dev {
    // SPI 设备
    struct spi_device *client;

    dev_t devt;

    struct cdev cdev;

    struct class *spidev_class;

    // framebuffer 操作句柄
    struct fb_info *oled_fb_info;

    struct task_struct *led_thread;

    struct device *oled_device;
};

static int oled_proc_thread(void *data) {
    return 0;
}

static int spi_oled_probe(struct spi_device *spi)
{
    // 1. 记录 SPI 设备
    struct oled_dev *oled_dev;
    int ret;

    printk("oled probe....\n");
    //1. 先给结构体分配空间

    oled_dev = kzalloc(sizeof(*oled_dev), GFP_KERNEL);
    if (!oled_dev) {
        return -EIO;
    }
    oled_dev->client = spi;

    // 2. 注册字符设备
    // 3. 创建并分配 fb_info 结构体
    // 4. 设置 fb_info 结构体
    // 5. 注册 fb_info
    // 6. 创建一个内核线程循环处理 fb数据
    // 7 初始化 oled
    return 0;
}

static int spi_oled_remove(struct spi_device *spi) {

    // 释放资源
    //
    return 0;
}


static struct spi_driver spid_oled_drv = {
    .driver = {
        .name = "100ask_oled_drv",
        .of_match_table = of_match_prt(),
    }

}

static int __init spi_oled_drv_init(void) {
    return i2c_add_driver(&at24c02_driver);
}

static void __exit spi_oled_drv_exit(void) {
    return i2c_del_driver(&at24c02_driver);
}

module_init(spi_oled_drv_init);

module_exit(spi_oled_drv_exit);
