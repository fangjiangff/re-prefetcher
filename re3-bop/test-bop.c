#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "This test requires AArch64 DC CIVAC, CPP RCTX, and CNTVCT_EL0."
#endif

#include "../until.h"

#ifndef ENABLE_PMU
#define ENABLE_PMU 1
#endif
#ifndef ENABLE_CACHE_PROBE
#define ENABLE_CACHE_PROBE 1
#endif
#ifndef PMU_CORE_X925
#define PMU_CORE_X925 1
#endif
#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 2000
#endif

#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"

#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define BOP_OFFSET_LINES 16
#define TRAIN_PC_COUNT 16
#define POOL_PAGES 4096
#define DEFAULT_ROUNDS 100
#define DEFAULT_THRESHOLD_NS 80
#define DEFAULT_TRAIN_PAIRS 1024
#define DEFAULT_TRIGGER_BATCH 64
#define PAGE_SEED 0x243f6a8885a308d3ULL

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

enum train_mode {
    TRAIN_NONE,
    TRAIN_FIXED_OFFSET,
};

struct case_stats {
    uint64_t rounds;
    uint64_t probes;
    uint64_t hits;
    uint64_t latency_ns;
    uint64_t l1_events;
    uint64_t l2_events;
};

typedef void (*access_function)(void *);

static uint8_t *mapping;
static size_t mapping_size;
static size_t page_order[POOL_PAGES];
static uint64_t timer_freq_hz;

#if ENABLE_PMU
static int pmu_fd = -1;
static int pmu_ready;
#endif

static inline uint64_t splitmix64_next(uint64_t *state)
{
    uint64_t value;

    *state += 0x9e3779b97f4a7c15ULL;
    value = *state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static inline uint64_t read_cntvct_el0(void)
{
    uint64_t value;

    asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb"
                 : "=r"(value) :: "memory");
    return value;
}

static inline uint64_t read_cntfrq_el0(void)
{
    uint64_t value;

    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

static inline uint64_t ticks_to_ns(uint64_t ticks)
{
    __uint128_t product = (__uint128_t)ticks * 1000000000ULL;

    product += timer_freq_hz / 2;
    return (uint64_t)(product / timer_freq_hz);
}

#define DEFINE_ACCESS(number, reg)                                      \
    __attribute__((noinline)) static void train_pc##number(void *addr)   \
    {                                                                    \
        asm volatile("ldr " #reg ", [%0]"                              \
                     :: "r"(addr) : #reg, "memory");                    \
    }

DEFINE_ACCESS(0, x1)
DEFINE_ACCESS(1, x2)
DEFINE_ACCESS(2, x3)
DEFINE_ACCESS(3, x4)
DEFINE_ACCESS(4, x5)
DEFINE_ACCESS(5, x6)
DEFINE_ACCESS(6, x7)
DEFINE_ACCESS(7, x8)
DEFINE_ACCESS(8, x9)
DEFINE_ACCESS(9, x10)
DEFINE_ACCESS(10, x11)
DEFINE_ACCESS(11, x12)
DEFINE_ACCESS(12, x13)
DEFINE_ACCESS(13, x14)
DEFINE_ACCESS(14, x15)
DEFINE_ACCESS(15, x16)

static access_function const train_pcs[TRAIN_PC_COUNT] = {
    train_pc0, train_pc1, train_pc2, train_pc3,
    train_pc4, train_pc5, train_pc6, train_pc7,
    train_pc8, train_pc9, train_pc10, train_pc11,
    train_pc12, train_pc13, train_pc14, train_pc15,
};

/* No training access uses this load PC. */
__attribute__((noinline)) static void bop_trigger_access(void *addr)
{
    asm volatile("ldr x17, [%0]" :: "r"(addr) : "x17", "memory");
}

static inline __attribute__((always_inline)) void wait_for_prefetch(void)
{
    asm volatile(".rept " STRINGIFY(PREFETCH_WAIT_NOPS) "\n\t"
                 "nop\n\t"
                 ".endr\n\t"
                 ::: "memory");
}

static uint8_t *page_addr(size_t page, size_t line)
{
    return mapping + page * PAGE_SIZE + line * LINE_SIZE;
}

static size_t round_page(size_t round, size_t position)
{
    /* Rotation preserves uniqueness within a round. */
    return page_order[(round * 977u + position) % POOL_PAGES];
}

static void init_page_order(void)
{
    uint64_t rng = PAGE_SEED;

    for (size_t i = 0; i < POOL_PAGES; i++)
        page_order[i] = i;
    for (size_t i = POOL_PAGES - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(&rng) % (i + 1));
        size_t temporary = page_order[i];

        page_order[i] = page_order[j];
        page_order[j] = temporary;
    }
}

static void prefault_mapping(void)
{
    uint64_t rng = PAGE_SEED ^ 0xa4093822299f31d0ULL;

    /* Random page order avoids leaving a sequential page stream behind. */
    for (size_t i = 0; i < POOL_PAGES; i++) {
        size_t page = page_order[i];
        size_t line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);

        *page_addr(page, line) = (uint8_t)i;
    }
    mfence();
}

static size_t noise_count(size_t train_pairs)
{
    return (train_pairs + 6u) / 7u;
}

static void train_bop(enum train_mode mode, size_t round, size_t train_pairs)
{
    uint64_t rng = PAGE_SEED ^ ((uint64_t)round * 0xd1b54a32d192ed03ULL)
                   ^ ((uint64_t)mode << 57);
    size_t next_noise_page = train_pairs;

    if (mode == TRAIN_NONE)
        return;

    for (size_t i = 0; i < train_pairs; i++) {
        size_t page = round_page(round, i);
        size_t base_line = (size_t)(splitmix64_next(&rng) % (LINES_PER_PAGE - BOP_OFFSET_LINES));
        size_t mate_line;
        uint8_t *base = page_addr(page, base_line);
        uint8_t *mate;

        mate_line = base_line + BOP_OFFSET_LINES;
        mate = page_addr(page, mate_line);

        flush(base);
        flush(mate);
        mfence();
        train_pcs[(i * 5u) % TRAIN_PC_COUNT](base);
        nops();
        train_pcs[(i * 7u + 3u) % TRAIN_PC_COUNT](mate);
        nops();

        /* Vary block length so candidate testing cannot phase-lock to bases. */
        if (i % 7u == 3u) {
            size_t noise_page = round_page(round, next_noise_page++);
            size_t noise_line =
                (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);
            uint8_t *noise = page_addr(noise_page, noise_line);

            flush(noise);
            mfence();
            train_pcs[(i * 11u + 1u) % TRAIN_PC_COUNT](noise);
            nops();
        }
    }
}

static void prepare_trigger_batch(size_t round, size_t train_pairs,
                                  size_t trigger_batch, size_t probe_delta,
                                  void **triggers, void **targets)
{
    uint64_t rng = PAGE_SEED ^ 0x13198a2e03707344ULL
                   ^ ((uint64_t)round * 0x94d049bb133111ebULL)
                   ^ probe_delta;
    size_t first_page = train_pairs + noise_count(train_pairs);
    size_t max_delta = BOP_OFFSET_LINES > probe_delta ?
                       BOP_OFFSET_LINES : probe_delta;

    for (size_t i = 0; i < trigger_batch; i++) {
        size_t page = round_page(round, first_page + i);
        size_t line =
            (size_t)(splitmix64_next(&rng) % (LINES_PER_PAGE - max_delta));

        triggers[i] = page_addr(page, line);
        targets[i] = page_addr(page, line + probe_delta);
        flush(triggers[i]);
        flush(targets[i]);
    }
    mfence();
}

static uint64_t timed_reload_ns(void *addr)
{
    uint64_t start;
    uint64_t end;

    mfence();
    start = read_cntvct_el0();
    mLoad_inline(addr);
    end = read_cntvct_el0();
    mfence();
    flush(addr);
    return ticks_to_ns(end - start);
}

#if ENABLE_PMU
static int pmu_setup(void)
{
    pmu_fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
    if (pmu_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n",
                PREFETCH_PMU_DEVICE_PATH, strerror(errno));
        return -1;
    }
    if (ioctl(pmu_fd, PREFETCH_PMU_IOC_REARM) != 0) {
        fprintf(stderr, "PMU rearm ioctl failed: %s\n", strerror(errno));
        close(pmu_fd);
        pmu_fd = -1;
        return -1;
    }
    return 0;
}
#endif

static void run_case(const char *name, enum train_mode mode,
                     size_t probe_delta, size_t rounds,
                     uint64_t threshold_ns, size_t train_pairs,
                     size_t trigger_batch)
{
    struct case_stats stats = {0};
    void **triggers = calloc(trigger_batch, sizeof(*triggers));
    void **targets = calloc(trigger_batch, sizeof(*targets));

    if (triggers == NULL || targets == NULL) {
        fprintf(stderr, "calloc trigger arrays failed\n");
        exit(1);
    }

    for (size_t round = 0; round < rounds; round++) {
#if ENABLE_PMU
        struct prefetch_pmu_snapshot before = {0};
        struct prefetch_pmu_snapshot after = {0};
#endif

        cpp_rctx();
        train_bop(mode, round, train_pairs);
        prepare_trigger_batch(round, train_pairs, trigger_batch, probe_delta,
                              triggers, targets);

#if ENABLE_PMU
        if (pmu_ready)
            before = prefetch_pmu_read();
#endif

        for (size_t i = 0; i < trigger_batch; i++)
            bop_trigger_access(triggers[i]);
        wait_for_prefetch();
        
#if ENABLE_PMU
        if (pmu_ready) {
            after = prefetch_pmu_read();
            stats.l1_events += prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
            stats.l2_events += prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
        }
#endif

#if ENABLE_CACHE_PROBE
        /* A coprime permutation separates trigger order from probe order. */
        for (size_t k = 0; k < trigger_batch; k++) {
            size_t i = (k * 37u + 11u) % trigger_batch;
            uint64_t latency = timed_reload_ns(targets[i]);

            stats.latency_ns += latency;
            stats.probes++;
            if (latency <= threshold_ns)
                stats.hits++;
        }
#endif
        stats.rounds++;
    }

    printf("\n[%s]\n", name);
    printf("# train_mode=%d probe_delta_lines=%zu rounds=%llu\n",
           mode, probe_delta, (unsigned long long)stats.rounds);
#if ENABLE_PMU
    if (pmu_ready) {
        double trigger_denominator =
            (double)stats.rounds * (double)trigger_batch;
        printf("# PMU %-24s total=%llu per_trigger=%.6f\n",
               "l1d_cache_hwprf",
               (unsigned long long)stats.l1_events,
               (double)stats.l1_events / trigger_denominator);
        printf("# PMU %-24s total=%llu per_trigger=%.6f\n",
               "l2d_cache_hwprf",
               (unsigned long long)stats.l2_events,
               (double)stats.l2_events / trigger_denominator);
    }
#endif
#if ENABLE_CACHE_PROBE
    printf("# CACHE probes=%llu hits=%llu hits_per_1000=%llu "
           "avg_latency_ns=%.2f threshold_ns=%llu\n",
           (unsigned long long)stats.probes,
           (unsigned long long)stats.hits,
           (unsigned long long)(stats.probes == 0 ? 0 :
               stats.hits * 1000u / stats.probes),
           stats.probes == 0 ? 0.0 :
               (double)stats.latency_ns / (double)stats.probes,
           (unsigned long long)threshold_ns);
#endif

    free(targets);
    free(triggers);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [rounds threshold_ns train_pairs trigger_batch]\n",
            program);
}

int main(int argc, char **argv)
{
    size_t rounds = DEFAULT_ROUNDS;
    uint64_t threshold_ns = DEFAULT_THRESHOLD_NS;
    size_t train_pairs = DEFAULT_TRAIN_PAIRS;
    size_t trigger_batch = DEFAULT_TRIGGER_BATCH;
    size_t pages_needed;
    int result = 0;

    if (argc != 1 && argc != 5) {
        usage(argv[0]);
        return 1;
    }
    if (argc == 5) {
        rounds = strtoull(argv[1], NULL, 0);
        threshold_ns = strtoull(argv[2], NULL, 0);
        train_pairs = strtoull(argv[3], NULL, 0);
        trigger_batch = strtoull(argv[4], NULL, 0);
    }
    pages_needed = train_pairs + noise_count(train_pairs) + trigger_batch;
    if (rounds == 0 || threshold_ns == 0 || train_pairs == 0 ||
        trigger_batch == 0 || pages_needed > POOL_PAGES ||
        trigger_batch % 2 != 0) {
        usage(argv[0]);
        fprintf(stderr, "pages_needed=%zu, limit=%d; trigger_batch must be even\n",
                pages_needed, POOL_PAGES);
        return 1;
    }

    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }

    init_page_order();
    mapping_size = (size_t)POOL_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    prefault_mapping();
    cpp_rctx();

    printf("# BOP existence and offset-relocation test\n");
    printf("# offset_lines=%d offset_bytes=%d\n",
           BOP_OFFSET_LINES, BOP_OFFSET_LINES * LINE_SIZE);
    printf("# rounds=%zu train_pairs=%zu trigger_batch=%zu pages=%d "
           "threshold_ns=%llu wait_nops=%d train_pcs=%d\n",
           rounds, train_pairs, trigger_batch, POOL_PAGES,
           (unsigned long long)threshold_ns, PREFETCH_WAIT_NOPS,
           TRAIN_PC_COUNT);
    printf("# train pairs and trigger pages are disjoint within each round\n");
    printf("# all triggers complete before any target is probed\n");
#if PMU_CORE_X925
    printf("# PMU core=Cortex-X925 events=0x8154,0x8155\n");
#else
    printf("# PMU core=Cortex-A725 events=0x8154,0x8285\n");
#endif

#if ENABLE_PMU
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        fprintf(stderr, "# PMU unavailable; continuing cache-only\n");
        result = 2;
    }
#endif

    run_case("untrained", TRAIN_NONE, BOP_OFFSET_LINES,
             rounds, threshold_ns, train_pairs, trigger_batch);
    run_case("fixed_offset_expected", TRAIN_FIXED_OFFSET, BOP_OFFSET_LINES,
             rounds, threshold_ns, train_pairs, trigger_batch);

#if ENABLE_PMU
    if (pmu_fd >= 0)
        close(pmu_fd);
#endif
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
