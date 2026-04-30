#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main()
{
    int fd;
    char val;

    fd = open("/dev/gpio_key", O_RDWR);

    while (1)
    {
        read(fd, &val, 1);

        if (val)
            printf("key pressed\n");

        write(fd, &val, 1);

        sleep(1);
    }
}
