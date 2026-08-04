CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -O2 -g
TARGET := ov5640_camera_test

.PHONY: all clean

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
