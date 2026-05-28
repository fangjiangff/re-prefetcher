#ifndef KERNEL_STRIDE_PF_IOCTL_H
#define KERNEL_STRIDE_PF_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define KERNEL_STRIDE_PF_IOC_MAGIC 's'

#define KERNEL_STRIDE_PF_LINE_SIZE 64
#define KERNEL_STRIDE_PF_STRIDE_LINES 11
#define KERNEL_STRIDE_PF_TRAIN_STEP 15
#define KERNEL_STRIDE_PF_PROBE_LINES 200
#define KERNEL_STRIDE_PF_ARRAY_LINES 256

struct kernel_stride_pf_measure {
    __u32 line;
    __u32 reserved;
    __u64 cycles;
};

#define KERNEL_STRIDE_PF_IOC_GET_BASE \
    _IOR(KERNEL_STRIDE_PF_IOC_MAGIC, 1, __u64)
#define KERNEL_STRIDE_PF_IOC_FLUSH \
    _IO(KERNEL_STRIDE_PF_IOC_MAGIC, 2)
#define KERNEL_STRIDE_PF_IOC_MEASURE \
    _IOWR(KERNEL_STRIDE_PF_IOC_MAGIC, 3, struct kernel_stride_pf_measure)
#define KERNEL_STRIDE_PF_IOC_KERNEL_TRAIN \
    _IO(KERNEL_STRIDE_PF_IOC_MAGIC, 4)

#endif
