#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "prefetch_kcache_ioctl.h"

static unsigned char *probe_page;
static unsigned char *probe_line;

static inline u64 rdtscp_ordered(void)
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

static inline void clflush_one(const void *addr)
{
    asm volatile("clflush (%0)" : : "r"(addr) : "memory");
}

static void flush_probe_line(void)
{
    clflush_one(probe_line);
    asm volatile("mfence" : : : "memory");
}

static void prefetch_probe_line(void)
{
    asm volatile("prefetcht0 (%0)" : : "r"(probe_line) : "memory");
}

static u64 measure_probe_load(void)
{
    volatile unsigned char value;
    u64 t0;
    u64 t1;

    t0 = rdtscp_ordered();
    value = READ_ONCE(probe_line[0]);
    asm volatile("lfence" : : : "memory");
    t1 = rdtscp_ordered();

    (void)value;
    return t1 - t0;
}

static long prefetch_kcache_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    __u64 value;

    (void)file;

    switch (cmd) {
    case PREFETCH_KCACHE_IOC_GET_ADDR:
        value = (__u64)(uintptr_t)probe_line;
        if (copy_to_user((void __user *)arg, &value, sizeof(value)) != 0)
            return -EFAULT;
        return 0;

    case PREFETCH_KCACHE_IOC_FLUSH:
        flush_probe_line();
        return 0;

    case PREFETCH_KCACHE_IOC_MEASURE:
        value = measure_probe_load();
        if (copy_to_user((void __user *)arg, &value, sizeof(value)) != 0)
            return -EFAULT;
        return 0;

    case PREFETCH_KCACHE_IOC_PREFETCH:
        prefetch_probe_line();
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations prefetch_kcache_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = prefetch_kcache_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = prefetch_kcache_ioctl,
#endif
};

static struct miscdevice prefetch_kcache_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "prefetch_kcache_probe",
    .fops = &prefetch_kcache_fops,
    .mode = 0666,
};

static int __init prefetch_kcache_init(void)
{
    int ret;

    probe_page = (unsigned char *)get_zeroed_page(GFP_KERNEL);
    if (!probe_page)
        return -ENOMEM;

    probe_line = probe_page;
    memset(probe_line, 0x5a, PAGE_SIZE);
    flush_probe_line();

    ret = misc_register(&prefetch_kcache_dev);
    if (ret != 0) {
        free_page((unsigned long)probe_page);
        probe_page = NULL;
        probe_line = NULL;
        return ret;
    }

    pr_info("prefetch_kcache_probe: kernel probe line at %px\n", probe_line);
    return 0;
}

static void __exit prefetch_kcache_exit(void)
{
    misc_deregister(&prefetch_kcache_dev);
    free_page((unsigned long)probe_page);
    probe_page = NULL;
    probe_line = NULL;
}

module_init(prefetch_kcache_init);
module_exit(prefetch_kcache_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("JFJF");
MODULE_DESCRIPTION("Measure whether user-mode software prefetch warms a kernel virtual address");
