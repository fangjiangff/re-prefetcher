#ifndef PREFETCH_PMU_IOCTL_H
#define PREFETCH_PMU_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define PREFETCH_PMU_DEVICE_PATH "/dev/prefetch_pmu_event"
#define PREFETCH_PMU_IOC_MAGIC 0xb7
#define PREFETCH_PMU_IOC_REARM _IO(PREFETCH_PMU_IOC_MAGIC, 0)

#endif
