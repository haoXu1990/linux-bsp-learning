#ifndef APP_ARGS_H
#define APP_ARGS_H

#define DEFAULT_FB_DEVICE "/dev/fb0"

struct app_args {
    const char *video_device;
    const char *fb_device;
};

int parse_app_args(int argc, char **argv, struct app_args *args);

#endif
