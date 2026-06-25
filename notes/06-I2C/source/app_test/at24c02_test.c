#include <stddef.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <i2c/smbus.h>
#include "i2cbusses.h"
#include <time.h>

#define AT24C02_ADDR 0x50

int main(int argc, char **argv){
    unsigned char buf[32];
    char filename[20];
    int file;
    int ret;
    const unsigned char *str;
    struct timespec req;
    unsigned char mem_addr = 0;

    if ((argc != 4 || argv[2][0] != 'w') && (argc != 3 || argv[2][0] != 'r')) {
        printf("Usage:\n");
		printf("write eeprom: %s <i2c_bus_number> w string\n", argv[0]);
		printf("read  eeprom: %s <i2c_bus_number> r\n", argv[0]);
		return -1;
    }

    if (argv[2][0] == 'w' && strlen(argv[3]) >= 256) {
        printf("string is too long, at24c02 only has 256 bytes\n");
        return -1;
    }

    // 打开设备
    file = open_i2c_dev(argv[1][0] - '0', filename, sizeof(filename), 0);
    if (file < 0)
	{
		printf("can't open %s\n", filename);
		return -1;
	}
    // 设置地址
    if (set_slave_addr(file, AT24C02_ADDR, 1)) {
        printf("set 24c0e slave addr error. \n");
        return -1;
    }

    if (argv[2][0] == 'w') {
        // 用户输入的字符串
        str = (const unsigned char *)argv[3];

        req.tv_sec = 0;
        req.tv_nsec = 20000000; /* 20ms */

        while (*str) {
            ret = i2c_smbus_write_byte_data(file, mem_addr, *str);

            if (ret) {
                printf(" 24c02 write byte data error\n");
                return -1;
            }

            nanosleep(&req, NULL);

            mem_addr++;
			str++;
        }

        ret = i2c_smbus_write_byte_data(file, mem_addr, 0); // string end char
		if (ret)
		{
			printf("i2c_smbus_write_byte_data err\n");
			return -1;
		}
    } else {
		ret = i2c_smbus_read_i2c_block_data(file, mem_addr, sizeof(buf), buf);
		if (ret < 0)
		{
			printf("i2c_smbus_read_i2c_block_data err\n");
			return -1;
		}

		if (ret >= (int)sizeof(buf))
			ret = sizeof(buf) - 1;
		buf[ret] = '\0';
		printf("get data: %s\n", buf);
	}

    close(file);
    return 0;
}
