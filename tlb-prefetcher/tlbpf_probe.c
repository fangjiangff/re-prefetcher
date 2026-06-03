// tlbpf_probe.c
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef USE_PTEDIT
#include "ptedit_header.h"
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define TRAIN_LEN 3

enum train_mode {
    MODE_NOP = 0,
    MODE_LOAD,
    MODE_PRFM,
    MODE_RANDOM_PRFM
};

struct counter {
    const char *name;
    int fd;
    uint64_t sum;
};

static volatile uint8_t global_sink;

static long perf_event_open(struct perf_event_attr *hw_event,
                            pid_t pid, int cpu, int group_fd,
                            unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static inline void isb(void) {
    asm volatile("isb" ::: "memory");
}

static inline void dsb_ish(void) {
    asm volatile("dsb ish" ::: "memory");
}

static inline uint64_t read_cntvct(void) {
    uint64_t v;
    asm volatile("isb; mrs %0, cntvct_el0; isb" : "=r"(v) :: "memory");
    return v;
}

static inline void do_prfm(const void *p) {
    asm volatile("prfm pldl1keep, [%0]" :: "r"(p) : "memory");
}

static inline uint8_t do_load8(const void *p) {
    uint8_t v;
    asm volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static void small_wait(int n) {
    for (int i = 0; i < n; i++) {
        asm volatile("nop" ::: "memory");
    }
}

static void pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        exit(1);
    }
}

static uint64_t read_uint_file(const char *path) {
    char buf[128];

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        fprintf(stderr, "failed to read %s\n", path);
        exit(1);
    }

    buf[n] = 0;
    return strtoull(buf, NULL, 0);
}

static uint64_t read_event_code(const char *path) {
    char buf[128];

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        fprintf(stderr, "failed to read %s\n", path);
        exit(1);
    }

    buf[n] = 0;

    // event files normally look like: event=0x34
    char *p = strstr(buf, "event=");
    if (!p) {
        fprintf(stderr, "unexpected event format in %s: %s\n", path, buf);
        exit(1);
    }

    p += strlen("event=");
    return strtoull(p, NULL, 0);
}

static int open_named_pmu_event(const char *pmu, const char *event) {
    char path[256];

    snprintf(path, sizeof(path),
             "/sys/bus/event_source/devices/%s/type", pmu);
    uint64_t type = read_uint_file(path);

    snprintf(path, sizeof(path),
             "/sys/bus/event_source/devices/%s/events/%s", pmu, event);
    uint64_t config = read_event_code(path);

    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));

    pe.type = type;
    pe.size = sizeof(pe);
    pe.config = config;
    pe.disabled = 1;

    // We want user-space activity only.
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.exclude_idle = 0;

    int fd = perf_event_open(&pe, 0, -1, -1, 0);
    if (fd < 0) {
        fprintf(stderr, "warning: perf_event_open %s/%s failed: %s\n",
                pmu, event, strerror(errno));
    }

    return fd;
}

static void counters_reset_enable(struct counter *cs, int n) {
    for (int i = 0; i < n; i++) {
        if (cs[i].fd >= 0) {
            ioctl(cs[i].fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(cs[i].fd, PERF_EVENT_IOC_ENABLE, 0);
        }
    }
}

static void counters_disable_read_accumulate(struct counter *cs, int n) {
    for (int i = 0; i < n; i++) {
        if (cs[i].fd >= 0) {
            ioctl(cs[i].fd, PERF_EVENT_IOC_DISABLE, 0);

            uint64_t val = 0;
            if (read(cs[i].fd, &val, sizeof(val)) == sizeof(val)) {
                cs[i].sum += val;
            }
        }
    }
}

static enum train_mode parse_mode(const char *s) {
    if (!strcmp(s, "nop")) return MODE_NOP;
    if (!strcmp(s, "load")) return MODE_LOAD;
    if (!strcmp(s, "prfm")) return MODE_PRFM;
    if (!strcmp(s, "random-prfm")) return MODE_RANDOM_PRFM;

    fprintf(stderr, "unknown mode: %s\n", s);
    exit(1);
}

static size_t parse_stride(const char *s, size_t page_size) {
    if (!strcmp(s, "8k")) {
        return 8UL * 1024;
    }

    if (!strcmp(s, "page")) {
        return page_size;
    }

    if (!strcmp(s, "2page")) {
        return 2 * page_size;
    }

    if (!strcmp(s, "4page")) {
        return 4 * page_size;
    }

    if (!strcmp(s, "2m")) {
        return 2UL * 1024 * 1024;
    }

    if (!strcmp(s, "32m")) {
        return 32UL * 1024 * 1024;
    }

    if (!strcmp(s, "table")) {
        // For AArch64 granule G, one translation table has G/8 entries.
        // Lowest-level table coverage = (G/8) * G = G^2 / 8.
        // 4KB  -> 2MB
        // 16KB -> 32MB
        // 64KB -> 512MB
        return (page_size / 8) * page_size;
    }

    if (!strcmp(s, "2table")) {
        return 2 * (page_size / 8) * page_size;
    }

    // Raw byte value, e.g., --stride 65536
    return strtoull(s, NULL, 0);
}

static void invalidate_tlb_one(void *p) {
#ifdef USE_PTEDIT
    ptedit_invalidate_tlb(p);
#else
    (void)p;
#endif
}

static void fallback_tlb_evict(uint8_t *evict_buf, size_t page_size, size_t pages) {
#ifndef USE_PTEDIT
    volatile uint8_t s = 0;
    for (size_t i = 0; i < pages; i++) {
        s ^= evict_buf[i * page_size];
    }
    global_sink ^= s;
#else
    (void)evict_buf;
    (void)page_size;
    (void)pages;
#endif
}

static void train(enum train_mode mode,
                  uint8_t **seq,
                  uint8_t **rnd,
                  int wait_nops) {
    if (mode == MODE_NOP) {
        for (int i = 0; i < TRAIN_LEN; i++) {
            asm volatile("nop; nop; nop; nop" ::: "memory");
        }
    } else if (mode == MODE_LOAD) {
        volatile uint8_t s = 0;
        for (int i = 0; i < TRAIN_LEN; i++) {
            s ^= do_load8(seq[i]);
        }
        global_sink ^= s;
    } else if (mode == MODE_PRFM) {
        for (int i = 0; i < TRAIN_LEN; i++) {
            do_prfm(seq[i]);
        }
    } else if (mode == MODE_RANDOM_PRFM) {
        for (int i = 0; i < TRAIN_LEN; i++) {
            do_prfm(rnd[i]);
        }
    }

    // PRFM is a hint and is not guaranteed to complete immediately.
    // Give the translation-side mechanisms some time.
    small_wait(wait_nops);

    dsb_ish();
    isb();
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  --cpu N              CPU to pin to, default 0\n"
        "  --mode MODE          nop|load|prfm|random-prfm, default prfm\n"
        "  --iters N            iterations, default 10000\n"
        "  --blocks N           sparse blocks, default 256\n"
        "  --stride S           8k|page|2page|4page|2m|32m|table|2table|bytes, default table\n"
        "  --pmu NAME           PMU source name, default armv8_cortex_a76\n"
        "  --wait N             nop wait after training, default 200\n",
        prog);
    exit(1);
}

int main(int argc, char **argv) {
    int cpu = 0;
    enum train_mode mode = MODE_PRFM;
    size_t iters = 10000;
    size_t blocks = 256;
    const char *stride_s = "table";
    const char *pmu = "armv8_cortex_a76";
    int wait_nops = 200;

    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size == 0) {
        fprintf(stderr, "failed to get page size\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
            cpu = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            mode = parse_mode(argv[++i]);
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            iters = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) {
            blocks = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--stride") && i + 1 < argc) {
            stride_s = argv[++i];
        } else if (!strcmp(argv[i], "--pmu") && i + 1 < argc) {
            pmu = argv[++i];
        } else if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            wait_nops = atoi(argv[++i]);
        } else {
            usage(argv[0]);
        }
    }

    size_t stride = parse_stride(stride_s, page_size);

    if (blocks < TRAIN_LEN + 8) {
        fprintf(stderr, "blocks too small\n");
        return 1;
    }

    pin_cpu(cpu);

#ifdef USE_PTEDIT
    if (ptedit_init()) {
        fprintf(stderr, "ptedit_init failed. Is pteditor.ko loaded?\n");
        return 1;
    }
#else
    fprintf(stderr,
            "warning: built without PTEditor. TLB invalidation uses noisy fallback eviction.\n"
            "         For TLB-prefetch experiments, -DUSE_PTEDIT is strongly recommended.\n");
#endif

    size_t region_size = blocks * stride + page_size;

    uint8_t *region = mmap(NULL, region_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                           -1, 0);
    if (region == MAP_FAILED) {
        perror("mmap region");
        return 1;
    }

    madvise(region, region_size, MADV_NOHUGEPAGE);

    // Fallback TLB eviction buffer.
    size_t evict_pages = 32768;
    size_t evict_size = evict_pages * page_size;

    uint8_t *evict_buf = mmap(NULL, evict_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                              -1, 0);
    if (evict_buf == MAP_FAILED) {
        perror("mmap evict_buf");
        return 1;
    }

    madvise(evict_buf, evict_size, MADV_NOHUGEPAGE);

    // Fault in one page per sparse block.
    for (size_t i = 0; i < blocks; i++) {
        region[i * stride] = (uint8_t)i;
    }

    for (size_t i = 0; i < evict_pages; i++) {
        evict_buf[i * page_size] = (uint8_t)i;
    }

    struct counter counters[] = {
        {"cpu_cycles",       -1, 0},
        {"inst_retired",     -1, 0},
        {"dtlb_walk",        -1, 0},
        {"l1d_tlb_refill",   -1, 0},
        {"l2d_tlb_refill",   -1, 0},
        {"l1d_cache_refill", -1, 0},
        {"l2d_cache_refill", -1, 0},
    };

    int nr_counters = sizeof(counters) / sizeof(counters[0]);

    for (int i = 0; i < nr_counters; i++) {
        counters[i].fd = open_named_pmu_event(pmu, counters[i].name);
    }

    uint64_t sum_lat = 0;
    uint64_t min_lat = UINT64_MAX;
    uint64_t max_lat = 0;

    for (size_t it = 0; it < iters; it++) {
        size_t base = (it * 17) % (blocks - TRAIN_LEN - 4);

        uint8_t *seq[TRAIN_LEN];
        uint8_t *rnd[TRAIN_LEN];

        for (int j = 0; j < TRAIN_LEN; j++) {
            seq[j] = region + (base + j) * stride;

            size_t r = (base + 97 + j * 53) % (blocks - 1);
            rnd[j] = region + r * stride;
        }

        uint8_t *target = region + (base + TRAIN_LEN) * stride;

        // Warm target data so the probe is more sensitive to translation latency.
        global_sink ^= do_load8(target);

        // Invalidate target and training translations.
        for (int j = 0; j < TRAIN_LEN; j++) {
            invalidate_tlb_one(seq[j]);
            invalidate_tlb_one(rnd[j]);
        }
        invalidate_tlb_one(target);

        dsb_ish();
        isb();

        // If not using PTEditor, try to disturb the TLB.
        fallback_tlb_evict(evict_buf, page_size, evict_pages);

        // Train.
        train(mode, seq, rnd, wait_nops);

        // Measure only the probe window with PMU.
        counters_reset_enable(counters, nr_counters);

        uint64_t t0 = read_cntvct();
        global_sink ^= do_load8(target);
        uint64_t t1 = read_cntvct();

        counters_disable_read_accumulate(counters, nr_counters);

        uint64_t lat = t1 - t0;
        sum_lat += lat;

        if (lat < min_lat) min_lat = lat;
        if (lat > max_lat) max_lat = lat;
    }

    printf("=== tlbpf_probe result ===\n");
    printf("cpu=%d\n", cpu);
    printf("mode=%d  // 0=nop 1=load 2=prfm 3=random-prfm\n", mode);
    printf("pmu=%s\n", pmu);
    printf("page_size=%zu bytes\n", page_size);
    printf("stride=%zu bytes (%s)\n", stride, stride_s);
    printf("blocks=%zu region_size=%.2f MB\n",
           blocks, (double)region_size / (1024.0 * 1024.0));
    printf("iters=%zu wait_nops=%d\n", iters, wait_nops);
    printf("target_latency_cntvct: avg=%.2f min=%lu max=%lu\n",
           (double)sum_lat / (double)iters, min_lat, max_lat);

    printf("probe-window PMU totals:\n");
    for (int i = 0; i < nr_counters; i++) {
        printf("  %-18s %lu", counters[i].name, counters[i].sum);
        if (iters > 0) {
            printf("  per_iter=%.4f", (double)counters[i].sum / (double)iters);
        }
        printf("\n");
    }

    printf("sink=%u\n", global_sink);

    for (int i = 0; i < nr_counters; i++) {
        if (counters[i].fd >= 0) close(counters[i].fd);
    }

#ifdef USE_PTEDIT
    ptedit_cleanup();
#endif

    munmap(region, region_size);
    munmap(evict_buf, evict_size);

    return 0;
}