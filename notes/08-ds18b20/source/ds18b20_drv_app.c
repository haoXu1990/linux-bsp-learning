#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main()
{
    int fd;
    int val;
    int ret;
    fd = open("/dev/ds18b20", O_RDWR);

    while (1)
    {
       ret = read(fd, &val, sizeof(val));

        if (ret < 0 ) {
            printf("key pressed\n");
        } else {
            printf("read ds18b20 temp = %d \n", val);
        }
        // 这里先随意了延时读了，先跑流程；
        sleep(1);
    }
}
