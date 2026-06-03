#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CACHE_LINE    64
#define TRAIN_STORES  8
#define RESET_STORES  96
#define MAX_AHEAD     4

static volatile uint64_t g_sink = 1;

static inline uint64_t rd_timer(void) {
    uint64_t v;
    asm volatile(
        "isb\n\t"
        "mrs %0, cntvct_el0\n\t"
        "isb\n\t"
        : "=r"(v)
        :
        : "memory");
    return v;
}

static inline uint64_t rd_cntfrq(void) {
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "warning: sched_setaffinity(cpu=%d) failed: %s\n",
                cpu, strerror(errno));
    }
}

static uint64_t xorshift64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static void touch_buffer(uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; i += CACHE_LINE) {
        p[i] = (uint8_t)i;
    }
}

/*
 * AArch64 cache line flush:
 *
 * DC CIVAC = Clean and Invalidate data/unified cache line by VA to PoC.
 *
 * 注意：
 * 1. 有些系统不允许 EL0 用户态执行 dc civac，会 SIGILL。
 * 2. sudo 不能解决 EL0 权限问题，需要内核允许 UCI 或在内核态执行。
 */
static inline void dc_civac_line(void *addr) {
    asm volatile("dc civac, %0" :: "r"(addr) : "memory");
}

static inline void flush_barrier(void) {
    asm volatile("dsb ish\n\tisb\n\t" ::: "memory");
}

/*
 * flush 当前 trial 会涉及的所有 line：
 *
 * base + 0 * stride
 * base + 1 * stride
 * ...
 * base + (TRAIN_STORES + MAX_AHEAD - 1) * stride
 *
 * 其中：
 * 0 .. TRAIN_STORES-1 是训练 store 会写的 line
 * TRAIN_STORES .. TRAIN_STORES+MAX_AHEAD-1 是 future probe 候选 line
 */
static void flush_train_and_future_lines(uint8_t *base,
                                         size_t stride,
                                         int train_n,
                                         int max_ahead) {
    int total = train_n + max_ahead;

    for (int i = 0; i < total; i++) {
        uint8_t *p = base + (size_t)i * stride;
        dc_civac_line(p);
    }

    flush_barrier();
}

/*
 * 只 flush 一个 probe line，用于 calibration。
 */
static void flush_one_line(void *p) {
    dc_civac_line(p);
    flush_barrier();
}

/*
 * 关键点：
 * reset 和 train 都调用这个函数。
 * 因此二者使用同一个静态 store 指令 PC。
 *
 * reset 阶段：addr_list 是随机地址，用于破坏该 PC 的 stride 历史。
 * train 阶段：addr_list 是 base, base+S, base+2S ...，用于训练 store stride。
 */
__attribute__((noinline))
static void issue_store_sequence(uint8_t **addr_list, int n, uint64_t tag) {
    for (int i = 0; i < n; i++) {
        uint8_t *p = addr_list[i];
        uint64_t v = tag + (uint64_t)i * 0x9e3779b97f4a7c15ULL;

        asm volatile(
            "str %x[val], [%[addr]]\n\t"
            :
            : [val] "r"(v), [addr] "r"(p)
            : "memory");
    }

    asm volatile("dmb ish\n\tisb\n\t" ::: "memory");
}

static void build_random_reset_addrs(uint8_t **list,
                                     uint8_t *noise,
                                     size_t noise_size,
                                     uint64_t *rng) {
    size_t lines = noise_size / CACHE_LINE;

    for (int i = 0; i < RESET_STORES; i++) {
        size_t line = xorshift64(rng) % lines;
        list[i] = noise + line * CACHE_LINE;
    }
}

static void build_stride_train_addrs(uint8_t **list,
                                     uint8_t *base,
                                     size_t stride) {
    for (int i = 0; i < TRAIN_STORES; i++) {
        list[i] = base + (size_t)i * stride;
    }
}

static uint64_t timed_one_load(uint8_t *p) {
    uint64_t t0, t1, v;

    asm volatile("dsb ish\n\tisb\n\t" ::: "memory");

    t0 = rd_timer();
    v = *(volatile uint64_t *)p;
    asm volatile("isb\n\t" ::: "memory");
    t1 = rd_timer();

    g_sink ^= v;
    return t1 - t0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t percentile_sorted(uint64_t *v, int n, double p) {
    int idx = (int)((n - 1) * p);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return v[idx];
}

static double hit_rate(uint64_t *v, int n, uint64_t th) {
    int hit = 0;

    for (int i = 0; i < n; i++) {
        if (v[i] <= th) {
            hit++;
        }
    }

    return 100.0 * (double)hit / (double)n;
}

static uint64_t calibrate_hit_threshold(uint8_t *test) {
    const int n = 256;
    uint64_t hot[n];
    uint64_t cold[n];

    uint8_t *p = test + 4096;
    *(volatile uint64_t *)p = 0x12345678ULL;

    /*
     * hot latency
     */
    for (int i = 0; i < n; i++) {
        hot[i] = timed_one_load(p);
    }

    /*
     * cold latency after DC CIVAC
     */
    for (int i = 0; i < n; i++) {
        flush_one_line(p);
        cold[i] = timed_one_load(p);
    }

    qsort(hot, n, sizeof(uint64_t), cmp_u64);
    qsort(cold, n, sizeof(uint64_t), cmp_u64);

    uint64_t hot_med = hot[n / 2];
    uint64_t cold_med = cold[n / 2];

    uint64_t th;
    if (cold_med > hot_med) {
        th = hot_med + (cold_med - hot_med) / 2;
    } else {
        th = hot_med + 1;
    }

    printf("Calibration:\n");
    printf("  hot_med  = %" PRIu64 "\n", hot_med);
    printf("  cold_med = %" PRIu64 "\n", cold_med);
    printf("  hit_threshold <= %" PRIu64 "\n\n", th);

    return th;
}

static void *alloc_aligned_or_die(const char *name, size_t size) {
    void *p = NULL;
    int ret = posix_memalign(&p, 4096, size);

    if (ret != 0) {
        fprintf(stderr, "%s allocation failed: size=%zu MB, error=%s\n",
                name, size / 1024 / 1024, strerror(ret));
        exit(1);
    }

    return p;
}

int main(int argc, char **argv) {
    int cpu = 0;
    int trials = 2000;
    int delay_nop = 0;

    if (argc >= 2) {
        cpu = atoi(argv[1]);
    }

    if (argc >= 3) {
        trials = atoi(argv[2]);
    }

    if (argc >= 4) {
        delay_nop = atoi(argv[3]);
    }

    if (trials < 10) {
        trials = 10;
    }

    pin_cpu(cpu);

    /*
     * 使用 DC CIVAC 后不再需要 128MB eviction buffer。
     * test/noise 也可以比较小。
     */
    size_t test_size  = 16 * 1024 * 1024;
    size_t noise_size = 16 * 1024 * 1024;

    uint8_t *test  = alloc_aligned_or_die("test", test_size);
    uint8_t *noise = alloc_aligned_or_die("noise", noise_size);

    madvise(test, test_size, MADV_HUGEPAGE);
    madvise(noise, noise_size, MADV_HUGEPAGE);

    touch_buffer(test, test_size);
    touch_buffer(noise, noise_size);

    printf("Timer: CNTVCT_EL0, cntfrq=%" PRIu64 " Hz\n", rd_cntfrq());
    printf("CPU=%d, trials=%d, delay_nop=%d\n", cpu, trials, delay_nop);
    printf("TRAIN_STORES=%d, RESET_STORES=%d, MAX_AHEAD=%d\n",
           TRAIN_STORES, RESET_STORES, MAX_AHEAD);
    printf("Flush method: DC CIVAC\n\n");

    uint64_t hit_th = calibrate_hit_threshold(test);

    uint8_t *reset_addrs[RESET_STORES];
    uint8_t *train_addrs[TRAIN_STORES];

    uint64_t *base_lat  = calloc((size_t)trials, sizeof(uint64_t));
    uint64_t *train_lat = calloc((size_t)trials, sizeof(uint64_t));

    if (!base_lat || !train_lat) {
        fprintf(stderr, "latency allocation failed\n");
        return 1;
    }

    /*
     * 这些 stride 保证 train + future probe 尽量留在同一个 4KB page 内。
     */
    const size_t stride_list[] = {
        64, 128, 192, 256, 512
    };

    int n_stride = sizeof(stride_list) / sizeof(stride_list[0]);

    printf("%8s %5s | %10s %10s %10s %8s | %10s %10s %10s %8s | %s\n",
           "strideB", "ahead",
           "base_p10", "base_med", "base_p90", "base_hit",
           "train_p10", "train_med", "train_p90", "train_hit",
           "effect");

    printf("-------------------------------------------------------------------------------------------------------------\n");

    uint64_t rng = 0x123456789abcdef0ULL;
    size_t pages = test_size / 4096;

    for (int si = 0; si < n_stride; si++) {
        size_t stride = stride_list[si];

        for (int ahead = 1; ahead <= MAX_AHEAD; ahead++) {
            size_t max_span =
                64 + (size_t)(TRAIN_STORES + MAX_AHEAD - 1) * stride + 8;

            if (max_span >= 4096) {
                continue;
            }

            for (int t = 0; t < trials; t++) {
                size_t page = xorshift64(&rng) % pages;

                /*
                 * base 选择在 page 内，避免跨 4KB 边界。
                 */
                uint8_t *base = test + page * 4096 + 64;
                uint8_t *probe =
                    base + (size_t)(TRAIN_STORES + ahead - 1) * stride;

                /*
                 * baseline round:
                 *
                 * 1. 用同一个 store PC 发送随机 store 序列，重置/破坏 stride 状态
                 * 2. 使用 DC CIVAC flush 当前 trial 的 train + future lines
                 * 3. 不训练
                 * 4. 只 probe 一个位置
                 */
                build_random_reset_addrs(reset_addrs, noise, noise_size, &rng);
                issue_store_sequence(reset_addrs, RESET_STORES, rng);

                flush_train_and_future_lines(base,
                                             stride,
                                             TRAIN_STORES,
                                             MAX_AHEAD);

                for (int k = 0; k < delay_nop; k++) {
                    asm volatile("" ::: "memory");
                }

                base_lat[t] = timed_one_load(probe);

                /*
                 * trained round:
                 *
                 * 1. 每轮前再次 reset 同一个 store PC
                 * 2. flush 当前 trial 的 train + future lines
                 * 3. 用固定 stride store 序列训练
                 * 4. 只 probe 一个 future 位置
                 */
                build_random_reset_addrs(reset_addrs, noise, noise_size, &rng);
                issue_store_sequence(reset_addrs, RESET_STORES, rng);

                flush_train_and_future_lines(base,
                                             stride,
                                             TRAIN_STORES,
                                             MAX_AHEAD);

                build_stride_train_addrs(train_addrs, base, stride);
                issue_store_sequence(train_addrs, TRAIN_STORES, rng);

                for (int k = 0; k < delay_nop; k++) {
                    asm volatile("" ::: "memory");
                }

                train_lat[t] = timed_one_load(probe);
            }

            qsort(base_lat, trials, sizeof(uint64_t), cmp_u64);
            qsort(train_lat, trials, sizeof(uint64_t), cmp_u64);

            uint64_t b_p10 = percentile_sorted(base_lat, trials, 0.10);
            uint64_t b_med = percentile_sorted(base_lat, trials, 0.50);
            uint64_t b_p90 = percentile_sorted(base_lat, trials, 0.90);

            uint64_t t_p10 = percentile_sorted(train_lat, trials, 0.10);
            uint64_t t_med = percentile_sorted(train_lat, trials, 0.50);
            uint64_t t_p90 = percentile_sorted(train_lat, trials, 0.90);

            double b_hit = hit_rate(base_lat, trials, hit_th);
            double t_hit = hit_rate(train_lat, trials, hit_th);

            const char *effect = "-";

            if (t_med + 1 < b_med || t_hit > b_hit + 15.0) {
                effect = "prefetch-like";
            }

            printf("%8zu %5d | %10" PRIu64 " %10" PRIu64 " %10" PRIu64 " %7.1f%% | "
                   "%10" PRIu64 " %10" PRIu64 " %10" PRIu64 " %7.1f%% | %s\n",
                   stride,
                   ahead,
                   b_p10,
                   b_med,
                   b_p90,
                   b_hit,
                   t_p10,
                   t_med,
                   t_p90,
                   t_hit,
                   effect);
        }

        printf("\n");
    }

    fprintf(stderr, "sink=%" PRIu64 "\n", g_sink);

    free(base_lat);
    free(train_lat);
    free(test);
    free(noise);

    return 0;
}