#include <stdio.h>
#include <string.h>

#include "app_args.h"

static int expect_string(const char *name, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) != 0) {
        printf("%s failed: actual=%s expected=%s\n", name, actual, expected);
        return -1;
    }

    return 0;
}

int main(void)
{
    struct app_args args;
    char *default_argv[] = {"v4l2_camera_view", "/dev/video1"};
    char *fb1_argv[] = {"v4l2_camera_view", "/dev/video1", "/dev/fb1"};
    char *fb_test_argv[] = {"v4l2_camera_view", "--fb-test", "/dev/fb1"};
    char *invalid_argv[] = {"v4l2_camera_view"};

    if (parse_app_args(2, default_argv, &args) != 0)
        return -1;
    if (args.mode != APP_MODE_CAMERA)
        return -1;
    if (expect_string("default video", args.video_device, "/dev/video1") != 0)
        return -1;
    if (expect_string("default fb", args.fb_device, DEFAULT_FB_DEVICE) != 0)
        return -1;

    if (parse_app_args(3, fb1_argv, &args) != 0)
        return -1;
    if (args.mode != APP_MODE_CAMERA)
        return -1;
    if (expect_string("explicit video", args.video_device, "/dev/video1") != 0)
        return -1;
    if (expect_string("explicit fb", args.fb_device, "/dev/fb1") != 0)
        return -1;

    if (parse_app_args(3, fb_test_argv, &args) != 0)
        return -1;
    if (args.mode != APP_MODE_FB_TEST)
        return -1;
    if (args.video_device != NULL)
        return -1;
    if (expect_string("fb test device", args.fb_device, "/dev/fb1") != 0)
        return -1;

    if (parse_app_args(1, invalid_argv, &args) == 0)
        return -1;

    printf("app_args selftest passed\n");
    return 0;
}
