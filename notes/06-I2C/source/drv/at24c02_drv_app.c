#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AT24C02_DEV_PATH "/dev/at24c02"
#define AT24C02_SIZE     256
// 可以按照下面这样划分 空间
// 0x10 ~ 0x2f 保存设备名，最多 32 字节
// 0x30 ~ 0x3f 保存序列号，最多 16 字节
// 0x40 ~ 0x7f 保存配置参数，最多 64 字节

static void print_usage(const char *prog)
{
	printf("Usage:\n");
	printf("  Write: %s 0 <addr> <string>\n", prog);
	printf("  Read : %s 1 <addr>\n", prog);
	printf("Example:\n");
	printf("  %s 0 0x10 \"hello\"\n", prog);
	printf("  %s 1 0x10\n", prog);
}

static int parse_ulong(const char *str, unsigned long *value)
{
	char *end;

	errno = 0;
	*value = strtoul(str, &end, 0);
	if (errno || *end != '\0')
		return -1;

	return 0;
}

int main(int argc, char **argv)
{
	unsigned char buf[AT24C02_SIZE + 1];
	unsigned long op;
	unsigned long addr;
	size_t len;
	ssize_t ret;
	int fd;

	if (argc != 3 && argc != 4) {
		print_usage(argv[0]);
		return 1;
	}

	if (parse_ulong(argv[1], &op) || (op != 0 && op != 1)) {
		fprintf(stderr, "invalid operation: %s\n", argv[1]);
		print_usage(argv[0]);
		return 1;
	}

	if ((op == 0 && argc != 4) || (op == 1 && argc != 3)) {
		print_usage(argv[0]);
		return 1;
	}

	if (parse_ulong(argv[2], &addr) || addr >= AT24C02_SIZE) {
		fprintf(stderr, "invalid address: %s\n", argv[2]);
		return 1;
	}

	fd = open(AT24C02_DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " AT24C02_DEV_PATH);
		return 1;
	}

	if (lseek(fd, (off_t)addr, SEEK_SET) < 0) {
		perror("lseek");
		close(fd);
		return 1;
	}

	if (op == 0) {
		len = strlen(argv[3]) + 1;
		if (addr + len > AT24C02_SIZE) {
			fprintf(stderr, "string is too long for address 0x%lx\n", addr);
			close(fd);
			return 1;
		}

		ret = write(fd, argv[3], len);
		if (ret < 0) {
			perror("write");
			close(fd);
			return 1;
		}

		if ((size_t)ret != len) {
			fprintf(stderr, "partial write: %zd/%zu bytes\n", ret, len);
			close(fd);
			return 1;
		}

		printf("write %zu bytes to 0x%02lx: %s\n", len, addr, argv[3]);
	} else {
		len = AT24C02_SIZE - addr;
		ret = read(fd, buf, len);
		if (ret < 0) {
			perror("read");
			close(fd);
			return 1;
		}

		buf[ret] = '\0';
		printf("%s\n", buf);
	}

	close(fd);
	return 0;
}
