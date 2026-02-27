#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <signal.h>

static const char *dev_path = "/dev/gpio_irq_drv";

int main(int argc, char **argv) {
    if (argc >= 2) dev_path = argv[1];

    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        printf("open error: %d\n", fd);
        return 1;
    }

    printf("read from %s ... (Ctrl+C to quit)\n", dev_path);

    for (;;) {
        unsigned char s = 0;
        ssize_t n = read(fd, &s, 1);
        if (n < 0) {

            printf("read error: %d\n",n);
            break;
        }
        if (n == 0) {
            printf("read error eof \n.");
            break;
        }


        printf("key: %s (%u)\n", s ? "DOWN" : "UP", (unsigned)s);
    }

    close(fd);
    return 0;
}
