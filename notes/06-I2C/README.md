

## 配置设备树
需要注意在有些 SoC 平台中 I2C 总线的名称会用 TWI 来表示，例如当前 T113-S3 平台 芯片手册和设备树中都是描述TWI0-TWI3；
根据当前开发板情况，本次实验选择了 `PE6`,`PE7` 2个引脚，设备树配置如下：
```shell
#  配置引脚
twi3_pins_a: twi3@0 {
		/* pins = "PE16", "PE17"; */
		/* pins = "PG10", "PG11"; */
		/*pins = "PB6", "PB7";*/
		pins= "PE6", "PE7";
		function = "twi3";
		drive-strength = <10>;
	};

	twi3_pins_b: twi3@1 {
		/* pins = "PE16", "PE17"; */
		/* pins = "PG10", "PG11"; */
		/*pins = "PB6", "PB7";*/
		pins = "PE6", "PE7";
		function = "gpio_in";
	};
# 启用 twi3
&twi3 {
	clock-frequency = <400000>;
	pinctrl-0 = <&twi3_pins_a>;
	pinctrl-1 = <&twi3_pins_b>;
	pinctrl-names = "default", "sleep";
	status = "okay";
};

# 关闭与PE6,PE7 冲突的 GMAC0
&gmac0 {
	pinctrl-0 = <&gmac0_pins_a>;
	pinctrl-1 = <&gmac0_pins_b>;
	pinctrl-names = "default", "sleep";
	phy-mode = "rmii";
	use_ephy25m = <0>;
	tx-delay = <3>;
	rx-delay = <0>;
	phy-rst = <&pio PE 10 GPIO_ACTIVE_HIGH>;
	status = "disabled";
};
```
更新设备树后用可以用下列命令检查
```shell
# i2cdetect -l
i2c-2   i2c             twi2                                    I2C adapter
i2c-3   i2c             twi3                                    I2C adapter （我们配置的TWI3）

#i2cget -f -y 3 0x50 0x00
0xff
证明读写正常
```

## 编写 drv 驱动
需要重新配置下设备树
```shell

&twi3 {
	clock-frequency = <400000>;
	pinctrl-0 = <&twi3_pins_a>;
	pinctrl-1 = <&twi3_pins_b>;
	pinctrl-names = "default", "sleep";
	status = "okay";
	
	at24c02@50 {
		compatible = "100ask,at24c02";
		reg = <0x50>;
	};
};

at24c02@50 部分模仿设备树其它操作写的，50 应该代表设备地址
```
驱动按照 i2c 总线设备模型编写，整体和前面差不多，这里不在赘述，
