KDIR ?= /lib/modules/$(shell uname -r)/build
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

TARGET := kernel_stride_pf_user
obj-m += kernel_stride_pf_probe.o

.PHONY: all user module clean

all: user module

user: $(TARGET)

$(TARGET): kernel_stride_pf_user.c kernel_stride_pf_ioctl.h
	$(CC) $(CFLAGS) -o $@ kernel_stride_pf_user.c

module: kernel_stride_pf_probe.c kernel_stride_pf_ioctl.h
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	rm -f $(TARGET)
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
