/*
 * cmc_probe.c - timing probe for Arm CMC / temporal prefetching
 *
 * Goal:
 *   Build an irregular, one-cache-line-per-page pointer-chasing cycle so that
 *   ordinary stride/stream/region prefetchers have very little to learn, while
 *   a temporal/correlation prefetcher can learn and replay the repeated miss
 *   sequence.  The program compares:
 *     (1) repeated traversal of one fixed random cycle
 *     (2) a negative control that reshuffles the cycle before every traversal
 *
 * Expected CMC signal:
 *   For a working set much larger than private caches/LLC, repeated passes over
 *   the fixed cycle become noticeably faster than pass 0 and faster than the
 *   reshuffled control.  This is indirect evidence; PMU events are preferable
 *   if your kernel exposes L2/CMC prefetch counters.
 *
 * Build on AArch64 Linux/Android NDK:
 *   gcc -O2 -march=armv8.2-a -fno-tree-vectorize -fno-prefetch-loop-arrays \
 *       cmc_probe.c -o cmc_probe
 *
 * Example:
 *   taskset -c 4 ./cmc_probe -n 32768 -s 4096 -p 8
 *
 * Notes:
 *   - Default spacing is 4096 B: only one accessed line per 4 KiB page.  This
 *     intentionally avoids intra-region footprints and constant strides, but it
 *     may add TLB pressure.  Try -s 2048 or -s 1024 as a sensitivity check.
 *   - Use a performance governor and pin to one Cortex-A78 core when possible.
 *   - Do not compile with software prefetching enabled.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define CACHELINE 64ULL

struct Node {
    struct Node *next;
};

static volatile uintptr_t global_sink;

static inline uint64_t splitmix64_next(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static inline uint64_t now_ticks(void) {
#if defined(__aarch64__)
    uint64_t v;
    asm volatile("isb; mrs %0, cntvct_el0; isb" : "=r"(v) :: "memory");
    return v;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static inline uint64_t ticks_freq(void) {
#if defined(__aarch64__)
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#else
    return 1000000000ULL;
#endif
}

static inline void full_barrier(void) {
#if defined(__aarch64__)
    asm volatile("dsb sy; isb" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

static int pin_cpu(int cpu) {
    if (cpu < 0) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "warning: sched_setaffinity(%d) failed: %s\n", cpu, strerror(errno));
        return -1;
    }
    return 0;
}

static inline struct Node *node_at(char *base, size_t idx, size_t spacing) {
    return (struct Node *)(void *)(base + idx * spacing);
}

static void init_perm(size_t *perm, size_t n) {
    for (size_t i = 0; i < n; i++) perm[i] = i;
}

static void shuffle_perm(size_t *perm, size_t n, uint64_t *rng) {
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(rng) % (i + 1));
        size_t tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }
}

/*
 * Build the pointer cycle corresponding to perm[0] -> perm[1] -> ... -> perm[0].
 * The actual stores to nodes are done in linear node-index order.  This avoids
 * accidentally training a temporal prefetcher during construction.
 */
static struct Node *build_cycle_linear_writes(char *base, size_t n, size_t spacing,
                                             const size_t *perm, size_t *succ) {
    for (size_t pos = 0; pos < n; pos++) {
        size_t src = perm[pos];
        size_t dst = perm[(pos + 1 == n) ? 0 : pos + 1];
        succ[src] = dst;
    }

    for (size_t i = 0; i < n; i++) {
        node_at(base, i, spacing)->next = node_at(base, succ[i], spacing);
    }

    full_barrier();
    return node_at(base, perm[0], spacing);
}

/* Fault in pages and discourage the optimizer from dropping the memory. */
static void touch_nodes(char *base, size_t n, size_t spacing) {
    uintptr_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        struct Node *p = node_at(base, i, spacing);
        p->next = p;
        acc ^= (uintptr_t)p;
    }
    global_sink ^= acc;
    full_barrier();
}

__attribute__((noinline))
static uintptr_t chase(struct Node *start, size_t steps) {
    struct Node *p = start;
    uintptr_t acc = 0;
    for (size_t i = 0; i < steps; i++) {
        p = p->next;
        acc ^= (uintptr_t)p;
        /* Keep p live and maintain a strict load-use dependency. */
        asm volatile("" : "+r"(p), "+r"(acc) :: "memory");
    }
    global_sink ^= acc;
    return acc;
}

static double measure_chase(struct Node *start, size_t steps, uint64_t freq) {
    full_barrier();
    uint64_t t0 = now_ticks();
    chase(start, steps);
    uint64_t t1 = now_ticks();
    full_barrier();
    double sec = (double)(t1 - t0) / (double)freq;
    return sec * 1e9 / (double)steps;
}

static void *alloc_region(size_t bytes, const char *name) {
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    size_t rounded = (bytes + pagesz - 1) & ~(pagesz - 1);
    void *p = NULL;

    p = mmap(NULL, rounded, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap %s %zu bytes failed: %s\n", name, rounded, strerror(errno));
        exit(1);
    }

#ifdef MADV_HUGEPAGE
    /* Helpful when transparent huge pages are enabled; harmless otherwise. */
    madvise(p, rounded, MADV_HUGEPAGE);
#endif

    return p;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-n nodes] [-s spacing_bytes] [-p passes] [-c cpu] [-r seed]\n"
        "Defaults: -n 32768 -s 4096 -p 8 -c -1 -r 1\n"
        "Recommended: working set n*spacing should be much larger than LLC.\n",
        prog);
}

int main(int argc, char **argv) {
    size_t n = 32768;
    size_t spacing = 4096;
    int passes = 8;
    int cpu = -1;
    uint64_t seed = 1;

    int opt;
    while ((opt = getopt(argc, argv, "n:s:p:c:r:h")) != -1) {
        switch (opt) {
        case 'n': n = strtoull(optarg, NULL, 0); break;
        case 's': spacing = strtoull(optarg, NULL, 0); break;
        case 'p': passes = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'r': seed = strtoull(optarg, NULL, 0); break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    // if (n < 1024 || passes < 3) {
    //     fprintf(stderr, "Use at least -n 1024 and -p 3.\n");
    //     return 1;
    // }
    if (spacing < sizeof(struct Node) || (spacing % CACHELINE) != 0) {
        fprintf(stderr, "spacing must be >= %zu and a multiple of %llu.\n",
                sizeof(struct Node), (unsigned long long)CACHELINE);
        return 1;
    }

    pin_cpu(cpu);

    uint64_t freq = ticks_freq();
    size_t bytes = n * spacing;
    printf("cmc_probe: nodes=%zu spacing=%zu working_set=%.1f MiB per list passes=%d timer_freq=%" PRIu64 " Hz\n",
           n, spacing, (double)bytes / (1024.0 * 1024.0), passes, freq);
    printf("pattern: random one-cache-line-per-%zuB pointer cycle; fixed-repeat vs reshuffled-control\n\n", spacing);

    char *fixed_base = (char *)alloc_region(bytes, "fixed list");
    char *ctrl_base  = (char *)alloc_region(bytes, "control list");
    size_t *perm = (size_t *)malloc(n * sizeof(size_t));
    size_t *succ = (size_t *)malloc(n * sizeof(size_t));
    if (!perm || !succ) {
        fprintf(stderr, "malloc perm/succ failed\n");
        return 1;
    }

    touch_nodes(fixed_base, n, spacing);
    touch_nodes(ctrl_base, n, spacing);

    uint64_t rng = seed;
    init_perm(perm, n);
    shuffle_perm(perm, n, &rng);
    struct Node *fixed_start = build_cycle_linear_writes(fixed_base, n, spacing, perm, succ);

    printf("Fixed repeated temporal stream:\n");
    double first = 0.0, warm_sum = 0.0;
    int warm_cnt = 0;
    for (int p = 0; p < passes; p++) {
        double ns = measure_chase(fixed_start, n, freq);
        if (p == 0) first = ns;
        if (p >= passes / 2) { warm_sum += ns; warm_cnt++; }
        printf("  fixed pass %2d: %8.2f ns/load", p, ns);
        if (p > 0) printf("  speedup_vs_pass0=%5.2fx", first / ns);
        printf("\n");
    }
    double fixed_warm = warm_sum / (double)warm_cnt;

    printf("\nReshuffled negative control, one traversal per new random cycle:\n");
    double ctrl_sum = 0.0;
    for (int p = 0; p < passes; p++) {
        init_perm(perm, n);
        shuffle_perm(perm, n, &rng);
        struct Node *ctrl_start = build_cycle_linear_writes(ctrl_base, n, spacing, perm, succ);
        double ns = measure_chase(ctrl_start, n, freq);
        ctrl_sum += ns;
        printf("  ctrl  pass %2d: %8.2f ns/load\n", p, ns);
    }
    double ctrl_avg = ctrl_sum / (double)passes;

    printf("\nSummary:\n");
    printf("  fixed pass0             : %8.2f ns/load\n", first);
    printf("  fixed warm avg last half: %8.2f ns/load\n", fixed_warm);
    printf("  reshuffled control avg  : %8.2f ns/load\n", ctrl_avg);
    printf("  warm speedup vs pass0   : %8.2fx\n", first / fixed_warm);
    printf("  warm speedup vs control : %8.2fx\n", ctrl_avg / fixed_warm);

    if (first / fixed_warm > 1.15 && ctrl_avg / fixed_warm > 1.15) {
        printf("  verdict: strong timing signal consistent with CMC/temporal prefetching.\n");
    } else if (first / fixed_warm > 1.08 || ctrl_avg / fixed_warm > 1.08) {
        printf("  verdict: weak timing signal; rerun with larger -n, different -s, fixed frequency, and isolated core.\n");
    } else {
        printf("  verdict: no clear timing signal. CMC may be disabled/throttled, evicted, not present, or hidden by TLB/DRAM noise.\n");
    }

    printf("sink=%" PRIuPTR "\n", global_sink);
    return 0;
}
