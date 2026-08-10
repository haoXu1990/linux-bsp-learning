CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -O2 -g
TARGET := mxc-csi-pxp-demo

.PHONY: all clean

all: $(TARGET)

$(TARGET): mxc-csi-pxp-demo.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
