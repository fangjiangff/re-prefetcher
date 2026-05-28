#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "kernel_stride_pf_ioctl.h"

#define ARRAY_BYTES \
    (KERNEL_STRIDE_PF_ARRAY_LINES * KERNEL_STRIDE_PF_LINE_SIZE)

static unsigned char *array2;
static unsigned int array_order;

#if defined(CONFIG_X86)
#define KERNEL_STRIDE_PF_ARCH "x86"

static inline u64 read_timer_ordered(void)
{
    u32 lo;
    u32 hi;
    u32 aux;

    asm volatile("lfence\n\t"
                 "rdtscp\n\t"
                 "lfence"
                 : "=a"(lo), "=d"(hi), "=c"(aux)
                 :
                 : "memory");

    return ((u64)hi << 32) | lo;
}

static inline void flush_one_line(const void *addr)
{
    asm volatile("clflush (%0)" : : "r"(addr) : "memory");
}

static inline void full_barrier(void)
{
    asm volatile("mfence" : : : "memory");
}

static inline void load_barrier(void)
{
    asm volatile("lfence" : : : "memory");
}

#elif defined(CONFIG_ARM64)
#define KERNEL_STRIDE_PF_ARCH "arm64"

static inline u64 read_timer_ordered(void)
{
    u64 value;

    asm volatile("isb\n\t"
                 "mrs %0, cntvct_el0\n\t"
                 "isb"
                 : "=r"(value)
                 :
                 : "memory");
    return value;
}

static inline void flush_one_line(const void *addr)
{
    asm volatile("dc civac, %0" : : "r"(addr) : "memory");
}

static inline void full_barrier(void)
{
    asm volatile("dsb ish" : : : "memory");
}

static inline void load_barrier(void)
{
    asm volatile("dsb ishld" : : : "memory");
}

#else
#error "kernel_stride_pf_probe supports only x86 and arm64"
#endif

static void flush_array2(void)
{
    int line;

    for (line = 0; line < KERNEL_STRIDE_PF_ARRAY_LINES; ++line) {
        flush_one_line(array2 + line * KERNEL_STRIDE_PF_LINE_SIZE);
    }

    full_barrier();
}

static u64 measure_line(int line)
{
    volatile unsigned char value;
    unsigned char *addr = array2 + line * KERNEL_STRIDE_PF_LINE_SIZE;
    u64 t0;
    u64 t1;

    t0 = read_timer_ordered();
    value = READ_ONCE(*addr);
    load_barrier();
    t1 = read_timer_ordered();

    (void)value;
    return t1 - t0;
}

static void kernel_train_array2(void)
{
    int repeat;
    int step;
    volatile unsigned char value;

    for (repeat = 0; repeat < 5; ++repeat) {
        for (step = 0; step < KERNEL_STRIDE_PF_TRAIN_STEP; ++step) {
            unsigned char *addr = array2 +
                step * KERNEL_STRIDE_PF_STRIDE_LINES * KERNEL_STRIDE_PF_LINE_SIZE;

            value = READ_ONCE(*addr);
            full_barrier();
        }
    }

    (void)value;
}

static long kernel_stride_pf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct kernel_stride_pf_measure measurement;
    __u64 value;

    (void)file;

    switch (cmd) {
    case KERNEL_STRIDE_PF_IOC_GET_BASE:
        value = (__u64)(uintptr_t)array2;
        if (copy_to_user((void __user *)arg, &value, sizeof(value)) != 0)
            return -EFAULT;
        return 0;

    case KERNEL_STRIDE_PF_IOC_FLUSH:
        flush_array2();
        return 0;

    case KERNEL_STRIDE_PF_IOC_MEASURE:
        if (copy_from_user(&measurement, (void __user *)arg, sizeof(measurement)) != 0)
            return -EFAULT;
        if (measurement.line >= KERNEL_STRIDE_PF_PROBE_LINES)
            return -EINVAL;

        measurement.cycles = measure_line(measurement.line);
        if (copy_to_user((void __user *)arg, &measurement, sizeof(measurement)) != 0)
            return -EFAULT;
        return 0;

    case KERNEL_STRIDE_PF_IOC_KERNEL_TRAIN:
        kernel_train_array2();
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations kernel_stride_pf_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = kernel_stride_pf_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = kernel_stride_pf_ioctl,
#endif
};

static struct miscdevice kernel_stride_pf_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "kernel_stride_pf_probe",
    .fops = &kernel_stride_pf_fops,
    .mode = 0666,
};

static int __init kernel_stride_pf_init(void)
{
    int ret;

    array_order = get_order(ARRAY_BYTES);
    array2 = (unsigned char *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, array_order);
    if (!array2)
        return -ENOMEM;

    memset(array2, 0x5a, ARRAY_BYTES);
    flush_array2();

    ret = misc_register(&kernel_stride_pf_dev);
    if (ret != 0) {
        free_pages((unsigned long)array2, array_order);
        array2 = NULL;
        return ret;
    }

    pr_info("kernel_stride_pf_probe: arch=%s array2 kernel VA at %px, bytes=%u\n",
            KERNEL_STRIDE_PF_ARCH, array2, ARRAY_BYTES);
    return 0;
}

static void __exit kernel_stride_pf_exit(void)
{
    misc_deregister(&kernel_stride_pf_dev);
    free_pages((unsigned long)array2, array_order);
    array2 = NULL;
}

module_init(kernel_stride_pf_init);
module_exit(kernel_stride_pf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("Probe whether user-mode prefetches to kernel VAs trigger kernel-address hardware prefetching");
