// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "bop_el0_el1_ioctl.h"

static long bop_el0_el1_ioctl(struct file *file, unsigned int command,
				      unsigned long argument)
{
	struct bop_el0_el1_request request;
	unsigned long delta;
	const unsigned char __user *base;
	const unsigned char __user *pair_base;
	u8 value;
	int ret;

	(void)file;
	if (command != BOP_EL0_EL1_IOC_TRIGGER &&
	    command != BOP_EL0_EL1_IOC_TRAIN)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)argument,
			   sizeof(request)) != 0)
		return -EFAULT;
	if (request.offset_lines == 0 || request.offset_lines >= 64)
		return -EINVAL;
	if (command == BOP_EL0_EL1_IOC_TRAIN && request.repeats == 0)
		return -EINVAL;
	if (command == BOP_EL0_EL1_IOC_TRAIN &&
	    request.repeats > BOP_EL0_EL1_TRAIN_PAGES)
		return -EINVAL;
	if (command == BOP_EL0_EL1_IOC_TRAIN &&
	    request.mode != BOP_EL0_EL1_TRAIN_PAIRS &&
	    request.mode != BOP_EL0_EL1_TRAIN_STRIDE)
		return -EINVAL;
	delta = (unsigned long)request.offset_lines * 64UL;
	if (request.base_addr > ULONG_MAX - delta - 1UL)
		return -EFAULT;

	base = (const unsigned char __user *)(uintptr_t)request.base_addr;
	if (command == BOP_EL0_EL1_IOC_TRAIN) {
		unsigned long span;

		if (request.mode == BOP_EL0_EL1_TRAIN_STRIDE) {
			if (request.repeats > (ULONG_MAX - 1UL) / delta)
				return -EFAULT;
			span = (unsigned long)request.repeats * delta + 1UL;
		} else {
			if (BOP_EL0_EL1_TRAIN_PAGES - 1UL >
			    (ULONG_MAX - delta - 1UL) / PAGE_SIZE)
				return -EFAULT;
			span = (BOP_EL0_EL1_TRAIN_PAGES - 1UL) * PAGE_SIZE +
				delta + 1UL;
		}
		if (!access_ok(base, span))
			return -EFAULT;
		for (u32 i = 0; i < request.repeats; i++) {
			if (request.mode == BOP_EL0_EL1_TRAIN_STRIDE) {
				pair_base = base + (unsigned long)i * delta;
				ret = get_user(value, pair_base);
			} else {
				u32 page = bop_el0_el1_pair_page(i, request.page_seed);

				pair_base = base + (unsigned long)page * PAGE_SIZE;
				ret = get_user(value, pair_base);
			}
			if (ret != 0)
				return ret;
			barrier();
			if (request.mode == BOP_EL0_EL1_TRAIN_PAIRS) {
				ret = get_user(value, pair_base + delta);
				if (ret != 0)
					return ret;
				barrier();
			}
		}
	} else {
		/* EL1 trigger: touch X only; X+D is left for the user probe. */
		if (!access_ok(base, delta + 1UL))
			return -EFAULT;
		ret = get_user(value, base);
		if (ret != 0)
			return ret;
		barrier();
	}
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
	return misc_register(&bop_el0_el1_device);
}

static void __exit bop_el0_el1_exit(void)
{
	misc_deregister(&bop_el0_el1_device);
}

module_init(bop_el0_el1_init);
module_exit(bop_el0_el1_exit);

MODULE_AUTHOR("re-prefetcher contributors");
MODULE_DESCRIPTION("EL1 trigger for testing cross-EL0/EL1 BOP state");
MODULE_LICENSE("GPL");
