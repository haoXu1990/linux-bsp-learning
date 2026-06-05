#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const char *ev_type_name(unsigned short type)
{
  switch (type) {
  case EV_SYN:
    return "EV_SYN";
  case EV_KEY:
    return "EV_KEY";
  case EV_REL:
    return "EV_REL";
  case EV_ABS:
    return "EV_ABS";
  case EV_MSC:
    return "EV_MSC";
  default:
    return "EV_UNKNOWN";
  }
}

static const char *key_code_name(unsigned short code)
{
  switch (code) {
  case BTN_LEFT:
    return "BTN_LEFT";
  case BTN_RIGHT:
    return "BTN_RIGHT";
  case BTN_MIDDLE:
    return "BTN_MIDDLE";
  case BTN_SIDE:
    return "BTN_SIDE";
  case BTN_EXTRA:
    return "BTN_EXTRA";
  case KEY_L:
    return "KEY_L";
  case KEY_S:
    return "KEY_S";
  case KEY_ENTER:
    return "KEY_ENTER";
  default:
    return "KEY_UNKNOWN";
  }
}

static const char *rel_code_name(unsigned short code)
{
  switch (code) {
  case REL_X:
    return "REL_X";
  case REL_Y:
    return "REL_Y";
  case REL_WHEEL:
    return "REL_WHEEL";
  case REL_HWHEEL:
    return "REL_HWHEEL";
  default:
    return "REL_UNKNOWN";
  }
}

static void print_event(const struct input_event *ev)
{
  printf("%ld.%06ld type=%s(0x%04x) code=0x%04x value=%d",
         ev->time.tv_sec, ev->time.tv_usec, ev_type_name(ev->type), ev->type,
         ev->code, ev->value);

  if (ev->type == EV_KEY) {
    printf(" %s %s", key_code_name(ev->code),
           ev->value ? "pressed" : "released");
  } else if (ev->type == EV_REL) {
    printf(" %s delta=%d", rel_code_name(ev->code), ev->value);
  } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
    printf(" SYN_REPORT");
  }

  printf("\n");
  fflush(stdout);
}

int main(int argc, char **argv)
{
  const char *dev_name;
  struct input_event ev;
  char name[256] = "unknown";
  int fd;

  if (argc != 2) {
    printf("Usage: %s /dev/input/eventX\n", argv[0]);
    printf("Example: %s /dev/input/event1\n", argv[0]);
    return -1;
  }

  dev_name = argv[1];
  fd = open(dev_name, O_RDONLY);
  if (fd < 0) {
    perror("open input event");
    return -1;
  }

  if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
    snprintf(name, sizeof(name), "unknown");

  printf("listen %s, input name: %s\n", dev_name, name);
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

    print_event(&ev);
  }

  close(fd);
  return 0;
}
