#include "app_args.h"

int parse_app_args(int argc, char **argv, struct app_args *args)
{
    if (!args || argc < 2 || argc > 3)
        return -1;

    args->video_device = argv[1];
    args->fb_device = argc == 3 ? argv[2] : DEFAULT_FB_DEVICE;

    return 0;
}
