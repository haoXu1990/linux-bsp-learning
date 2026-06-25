#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/time.h>
#include <unistd.h>

#define LONG_PRESS_MS 3000

static long long get_time_ms(void)
{
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void do_reboot(void)
{
  printf("reboot now\n");
  fflush(stdout);

  sync();

  if (reboot(RB_AUTOBOOT) < 0)
    perror("reboot");
}

int main(int argc, char **argv)
{
  const char *dev_name;
  struct input_event ev;
  long long press_time_ms = 0;
  int pressed = 0;
  int fd;

  if (argc != 2) {
    printf("Usage: %s /dev/input/eventX\n", argv[0]);
    return -1;
  }

  dev_name = argv[1];

  fd = open(dev_name, O_RDONLY);
  if (fd < 0) {
    perror("open input event");
    return -1;
  }

  printf("listen %s, long press KEY_POWER %d ms to reboot\n",
         dev_name, LONG_PRESS_MS);
  fflush(stdout);

  while (1) {
    int ret = read(fd, &ev, sizeof(ev));

    if (ret < 0) {
      if (errno == EINTR)
        continue;

      perror("read input event");
      break;
    }

    if (ret != sizeof(ev)) {
      printf("read input event size error, ret = %d\n", ret);
      break;
    }

    if (ev.type != EV_KEY || ev.code != KEY_POWER)
      continue;

    // value = 1 表示按下，0 表示松开，2 表示长按重复事件
    if (ev.value == 0) {
      pressed = 1;
      press_time_ms = get_time_ms();
      printf("KEY_POWER down\n");
      fflush(stdout);
    } else if (ev.value == 1) {
      long long press_ms;

      if (!pressed) {
        printf("KEY_POWER up, but no down record, ignore\n");
        fflush(stdout);
        continue;
      }

      press_ms = get_time_ms() - press_time_ms;
      pressed = 0;

      if (press_ms >= LONG_PRESS_MS) {
        printf("long press, %lld ms, reboot\n", press_ms);
        fflush(stdout);
        do_reboot();
      } else {
        printf("short press, %lld ms, need %d ms, ignore\n",
               press_ms, LONG_PRESS_MS);
        fflush(stdout);
      }
    }
  }

  close(fd);
  return 0;
}
