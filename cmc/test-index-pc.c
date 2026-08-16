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
#error "This test requires AArch64"
#endif

#include "../store-stride-prefetcher/until.h"

#ifndef ENABLE_CACHE_PROBE
#define ENABLE_CACHE_PROBE 1
#endif
#ifndef ENABLE_PMU
#define ENABLE_PMU 1
#endif

#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"

#define NODE_COUNT 64
#define MAPPING_PAGES 256
#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define NODE_SEED 0x9e3779b97f4a7c15ULL
#define DEFAULT_ROUNDS 500
#define DEFAULT_THRESHOLD_NS 96
#define DEFAULT_TRAINING_REPLAYS 8
#define DEFAULT_WAIT_NOPS 1000

enum train_pc_mode {
    PC_SAME_AS_TRIGGER = 0,
    PC_SINGLE_OTHER = 1,
    PC_ALTERNATE_2 = 2,
    PC_ROUND_ROBIN_8 = 3,
    PC_ROTATE_EACH_REPLAY = 4,
};

struct node {
    size_t page;
    size_t line;
};

struct stats {
    uint64_t samples;
    uint64_t hits;
    uint64_t latency_ns;
    uint64_t l1_events;
    uint64_t l2_events;
};

typedef void (*access_function)(void *);

static struct node nodes[NODE_COUNT];
static uint8_t *mapping;
static size_t mapping_size;
static uint64_t timer_freq_hz;

#if ENABLE_PMU
static int pmu_fd = -1;
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

static inline void wait_for_prefetch(unsigned int wait_nops)
{
    for (unsigned int i = 0; i < wait_nops; i++)
        asm volatile("nop" ::: "memory");
}

#define DEFINE_ACCESS(number, reg)                               \
    __attribute__((noinline)) static void access_pc##number(void *addr) \
    {                                                             \
        asm volatile("ldr " #reg ", [%0]"                       \
                     :: "r"(addr) : #reg, "memory");             \
    }

/* PC0 is the trigger PC. PC1-PC8 are independent training PCs. */
DEFINE_ACCESS(0, x9)
DEFINE_ACCESS(1, x10)
DEFINE_ACCESS(2, x11)
DEFINE_ACCESS(3, x12)
DEFINE_ACCESS(4, x13)
DEFINE_ACCESS(5, x14)
DEFINE_ACCESS(6, x15)
DEFINE_ACCESS(7, x16)
DEFINE_ACCESS(8, x17)

static access_function const access_pcs[9] = {
    access_pc0, access_pc1, access_pc2, access_pc3, access_pc4,
    access_pc5, access_pc6, access_pc7, access_pc8,
};

static uint8_t *node_addr(size_t index)
{
    return mapping + nodes[index].page * PAGE_SIZE +
           nodes[index].line * LINE_SIZE;
}

static void init_nodes(void)
{
    uint64_t rng = NODE_SEED;
    size_t pages[MAPPING_PAGES];

    for (size_t i = 0; i < MAPPING_PAGES; i++)
        pages[i] = i;
    for (size_t i = MAPPING_PAGES - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(&rng) % (i + 1));
        size_t temporary = pages[i];

        pages[i] = pages[j];
        pages[j] = temporary;
    }
    for (size_t i = 0; i < NODE_COUNT; i++) {
        nodes[i].page = pages[i];
        nodes[i].line =
            (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);
    }
}

static void flush_nodes(void)
{
    for (size_t i = 0; i < NODE_COUNT; i++)
        flush(node_addr(i));
    mfence();
}

static size_t training_pc_index(int mode, size_t position, int replay)
{
    switch (mode) {
    case PC_SAME_AS_TRIGGER:
        return 0;
    case PC_SINGLE_OTHER:
        return 1;
    case PC_ALTERNATE_2:
        return 1 + position % 2;
    case PC_ROUND_ROBIN_8:
        return 1 + position % 8;
    case PC_ROTATE_EACH_REPLAY:
        return 1 + (position + (size_t)replay) % 8;
    default:
        abort();
    }
}

static void train_sequence(int training_replays, int pc_mode)
{
    for (int replay = 0; replay < training_replays; replay++) {
        flush_nodes();
        for (size_t position = 0; position < NODE_COUNT; position++) {
            size_t pc = training_pc_index(pc_mode, position, replay);

            access_pcs[pc](node_addr(position));
            /* Keep consecutive misses inside the CMC correlation window. */
            nops();
        }
    }
}

#if ENABLE_CACHE_PROBE
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
#endif

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

static void run_case(struct stats *stats, int rounds, uint64_t threshold_ns,
                     int training_replays, int pc_mode,
                     unsigned int wait_nops)
{
    for (int round = 0; round < rounds; round++) {
#if ENABLE_PMU
        struct prefetch_pmu_snapshot before;
        struct prefetch_pmu_snapshot after;
#endif

        /* Every sample starts from an empty prefetch context. */
        cpp_rctx();
        if (training_replays > 0)
            train_sequence(training_replays, pc_mode);
        flush_nodes();

#if ENABLE_PMU
        before = prefetch_pmu_read();
#endif
        /* PC0 is used for every treatment trigger. */
        access_pc0(node_addr(0));
        wait_for_prefetch(wait_nops);
#if ENABLE_PMU
        after = prefetch_pmu_read();
        stats->l1_events +=
            prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
        stats->l2_events +=
            prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
#endif

#if ENABLE_CACHE_PROBE
        {
            uint64_t latency = timed_reload_ns(node_addr(1));

            stats->latency_ns += latency;
            if (latency <= threshold_ns)
                stats->hits++;
        }
#else
        (void)threshold_ns;
#endif
        stats->samples++;
    }
}

static const char *mode_name(int mode)
{
    static const char *const names[] = {
        "same_pc",
        "single_other_pc",
        "alternate_2_pc",
        "round_robin_8_pc",
        "rotate_pc_each_replay",
    };

    return names[mode];
}

static int distinct_pc_count(int mode)
{
    static const int counts[] = {1, 1, 2, 8, 8};

    return counts[mode];
}

static void print_row(const char *name, int pc_mode, int training_replays,
                      const struct stats *stats)
{
    double samples = stats->samples == 0 ? 1.0 : (double)stats->samples;
    double hit_per_1000 =
        (double)stats->hits * 1000.0 / samples;
    double average_latency = (double)stats->latency_ns / samples;
    double average_l1 = (double)stats->l1_events / samples;
    double average_l2 = (double)stats->l2_events / samples;

    printf("%-22s\t%d\t%d\t%d\t%llu\t%llu\t%.3f\t%.3f\t"
           "%llu\t%.4f\t%llu\t%.4f\n",
           name, pc_mode, pc_mode < 0 ? 0 : distinct_pc_count(pc_mode),
           training_replays,
           (unsigned long long)stats->samples,
           (unsigned long long)stats->hits,
           hit_per_1000, average_latency,
           (unsigned long long)stats->l1_events, average_l1,
           (unsigned long long)stats->l2_events, average_l2);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [rounds threshold_ns training_replays wait_nops]\n",
            program);
}

int main(int argc, char **argv)
{
    int rounds = DEFAULT_ROUNDS;
    uint64_t threshold_ns = DEFAULT_THRESHOLD_NS;
    int training_replays = DEFAULT_TRAINING_REPLAYS;
    unsigned int wait_nops = DEFAULT_WAIT_NOPS;
    struct stats stats;

    if (argc != 1 && argc != 5) {
        usage(argv[0]);
        return 1;
    }
    if (argc == 5) {
        rounds = atoi(argv[1]);
        threshold_ns = strtoull(argv[2], NULL, 0);
        training_replays = atoi(argv[3]);
        wait_nops = (unsigned int)strtoul(argv[4], NULL, 0);
    }
    if (rounds <= 0 || threshold_ns == 0 || training_replays <= 0) {
        usage(argv[0]);
        return 1;
    }

    init_nodes();
    timer_freq_hz = read_cntfrq_el0();
    mapping_size = (size_t)MAPPING_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    memset(mapping, 0xa5, mapping_size);
    for (size_t offset = 0; offset < mapping_size; offset += LINE_SIZE)
        mLoad_inline(mapping + offset);
    flush_nodes();

#if ENABLE_PMU
    if (pmu_setup() != 0)
        return 2;
#endif

    printf("# A725 CMC load-PC indexing experiment\n");
    printf("# train=node0..node63 trigger=node0@PC0 probe=node1 "
           "cpp_rctx=each_round\n");
    printf("# rounds=%d threshold_ns=%llu training_replays=%d wait_nops=%u "
           "enable_pmu=%d enable_cache_probe=%d\n",
           rounds, (unsigned long long)threshold_ns, training_replays,
           wait_nops, ENABLE_PMU, ENABLE_CACHE_PROBE);
    printf("case\tpc_mode\tdistinct_train_pcs\ttraining_replays\tsamples\t"
           "hits\thit_per_1000\tavg_latency_ns\tl1_events\tavg_l1_pmu\t"
           "l2_events\tavg_l2_pmu\n");

    memset(&stats, 0, sizeof(stats));
    run_case(&stats, rounds, threshold_ns, 0, PC_SAME_AS_TRIGGER,
             wait_nops);
    print_row("untrained", -1, 0, &stats);

    for (int mode = PC_SAME_AS_TRIGGER;
         mode <= PC_ROTATE_EACH_REPLAY; mode++) {
        memset(&stats, 0, sizeof(stats));
        run_case(&stats, rounds, threshold_ns, training_replays, mode,
                 wait_nops);
        print_row(mode_name(mode), mode, training_replays, &stats);
    }

#if ENABLE_PMU
    close(pmu_fd);
#endif
    munmap(mapping, mapping_size);
    return 0;
}
