// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "bop_el0_el1_ioctl.h"

#ifndef BOP_EL0_EL1_SHARED_USER_PAGES
#define BOP_EL0_EL1_SHARED_USER_PAGES 1
#endif

#if !BOP_EL0_EL1_SHARED_USER_PAGES
static u8 *training_pool;
#endif
static u32 training_slots[BOP_EL0_EL1_POOL_PAGES];
static u32 prepared_offset_lines;

static u64 splitmix64_next(u64 *state)
{
	u64 value;

	*state += 0x9e3779b97f4a7c15ULL;
	value = *state;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

static void prepare_training_slots(u32 offset_lines)
{
	u64 rng = BOP_EL0_EL1_PAGE_SEED ^ 0x9e3779b97f4a7c15ULL ^
		offset_lines;

	for (u32 i = 0; i < BOP_EL0_EL1_POOL_PAGES; i++)
		training_slots[i] = i;
	for (u32 i = BOP_EL0_EL1_POOL_PAGES - 1; i > 0; i--) {
		u32 j = (u32)(splitmix64_next(&rng) % (i + 1));
		u32 temporary = training_slots[i];

		training_slots[i] = training_slots[j];
		training_slots[j] = temporary;
	}
}

#if !BOP_EL0_EL1_SHARED_USER_PAGES
static noinline u8 train_load(const u8 *address)
{
	u8 value;

	asm volatile("ldrb %w0, [%1]"
		     : "=r"(value) : "r"(address) : "memory");
	return value;
}
#endif

#if BOP_EL0_EL1_SHARED_USER_PAGES
static noinline int train_load_user(u8 *value, const u8 __user *address)
{
	return get_user(*value, address);
}
#endif

#if !BOP_EL0_EL1_SHARED_USER_PAGES
static void flush_training_pool(void)
{
	for (size_t offset = 0;
	     offset < BOP_EL0_EL1_POOL_PAGES * PAGE_SIZE;
	     offset += 64)
		asm volatile("dc civac, %0" :: "r"(training_pool + offset) :
			     "memory");
	asm volatile("dsb sy\n\tisb" ::: "memory");
}
#endif

/* Match EL0 training spacing; NOPs do not enforce hardware ordering. */
static inline void training_wait(void)
{
	for (int k = 0; k < 100; k++)
		asm volatile("nop" ::: "memory");
}

static long bop_el0_el1_ioctl(struct file *file, unsigned int command,
				      unsigned long argument)
{
	struct bop_el0_el1_request request;
	unsigned long delta;
	u8 value = 0;

	(void)file;
	if (command != BOP_EL0_EL1_IOC_FLUSH &&
	    command != BOP_EL0_EL1_IOC_TRAIN &&
	    command != BOP_EL0_EL1_IOC_SWITCH &&
	    command != BOP_EL0_EL1_IOC_TRIGGER)
		return -ENOTTY;
	if (command == BOP_EL0_EL1_IOC_SWITCH)
		return 0;
	if (copy_from_user(&request, (void __user *)argument,
			   sizeof(request)) != 0)
		return -EFAULT;
	if (command == BOP_EL0_EL1_IOC_TRIGGER) {
		/* Read X only; X+D is reserved for the EL0 cache probe. */
		if (get_user(value, (const u8 __user *)(uintptr_t)request.base_addr))
			return -EFAULT;
		goto done;
	}
	if (request.offset_lines == 0 || request.offset_lines >= 64)
		return -EINVAL;
	if (command == BOP_EL0_EL1_IOC_FLUSH) {
		prepare_training_slots(request.offset_lines);
		prepared_offset_lines = request.offset_lines;
#if !BOP_EL0_EL1_SHARED_USER_PAGES
		flush_training_pool();
#endif
		goto done;
	}
	if (request.repeats == 0 ||
	    request.offset_lines != prepared_offset_lines ||
	    request.first_pair > BOP_EL0_EL1_TRAIN_PAGES - request.repeats)
		return -EINVAL;
	delta = (unsigned long)request.offset_lines * 64UL;
#if BOP_EL0_EL1_SHARED_USER_PAGES
	if (request.base_addr > ULONG_MAX -
		BOP_EL0_EL1_POOL_PAGES * PAGE_SIZE ||
	    !access_ok((const u8 __user *)(uintptr_t)request.base_addr,
		       BOP_EL0_EL1_POOL_PAGES * PAGE_SIZE))
		return -EFAULT;
#endif
	{
		u64 rng = BOP_EL0_EL1_PAGE_SEED ^
			((u64)request.offset_lines << 32);
		u32 line_count = PAGE_SIZE / 64 - request.offset_lines;

		for (u32 i = 0; i < request.first_pair + request.repeats; i++) {
			u32 line = (u32)(splitmix64_next(&rng) % line_count);
			u32 page = training_slots[i];
			if (i < request.first_pair)
				continue;

#if BOP_EL0_EL1_SHARED_USER_PAGES
			const u8 __user *pair_base =
				(const u8 __user *)(uintptr_t)request.base_addr +
				(unsigned long)page * PAGE_SIZE + (unsigned long)line * 64UL;
			int ret;

			ret = train_load_user(&value, pair_base);
			if (ret != 0)
				return ret;
			training_wait();
			ret = train_load_user(&value, pair_base + delta);
			if (ret != 0)
				return ret;
#else
			const u8 *pair_base = training_pool +
				(unsigned long)page * PAGE_SIZE + (unsigned long)line * 64UL;

		value = train_load(pair_base);
		training_wait();
		value = train_load(pair_base + delta);
#endif
		training_wait();
		}
	}

done:
	request.kernel_value = value;

	if (copy_to_user((void __user *)argument, &request,
			 sizeof(request)) != 0)
		return -EFAULT;
	return 0;
}

static const struct file_operations bop_el0_el1_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = bop_el0_el1_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = bop_el0_el1_ioctl,
#endif
};

static struct miscdevice bop_el0_el1_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "bop_el0_el1",
	.fops = &bop_el0_el1_fops,
	.mode = 0600,
};

static int __init bop_el0_el1_init(void)
{
	int ret;

#if !BOP_EL0_EL1_SHARED_USER_PAGES
	training_pool = vzalloc(BOP_EL0_EL1_POOL_PAGES * PAGE_SIZE);
	if (training_pool == NULL)
		return -ENOMEM;
#endif
	ret = misc_register(&bop_el0_el1_device);
	if (ret != 0) {
#if !BOP_EL0_EL1_SHARED_USER_PAGES
		vfree(training_pool);
#endif
		return ret;
	}
	return 0;
}

static void __exit bop_el0_el1_exit(void)
{
	misc_deregister(&bop_el0_el1_device);
#if !BOP_EL0_EL1_SHARED_USER_PAGES
	vfree(training_pool);
#endif
}

module_init(bop_el0_el1_init);
module_exit(bop_el0_el1_exit);

MODULE_AUTHOR("re-prefetcher contributors");
MODULE_DESCRIPTION("EL1 training for cross-EL0/EL1 BOP state");
MODULE_LICENSE("GPL");
