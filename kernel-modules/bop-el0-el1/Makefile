KERNELDIR ?= /lib/modules/$(shell uname -r)/build
USER_CC ?= cc
USER_CFLAGS ?= -O2 -Wall -Wextra -march=armv8.5-a+predres

obj-m += bop_el0_el1.o

.PHONY: all clean probe

all:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules

probe: test_bop_el0_el1

test_bop_el0_el1: test_bop_el0_el1.c bop_el0_el1_ioctl.h ../../until.h
	$(USER_CC) $(USER_CFLAGS) -I../.. -o $@ test_bop_el0_el1.c ../../re0-stride-prefetcher/until.c

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean
	$(RM) test_bop_el0_el1
