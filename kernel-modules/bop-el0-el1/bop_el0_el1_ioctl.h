#ifndef BOP_EL0_EL1_IOCTL_H
#define BOP_EL0_EL1_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define BOP_EL0_EL1_DEVICE_PATH "/dev/bop_el0_el1"
#define BOP_EL0_EL1_IOC_MAGIC 0xb8
#define BOP_EL0_EL1_TRAIN_PAGES 4095U
#define BOP_EL0_EL1_POOL_PAGES 8192U
#define BOP_EL0_EL1_PAGE_SEED 0x243f6a8885a308d3ULL

struct bop_el0_el1_request {
	uint64_t base_addr;
	uint32_t offset_lines;
	uint32_t repeats;
	uint32_t first_pair;
	uint64_t kernel_value;
};

#define BOP_EL0_EL1_IOC_FLUSH \
	_IOWR(BOP_EL0_EL1_IOC_MAGIC, 0, struct bop_el0_el1_request)
#define BOP_EL0_EL1_IOC_TRAIN \
	_IOWR(BOP_EL0_EL1_IOC_MAGIC, 1, struct bop_el0_el1_request)
#define BOP_EL0_EL1_IOC_TRIGGER \
	_IOWR(BOP_EL0_EL1_IOC_MAGIC, 3, struct bop_el0_el1_request)
#define BOP_EL0_EL1_IOC_SWITCH _IO(BOP_EL0_EL1_IOC_MAGIC, 2)

#endif
