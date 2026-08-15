KERNELDIR ?= /lib/modules/$(shell uname -r)/build
USER_CC ?= cc
USER_CFLAGS ?= -O2 -Wall -Wextra

obj-m += prefetch_pmu_event.o

.PHONY: all clean probe

all:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules

probe: probe_prefetch_pmu

probe_prefetch_pmu: probe_prefetch_pmu.c prefetch_pmu_event.h
	$(USER_CC) $(USER_CFLAGS) -o $@ probe_prefetch_pmu.c

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean
	$(RM) probe_prefetch_pmu
