
CC := $(CROSS_COMPILE)gcc
HOSTCC ?= gcc

CFLAGS ?= -Wall -Wextra -O2 -g
TARGET := v4l2_camera_view

SRC := main.c yuyv_rgb565.c app_args.c
TEST_TARGETS := test_yuyv_rgb565 test_app_args

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC) yuyv_rgb565.h app_args.h imx_fb_alpha.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: $(TEST_TARGETS)
	./test_yuyv_rgb565
	./test_app_args

test_yuyv_rgb565: test_yuyv_rgb565.c yuyv_rgb565.c yuyv_rgb565.h
	$(HOSTCC) $(CFLAGS) -o $@ test_yuyv_rgb565.c yuyv_rgb565.c

test_app_args: test_app_args.c app_args.c app_args.h
	$(HOSTCC) $(CFLAGS) -o $@ test_app_args.c app_args.c

clean:
	rm -f $(TARGET) $(TEST_TARGETS)
