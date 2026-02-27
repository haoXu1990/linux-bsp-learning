#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>


static const char *dev_path = "/dev/gpio_irq_drv";



int main(int argc, char **argv) {
    if (argc >= 2) dev_path = argv[1];

    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        printf("open error: %d\n", fd);
        return 1;
    }

    int epfd = epoll_create1(0);

    struct epoll_event ev, events[1];


    ev.events = EPOLLIN;
    ev.data.fd = fd;
   if ( epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
       printf("epoll_ctl error: \n");
       close(fd);
       return 1;
   }

    printf("read from %s ... (Ctrl+C to quit)\n", dev_path);

    while (1) {
        unsigned char s = 0;
        printf("epoll waitting ...\n");
        int ret = epoll_wait(epfd, events, 1, -1);
        if (ret == -1) {
            printf("epoll_wait error: \n");
            break;
        }
        else if (ret == 0) {
            printf("timeout\n");
            continue;
        }

        if (events[0].data.fd == fd) {
            ssize_t n = read(fd, &s, 1);

            if (n < 0) {
                printf("read error: %d\n",n);
                continue;
            } else if (n > 0) {
                printf("key: %s (%u)\n", s ? "DOWN" : "UP", (unsigned)s);
            } else {
                printf("read error eof \n.");
                continue;
            }

        }

    }

    close(fd);
    return 0;
}
