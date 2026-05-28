CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
KDIR ?= /lib/modules/$(shell uname -r)/build

USER_TARGETS := prefetch_any_va_demo prefetch_kernel_cache_user
obj-m += prefetch_kcache_probe.o

ifneq ($(KERNELRELEASE),)

else

.PHONY: all users module clean

all: users

users: $(USER_TARGETS)

prefetch_any_va_demo: prefetch_any_va_demo.c
	$(CC) $(CFLAGS) -o $@ $<

prefetch_kernel_cache_user: prefetch_kernel_cache_user.c prefetch_kcache_ioctl.h
	$(CC) $(CFLAGS) -o $@ $<

module: prefetch_kcache_probe.c prefetch_kcache_ioctl.h
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	rm -f $(USER_TARGETS)
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
