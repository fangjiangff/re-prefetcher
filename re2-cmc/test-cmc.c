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

#ifndef ENABLE_CACHE_PROBE
#define ENABLE_CACHE_PROBE 1
#endif
#ifndef ENABLE_PMU
#define ENABLE_PMU 1
#endif
#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 1000
#endif
#ifndef PMU_CORE_X925
#define PMU_CORE_X925 1
#endif

#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"

#define NODE_COUNT 128
#define TRIGGER_NODE_COUNT (NODE_COUNT - 1)
#define MAPPING_PAGES 256
#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define DEFAULT_ROUNDS 5000
#define DEFAULT_THRESHOLD_NS 120
#define DEFAULT_TRAINING_REPLAYS 8
#define NODE_SEED 0x9e3779b97f4a7c15ULL

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

struct node {
    size_t page;
    size_t line;
};

#if ENABLE_CACHE_PROBE
struct probe_stats {
    uint64_t latency_ns[TRIGGER_NODE_COUNT];
    uint64_t probes[TRIGGER_NODE_COUNT];
    uint64_t hits[TRIGGER_NODE_COUNT];
};
#endif

static struct node nodes[NODE_COUNT];
static uint8_t *mapping;
static size_t mapping_size;
static uint64_t timer_freq_hz;

#if ENABLE_PMU
struct pmu_stats {
    uint64_t samples[TRIGGER_NODE_COUNT];
    uint64_t l1_events[TRIGGER_NODE_COUNT];
    uint64_t l2_events[TRIGGER_NODE_COUNT];
};

static uint64_t pmu_l1_accumulated;
static uint64_t pmu_l2_accumulated;
static struct prefetch_pmu_snapshot pmu_before;
static int pmu_device_fd = -1;
static int pmu_ready;

static int pmu_setup(void)
{
    pmu_device_fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
    if (pmu_device_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n",
                PREFETCH_PMU_DEVICE_PATH, strerror(errno));
        return -1;
    }
#if PMU_CORE_X925
    printf("# PMU source=direct-mrs core=Cortex-X925 "
           "counter4=0x8154 counter5=0x8155\n");
#else
    printf("# PMU source=direct-mrs core=Cortex-A725 "
           "counter4=0x8154 counter5=0x8285\n");
#endif
    if (ioctl(pmu_device_fd, PREFETCH_PMU_IOC_REARM) != 0) {
        fprintf(stderr, "PMU rearm ioctl failed: %s\n", strerror(errno));
        close(pmu_device_fd);
        pmu_device_fd = -1;
        return -1;
    }
    return 0;
}

static void pmu_reset(void)
{
    pmu_l1_accumulated = 0;
    pmu_l2_accumulated = 0;
}

static void pmu_start(void)
{
    if (pmu_ready)
        pmu_before = prefetch_pmu_read();
}

static void pmu_stop(struct pmu_stats *stats, size_t node_index)
{
    if (pmu_ready) {
        struct prefetch_pmu_snapshot after = prefetch_pmu_read();
        uint64_t l1_delta =
            prefetch_pmu_delta(after.l1_hwprf, pmu_before.l1_hwprf);
        uint64_t l2_delta =
            prefetch_pmu_delta(after.l2_prefetch, pmu_before.l2_prefetch);

        pmu_l1_accumulated += l1_delta;
        pmu_l2_accumulated += l2_delta;
        stats->samples[node_index]++;
        stats->l1_events[node_index] += l1_delta;
        stats->l2_events[node_index] += l2_delta;
    }
}

static void pmu_print(uint64_t rounds)
{
    double denominator = rounds == 0 ? 1.0 : (double)rounds;

    if (!pmu_ready)
        return;
    printf("# PMU %-24s per_round=%.4f\n", "l1d_cache_hwprf",
           (double)pmu_l1_accumulated / denominator);
    printf("# PMU %-24s per_round=%.4f\n", "l2d_cache_hwprf",
           (double)pmu_l2_accumulated / denominator);
}

static void pmu_print_nodes(const struct pmu_stats *stats)
{
    printf("# node\tpmu_samples\tl1_events\tavg_l1_pmu\t"
           "l2_events\tavg_l2_pmu\n");
    for (size_t node_index = 0; node_index < TRIGGER_NODE_COUNT;
         node_index++) {
        uint64_t samples = stats->samples[node_index];
        double avg_l1_pmu = samples == 0 ? 0.0 :
            (double)stats->l1_events[node_index] / (double)samples;
        double avg_l2_pmu = samples == 0 ? 0.0 :
            (double)stats->l2_events[node_index] / (double)samples;

        printf("%3zu\t%5llu\t\t%5llu\t\t%10.3f\t%5llu\t\t%10.3f\n",
               node_index,
               (unsigned long long)samples,
               (unsigned long long)stats->l1_events[node_index], avg_l1_pmu,
               (unsigned long long)stats->l2_events[node_index], avg_l2_pmu);
    }
}

static void pmu_cleanup(void)
{
    if (pmu_device_fd >= 0)
        close(pmu_device_fd);
}
#else
static inline void pmu_reset(void) {}
static inline void pmu_start(void) {}
static inline void pmu_stop(void *stats, size_t node_index)
{
    (void)stats;
    (void)node_index;
}
static inline void pmu_print(uint64_t rounds) { (void)rounds; }
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

static inline __attribute__((always_inline)) void wait_for_prefetch(void)
{
    /* Keep the PMU window free of deliberate data accesses. */
    asm volatile(".rept " STRINGIFY(PREFETCH_WAIT_NOPS) "\n\t"
                 "nop\n\t"
                 ".endr\n\t"
                 ::: "memory");
}

/* Training and treatment trigger use this exact same load PC. */
__attribute__((noinline)) static void cmc_access(void *addr)
{
    asm volatile("ldr x9, [%0]" :: "r"(addr) : "x9", "memory");
}

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
        nodes[i].line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);
    }
}

static void flush_nodes(void)
{
    for (size_t i = 0; i < NODE_COUNT; i++)
        flush(node_addr(i));
    mfence();
}

static void train_sequence(int training_replays)
{
    for (int replay = 0; replay < training_replays; replay++) {
        flush_nodes();
        for (size_t i = 0; i < NODE_COUNT; i++) {
            cmc_access(node_addr(i));
            /* Preserve the temporal order without storing next pointers. */
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

static void record_probe(struct probe_stats *stats, size_t node_index,
                         void *target, uint64_t threshold_ns)
{
    uint64_t latency = timed_reload_ns(target);

    stats->latency_ns[node_index] += latency;
    stats->probes[node_index]++;
    if (latency <= threshold_ns)
        stats->hits[node_index]++;
}

static void print_probes(const struct probe_stats *stats)
{
    printf("# node\ttrigger_page\ttarget_page\ttarget_line\tprobes\thits\t"
           "per_1000\tavg_latency_ns\n");
    for (size_t node_index = 0; node_index < TRIGGER_NODE_COUNT;
         node_index++) {
        uint64_t probes = stats->probes[node_index];
        uint64_t average = probes == 0 ? 0 :
            stats->latency_ns[node_index] / probes;
        uint64_t per_1000 = probes == 0 ? 0 :
            stats->hits[node_index] * 1000 / probes;

        printf("%3zu\t%3zu\t\t%3zu\t\t%2zu\t\t%5llu\t%5llu\t%4llu\t\t%5llu\n",
               node_index, nodes[node_index].page,
               nodes[node_index + 1].page, nodes[node_index + 1].line,
               (unsigned long long)probes,
               (unsigned long long)stats->hits[node_index],
               (unsigned long long)per_1000,
               (unsigned long long)average);
    }
}
#endif

static void run_case(const char *name, int trained, int rounds,
                     uint64_t threshold_ns, int training_replays)
{
#if ENABLE_PMU
    struct pmu_stats pmu_stats = {0};
#endif
#if ENABLE_CACHE_PROBE
    struct probe_stats stats = {0};
#else
    (void)threshold_ns;
#endif
    pmu_reset();

    for (int round = 0; round < rounds; round++) {
        size_t node_index = (size_t)round % TRIGGER_NODE_COUNT;
        void *trigger = node_addr(node_index);
        void *target = node_addr(node_index + 1);

        /* Every sample starts from empty context-prefetcher state. */
        cpp_rctx();
        if (trained)
            train_sequence(training_replays);

        /* Remove trained data while retaining the learned CMC relation. */
        flush_nodes();

        pmu_start();
        cmc_access(trigger);
        // mfence();
        wait_for_prefetch();
#if ENABLE_PMU
        pmu_stop(&pmu_stats, node_index);
#else
        pmu_stop(NULL, node_index);
#endif

#if ENABLE_CACHE_PROBE
        record_probe(&stats, node_index, target, threshold_ns);
#else
        (void)target;
#endif
    }

    printf("\n[%s]\n", name);
    printf("# trained=%d trigger=node[i] target=node[i+1]\n", trained);
    pmu_print((uint64_t)rounds);
#if ENABLE_PMU
    pmu_print_nodes(&pmu_stats);
#endif
#if ENABLE_CACHE_PROBE
    print_probes(&stats);
#endif
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [rounds threshold_ns training_replays]\n",
            program);
}

int main(int argc, char **argv)
{
    int rounds = DEFAULT_ROUNDS;
    uint64_t threshold_ns = DEFAULT_THRESHOLD_NS;
    int training_replays = DEFAULT_TRAINING_REPLAYS;
    int result = 0;

    if (argc != 1 && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 4) {
        rounds = atoi(argv[1]);
        threshold_ns = strtoull(argv[2], NULL, 0);
        training_replays = atoi(argv[3]);
    }
    if (rounds <= 0 || threshold_ns == 0 || training_replays <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    init_nodes();
    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }

    mapping_size = (size_t)MAPPING_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }

    /* Contents deliberately contain no next-node pointers. */
    memset(mapping, 0xa5, mapping_size);
    for (size_t offset = 0; offset < mapping_size; offset += LINE_SIZE)
        mLoad_inline(mapping + offset);
    flush_nodes();

    printf("# basic CMC existence test\n");
    printf("# rounds=%d training_replays=%d nodes=%d trigger_nodes=%d "
           "threshold_ns=%llu wait_nops=%d cpp_rctx=each_round\n",
           rounds, training_replays, NODE_COUNT, TRIGGER_NODE_COUNT,
           (unsigned long long)threshold_ns, PREFETCH_WAIT_NOPS);
    printf("# timer=cntvct_el0 cntfrq_el0=%lluHz "
           "enable_pmu=%d enable_cache_probe=%d\n",
           (unsigned long long)timer_freq_hz,
           ENABLE_PMU, ENABLE_CACHE_PROBE);
    printf("# PMU window contains only trigger load plus NOP wait\n");

#if ENABLE_PMU
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        fprintf(stderr, "# direct PMU counter setup failed\n");
        result = 2;
    }
#else
    printf("# PMU disabled by ENABLE_PMU=0\n");
#endif

    run_case("untrained", 0, rounds, threshold_ns, training_replays);
    run_case("trained_A_to_B", 1, rounds, threshold_ns, training_replays);

#if ENABLE_PMU
    pmu_cleanup();
#endif
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
