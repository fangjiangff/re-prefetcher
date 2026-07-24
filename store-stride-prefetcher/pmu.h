#ifndef STORE_STRIDE_PREFETCHER_PMU_H
#define STORE_STRIDE_PREFETCHER_PMU_H

#include <stdint.h>

#ifndef PMU_CORE_X925
#define PMU_CORE_X925 0
#endif

#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif

#ifndef PMU_WINDOW_NAME
#define PMU_WINDOW_NAME "training-only-per-round"
#endif

#if defined(__aarch64__)

#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

struct pmu_counter {
    const char *name;
    uint64_t event;
    int fd;
    uint64_t accumulated_value;
    uint64_t accumulated_time_enabled;
    uint64_t accumulated_time_running;
};

struct pmu_read_value {
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
};

#define PMU_COUNTER(counter_name, counter_event) \
    {counter_name, counter_event, -1, 0, 0, 0}

#if PMU_CORE_X925 && PMU_CORE_A55
#error "Only one PMU_CORE_* target can be selected"
#endif

#if PMU_CORE_X925
static struct pmu_counter pmu_counters[] = {
    // for hardware prefetcher
    PMU_COUNTER("l1d_cache_hwprf", 0x8154),//Level 1 data cache hardware prefetch
    PMU_COUNTER("l2d_cache_hwprf", 0x8155),// Level 2 data cache hardware prefetch

        // for store
    // {"st_retired", 0x0007, -1},
    // {"st_spec",    0x0071, -1},//Operation speculatively executed, store
    // // l1 d
    // {"l1d_cache_miss", 0x8144, -1},
    // {"l1d_cache_refill", 0x0003, -1},

    // // // L2 d
    // {"l2d_cache_wr", 0x0051, -1},
    // {"l2d_cache_miss", 0x814c, -1},
    // {"L2D_CACHE_REFILL_RD", 0x0052, -1},
    // {"L2D_CACHE_REFILL_WR", 0x0053, -1},
    // {"L2D_CACHE_WB_VICTIM", 0x0056, -1},
    // {"L2D_CACHE_WB_CLEAN", 0x0057, -1},
    // {"L2D_CACHE_INVAL", 0x0058, -1},
    // {"l2d_cache_refill", 0x0017, -1},


    // // for software prefetch
    // {"prf_spec", 0x8087, -1},
    // // {"l1d_cache_prfm", 0x8142, -1},
    // {"l1d_cache_refill_prfm", 0x8146, -1},
    // // // {"l2d_cache_prfm", 0x814a, -1},
    // {"l2d_cache_refill_prfm", 0x814e, -1},
    // // {"l3d_cache_refill_prfm", 0x8153, -1},
    // {"l3d_cache", 0x002b, -1},
    // {"l3d_cache_refill", 0x002a, -1},
    // // {"l3d_cache_allocate", 0x0029, -1},
    // // {"l3d_cache_lmiss_rd", 0x400b, -1},
    // // for tlb
    // {"l1d_tlb", 0x0025, -1},
    // {"l1d_tlb_refill", 0x0005, -1},
    // {"l2d_tlb", 0x002f, -1},
    // {"l2d_tlb_refill", 0x002d, -1},
    // {"dtlb_walk", 0x0034, -1},
    //
    // {"cpu_cycles", 0x0011, -1},
    // {"inst_retired", 0x0008, -1},
    // {"ll_cache_miss_rd", 0x0037, -1},
    // {"stall_backend_membound", 0x8164, -1},
};
#elif PMU_CORE_A55
/* Cortex-A55 exposes prefetch-related cache refill events as 0xC0..0xC2. */
static struct pmu_counter pmu_counters[] = {
    PMU_COUNTER("l1d_cache_refill_prefetch", 0x00c2),// L1D linefill from prefetcher
    PMU_COUNTER("l2d_cache_refill_prefetch", 0x00c1),// L2/cluster linefill from prefetcher
    PMU_COUNTER("l3d_cache_refill_prefetch", 0x00c0),// L3 linefill from hardware prefetcher
};
#else
/* A725 exposes combined hardware/software L2 prefetch events. */
static struct pmu_counter pmu_counters[] = {
    // The counter counts each fetch triggered by L1 prefetchers
    PMU_COUNTER("l1d_cache_hwprf", 0x8154),//L1 数据预取器触发的 fetch 数；判断预取器是否启动
    // The counter counts each refill triggered by L1 prefetchers
    PMU_COUNTER("l1d_cache_refill_hwprf", 0x81bc),// Level 1 data cache refill, hardware prefetch
    // The counter counts each fetch counted by either L2D_CACHE_HWPRF or L2D_CACHE_PRFM
    PMU_COUNTER("l2d_cache_prf", 0x8285),//L2 预取命中？
    //The counter counts each refill counted by either L2D_CACHE_REFILL_HWPRF or L2D_CACHE_REFILL_PRFM.
    PMU_COUNTER("l2d_cache_refill_prf", 0x828d),//preload/prefetch 引起的 L2 refill；同时包含硬件预取和软件 PRFM
    // {"l1d_cache_refill", 0x0003, -1},
    // {"l2d_cache_refill", 0x0017, -1},
    // {"ll_cache_refill", 0x829a, -1},
    // {"l3d_cache", 0x002b, -1},
    // {"l3d_cache_refill", 0x002a, -1},
    // {"l3d_cache_allocate", 0x0029, -1},
    // {"l3d_cache_lmiss_rd", 0x400b, -1},
    // {"l1d_tlb", 0x0025, -1},
    // {"l1d_tlb_refill", 0x0005, -1},
    // {"l2d_tlb", 0x002f, -1},
    // {"l2d_tlb_refill", 0x002d, -1},
    // {"dtlb_walk", 0x0034, -1},
    // {"cpu_cycles", 0x0011, -1},
    // {"inst_retired", 0x0008, -1},
    // {"ll_cache_miss_rd", 0x0037, -1},
    // {"stall_backend_membound", 0x8164, -1},
};
#endif

#define PMU_COUNTER_COUNT \
    ((int)(sizeof(pmu_counters) / sizeof(pmu_counters[0])))

static const char *pmu_core_name(void) {
#if PMU_CORE_X925
    return "Cortex-X925";
#elif PMU_CORE_A55
    return "Cortex-A55";
#else
    return "Cortex-A725";
#endif
}

static long pmu_perf_event_open(struct perf_event_attr *attr, pid_t pid,
                                int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int pmu_read_text_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    ssize_t len = read(fd, buf, size - 1);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    if (len <= 0) {
        return -1;
    }

    buf[len] = '\0';
    return 0;
}

static int pmu_cpu_is_in_list(int cpu, const char *list) {
    const char *p = list;

    while (*p != '\0') {
        char *end;
        long first = strtol(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }

        long last = first;
        p = end;
        if (*p == '-') {
            p++;
            last = strtol(p, &end, 10);
            p = end;
        }

        if (cpu >= first && cpu <= last) {
            return 1;
        }
        while (*p != '\0' && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
    return 0;
}

static int pmu_read_type(const char *pmu_name, uint32_t *type) {
    char path[256];
    char buf[128];

    snprintf(path, sizeof(path),
             "/sys/bus/event_source/devices/%s/type", pmu_name);
    if (pmu_read_text_file(path, buf, sizeof(buf)) != 0) {
        return -1;
    }

    char *end;
    unsigned long value = strtoul(buf, &end, 0);
    if (end == buf || value > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    *type = (uint32_t)value;
    return 0;
}

static int pmu_find_type(uint32_t *type, char *name, size_t name_size) {
    const char *forced_name = getenv("PMU_DEVICE");
    if (forced_name != NULL && forced_name[0] != '\0') {
        if (pmu_read_type(forced_name, type) != 0) {
            fprintf(stderr, "PMU: cannot read PMU_DEVICE=%s: %s\n",
                    forced_name, strerror(errno));
            return -1;
        }
        snprintf(name, name_size, "%s", forced_name);
        return 0;
    }

    int cpu = 0;
    if (syscall(__NR_getcpu, &cpu, NULL, NULL) != 0) {
        fprintf(stderr, "PMU: getcpu failed: %s\n", strerror(errno));
        return -1;
    }
    for (int index = -1; index < 32; index++) {
        char candidate[64];
        char cpus_path[256];
        char cpus[256];

        if (index < 0) {
            snprintf(candidate, sizeof(candidate), "armv8_pmuv3");
        } else {
            snprintf(candidate, sizeof(candidate), "armv8_pmuv3_%d", index);
        }
        snprintf(cpus_path, sizeof(cpus_path),
                 "/sys/bus/event_source/devices/%s/cpus", candidate);
        if (pmu_read_text_file(cpus_path, cpus, sizeof(cpus)) != 0 ||
            !pmu_cpu_is_in_list(cpu, cpus)) {
            continue;
        }
        if (pmu_read_type(candidate, type) == 0) {
            snprintf(name, name_size, "%s", candidate);
            return 0;
        }
    }

    /* Some kernels expose the Arm core PMU only as PERF_TYPE_RAW. */
    *type = PERF_TYPE_RAW;
    snprintf(name, name_size, "raw-fallback");
    return 0;
}

static int pmu_setup(void) {
    uint32_t type;
    char pmu_name[64];

    if (pmu_find_type(&type, pmu_name, sizeof(pmu_name)) != 0) {
        return -1;
    }

    int opened = 0;
    for (int i = 0; i < PMU_COUNTER_COUNT; i++) {
        struct perf_event_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.type = type;
        attr.size = sizeof(attr);
        attr.config = pmu_counters[i].event;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                           PERF_FORMAT_TOTAL_TIME_RUNNING;

        /* Open events independently so the kernel can multiplex them. */
        int fd = (int)pmu_perf_event_open(&attr, 0, -1, -1, 0);
        if (fd < 0) {
            fprintf(stderr,
                    "PMU: perf_event_open %s/event=0x%llx failed: %s\n",
                    pmu_name, (unsigned long long)pmu_counters[i].event,
                    strerror(errno));
            continue;
        }
        pmu_counters[i].fd = fd;
        opened++;
    }

    if (opened == 0) {
        return -1;
    }

    printf("# PMU device=%s core=%s events=%d/%d window=%s\n",
           pmu_name, pmu_core_name(), opened, PMU_COUNTER_COUNT,
           PMU_WINDOW_NAME);
    return 0;
}

static int pmu_start(void) {
    static int reported = 0;
    int started = 0;
    for (int i = 0; i < PMU_COUNTER_COUNT; i++) {
        if (pmu_counters[i].fd < 0) {
            continue;
        }
        if (ioctl(pmu_counters[i].fd, PERF_EVENT_IOC_RESET, 0) != 0 ||
            ioctl(pmu_counters[i].fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
            fprintf(stderr, "PMU: failed to start %s: %s\n",
                    pmu_counters[i].name, strerror(errno));
            close(pmu_counters[i].fd);
            pmu_counters[i].fd = -1;
            continue;
        }
        started++;
    }
    if (!reported) {
        printf("# PMU started=%d/%d multiplex=enabled\n",
               started, PMU_COUNTER_COUNT);
        reported = 1;
    }
    return started > 0 ? 0 : -1;
}

static void pmu_reset_accumulated(void) {
    for (int i = 0; i < PMU_COUNTER_COUNT; i++) {
        pmu_counters[i].accumulated_value = 0;
        pmu_counters[i].accumulated_time_enabled = 0;
        pmu_counters[i].accumulated_time_running = 0;
    }
}

static void pmu_stop_and_accumulate(void) {
    for (int i = 0; i < PMU_COUNTER_COUNT; i++) {
        if (pmu_counters[i].fd < 0) {
            continue;
        }

        if (ioctl(pmu_counters[i].fd, PERF_EVENT_IOC_DISABLE, 0) != 0) {
            fprintf(stderr, "PMU: failed to stop %s: %s\n",
                    pmu_counters[i].name, strerror(errno));
            continue;
        }

        struct pmu_read_value result;
        if (read(pmu_counters[i].fd, &result, sizeof(result)) !=
            (ssize_t)sizeof(result)) {
            fprintf(stderr, "PMU: failed to read %s: %s\n",
                    pmu_counters[i].name, strerror(errno));
            continue;
        }

        pmu_counters[i].accumulated_value += result.value;
        pmu_counters[i].accumulated_time_enabled += result.time_enabled;
        pmu_counters[i].accumulated_time_running += result.time_running;
    }
}

static void pmu_print_accumulated(uint64_t rounds) {
    for (int i = 0; i < PMU_COUNTER_COUNT; i++) {
        if (pmu_counters[i].fd < 0) {
            continue;
        }

        double scaled = (double)pmu_counters[i].accumulated_value;
        if (pmu_counters[i].accumulated_time_running != 0 &&
            pmu_counters[i].accumulated_time_running <
                pmu_counters[i].accumulated_time_enabled) {
            scaled *=
                (double)pmu_counters[i].accumulated_time_enabled /
                (double)pmu_counters[i].accumulated_time_running;
        }
        printf("# PMU %-24s per_round=%.2f\n",
               pmu_counters[i].name,
               rounds != 0 ? scaled / (double)rounds : 0.0);
    }
}

static void pmu_cleanup(void) {
    for (int i = 0; i < PMU_COUNTER_COUNT; i++) {
        if (pmu_counters[i].fd >= 0) {
            close(pmu_counters[i].fd);
            pmu_counters[i].fd = -1;
        }
    }
}

#else

static int pmu_setup(void) { return -1; }
static int pmu_start(void) { return -1; }
static void pmu_reset_accumulated(void) {}
static void pmu_stop_and_accumulate(void) {}
static void pmu_print_accumulated(uint64_t rounds) { (void)rounds; }
static void pmu_cleanup(void) {}

#endif

#endif
