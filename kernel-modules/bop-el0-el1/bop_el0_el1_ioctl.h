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
#define BOP_EL0_EL1_PAGE_STEP 977U
#define BOP_EL0_EL1_ROUND_STEP 131U

static inline uint32_t bop_el0_el1_pair_page(uint32_t index, uint32_t seed)
{
	return (uint32_t)(((uint64_t)seed * BOP_EL0_EL1_ROUND_STEP +
		(uint64_t)index * BOP_EL0_EL1_PAGE_STEP) %
		BOP_EL0_EL1_TRAIN_PAGES);
}

struct bop_el0_el1_request {
	uint64_t base_addr;
	uint32_t offset_lines;
	uint32_t repeats;
	uint32_t mode;
	uint32_t page_seed;
	uint64_t kernel_value;
};

#define BOP_EL0_EL1_IOC_TRIGGER \
	_IOWR(BOP_EL0_EL1_IOC_MAGIC, 0, struct bop_el0_el1_request)
#define BOP_EL0_EL1_IOC_TRAIN \
	_IOWR(BOP_EL0_EL1_IOC_MAGIC, 1, struct bop_el0_el1_request)

#define BOP_EL0_EL1_TRAIN_PAIRS 0U
#define BOP_EL0_EL1_TRAIN_STRIDE 1U

#endif
