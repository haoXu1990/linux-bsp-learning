 1-wire 骨架分析
> drivers\w1\slaves\w1_therm.c 及其他相关文件画出层次图

## w1_therm.c
分析路径
1. 先浏览整个文件内容，理解关键骨架
  1.1  关键结构体
```shell
这个看名字就能差到是想吧功能抽象，然后不同的硬件实现自己的业务
 struct w1_therm_family_data {
	uint8_t rom[9];
	atomic_t refcnt;
};


struct therm_info {
	u8 rom[9];
	u8 crc;
	u8 verdict;
};

// init 方法中就是拿到这个数组批量注册，我要适配 DS18B20 ，应该就是增加一个结构体，然后实现里面的方法
struct w1_therm_family_converter {
	u8			broken;
	u16			reserved;
	struct w1_family	*f;
	int			(*convert)(u8 rom[9]);
	int			(*precision)(struct device *device, int val);
	int			(*eeprom)(struct device *device);
};



struct attribute {
	const char		*name;
	umode_t			mode;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	bool			ignore_lockdep:1;
	struct lock_class_key	*key;
	struct lock_class_key	skey;
#endif
};

只能看到 一个名字，具体mode 干什么的不明白，后面带入业务代码就能分析；



```
  
2. 根据 平台设备链路跟踪代码
```shell
 2.1 w1_therm_init
      2.2 注册
      w1_register_family(w1_therm_families[i].f) 
             	{
              		.f		= &w1_therm_family_DS18S20,
              		.convert	= w1_DS18S20_convert_temp,
              		.precision	= w1_DS18S20_precision,
              		.eeprom		= w1_therm_eeprom
             	  },
          2.2.1
          // 内核维护了一个 w1_families 的 LIST，应该是所有的 w1 都会加入进来
          // 检查当前注册的 fid 是否再  w1_families
          if (f->fid == newf->fid) {
     			  ret = -EEXIST;
     			  break;
      		}
          // 如果当前 w1_families 里面没有，就添加
          2.2.2 
          list_add_tail(&newf->family_entry, &w1_families);
          2.2.3
          // 添加后先匹配一次驱动程序
          w1_reconnect_slaves(newf, 1);
          // 如果能跟到后面的匹配过程，应该就知道怎么编写我自己的驱动了；
 2.1 匹配驱动程序
  w1.c-> w1_reconnect_slaves
  这里又维护了一个  w1_masters 的 LIST
  后面的看不懂了
  slaves
  master
  driver
 ```
 
3. 理解 1-wire
3.1 框架
物理层: DS18B20
总线层： GPIO, 单总线控制器
master： 主控制器

  w1-gpio.c, mxc_w1, matrox_w1.c

core：
w1_init.c

savle： 设备

driver：驱动

w1_therm.c

分析：
按照平台总线，initcal 的分析方式，先找入口函数
static int __init w1_init(void) (drivers/w1.c)
	w1_init_netlink();
	bus_register(&w1_bus_type);	  
	driver_register(&w1_master_driver);
	driver_register(&w1_slave_driver);
	
bus_register
类似 platform_bus_type ，处理设备怎么挂 ， driver 怎么匹配
w1_master_driver
谁在控制 1-wire 线，W1-GPIO.C （负责发复位信号，读bit，扫描设备）
w1_slave_driver
DS18B20 一类的设备，总线扫描的时候自动发现
driver
其实应该叫 w1_family 里面定义了 fid 也就是 设备 ROM ID，
fops {
.add_slave    = w1_ds2780_add_slave,   这个其实就是 probe？？？
	.remove_slave = w1_ds2780_remove_slave,
	.groups       = w1_ds2780_groups,
}
