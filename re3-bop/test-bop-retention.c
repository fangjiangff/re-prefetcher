#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
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
#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 2000
#endif

#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"

#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define TRAIN_PC_COUNT 16
#define POOL_PAGES 32768
#define DEFAULT_REPEATS 10
#define DEFAULT_THRESHOLD_NS 80
#define DEFAULT_TRAIN_PAIRS 3000
#define DEFAULT_DECAY_TRIGGERS 0
#define DEFAULT_PROBE_BATCH 64
#define DEFAULT_OFFSET_LINES 16
#define DEFAULT_CPU 6
#define PAGE_SEED 0x243f6a8885a308d3ULL

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

struct options {
    size_t repeats;
    uint64_t threshold_ns;
    size_t train_pairs;
    size_t decay_triggers;
    size_t probe_batch;
    size_t offset_lines;
    size_t cpu;
    int describe;
};

struct retention_stats {
    uint64_t probes;
    uint64_t cache_hits;
    uint64_t latency_ns;
    uint64_t l1_events;
    uint64_t l2_events;
};

typedef void (*access_function)(void *);

static uint8_t *mapping;
static size_t page_order[POOL_PAGES];
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

/* Training never uses this load PC. */
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

static size_t noise_count(size_t train_pairs)
{
    return train_pairs / 7u + (train_pairs % 7u != 0);
}

static size_t round_page(size_t repeat, size_t position)
{
    return page_order[(repeat * 977u + position) % POOL_PAGES];
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
    uint64_t rng = PAGE_SEED ^ 0xa4093822299b31d0ULL;

    for (size_t i = 0; i < POOL_PAGES; i++) {
        size_t page = page_order[i];
        size_t line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);

        *page_addr(page, line) = (uint8_t)i;
    }
    mfence();
}

static void train_bop(size_t repeat, size_t train_pairs,
                      size_t offset_lines)
{
    uint64_t rng = PAGE_SEED ^ ((uint64_t)repeat * 0xd1b54a32d192ed03ULL);
    size_t base_line_count = LINES_PER_PAGE - offset_lines;
    size_t next_noise_slot = train_pairs;

    for (size_t i = 0; i < train_pairs; i++) {
        size_t page = round_page(repeat, i);
        size_t base_line =
            (size_t)(splitmix64_next(&rng) % base_line_count);
        uint8_t *base = page_addr(page, base_line);
        uint8_t *mate = page_addr(page, base_line + offset_lines);

        flush(base);
        flush(mate);
        mfence();
        train_pcs[(i * 5u) % TRAIN_PC_COUNT](base);
        nops();
        train_pcs[(i * 7u + 3u) % TRAIN_PC_COUNT](mate);
        nops();

        if (i % 7u == 3u) {
            size_t noise_page = round_page(repeat, next_noise_slot++);
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

static void run_decay_stream(size_t repeat, size_t first_slot,
                             size_t decay_triggers, size_t offset_lines)
{
    uint64_t rng = PAGE_SEED ^ 0x13198a2e03707344ULL
                   ^ ((uint64_t)repeat * 0x94d049bb133111ebULL);
    size_t base_line_count = LINES_PER_PAGE - offset_lines;

    for (size_t i = 0; i < decay_triggers; i++) {
        size_t page = round_page(repeat, first_slot + i);
        size_t line = (size_t)(splitmix64_next(&rng) % base_line_count);
        uint8_t *trigger = page_addr(page, line);
        uint8_t *predicted = page_addr(page, line + offset_lines);

        flush(trigger);
        flush(predicted);
        mfence();
        bop_trigger_access(trigger);
        nops();
        /* Deliberately never access predicted. */
    }
    wait_for_prefetch();
    mfence();
}

static void prepare_endpoint(size_t repeat, size_t first_slot,
                             size_t probe_batch, size_t offset_lines,
                             void **triggers, void **targets)
{
    uint64_t rng = PAGE_SEED ^ 0x3c6ef372fe94f82bULL
                   ^ ((uint64_t)repeat * 0xbf58476d1ce4e5b9ULL);
    size_t base_line_count = LINES_PER_PAGE - offset_lines;

    for (size_t i = 0; i < probe_batch; i++) {
        size_t page = round_page(repeat, first_slot + i);
        size_t line = (size_t)(splitmix64_next(&rng) % base_line_count);

        triggers[i] = page_addr(page, line);
        targets[i] = page_addr(page, line + offset_lines);
        flush(triggers[i]);
        flush(targets[i]);
    }
    mfence();
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

static void run_experiment(const struct options *options,
                           struct retention_stats *stats)
{
    void **triggers = calloc(options->probe_batch, sizeof(*triggers));
    void **targets = calloc(options->probe_batch, sizeof(*targets));
    size_t first_decay = options->train_pairs + noise_count(options->train_pairs);
    size_t first_endpoint = first_decay + options->decay_triggers;

    if (triggers == NULL || targets == NULL) {
        fprintf(stderr, "calloc endpoint arrays failed\n");
        exit(1);
    }

    for (size_t repeat = 0; repeat < options->repeats; repeat++) {
#if ENABLE_PMU
        struct prefetch_pmu_snapshot before;
        struct prefetch_pmu_snapshot after;
#endif

        cpp_rctx();
        train_bop(repeat, options->train_pairs, options->offset_lines);
        run_decay_stream(repeat, first_decay, options->decay_triggers,
                         options->offset_lines);
        prepare_endpoint(repeat, first_endpoint, options->probe_batch,
                         options->offset_lines, triggers, targets);

#if ENABLE_PMU
        before = prefetch_pmu_read();
#endif
        for (size_t i = 0; i < options->probe_batch; i++)
            bop_trigger_access(triggers[i]);
        wait_for_prefetch();
        mfence();
#if ENABLE_PMU
        after = prefetch_pmu_read();
        stats->l1_events +=
            prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
        stats->l2_events +=
            prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
#endif

#if ENABLE_CACHE_PROBE
        for (size_t k = 0; k < options->probe_batch; k++) {
            size_t i = (k * 37u + 11u) % options->probe_batch;
            uint64_t latency = timed_reload_ns(targets[i]);

            stats->latency_ns += latency;
            stats->probes++;
            if (latency <= options->threshold_ns)
                stats->cache_hits++;
        }
#endif
    }

    free(targets);
    free(triggers);
}

static const char *mode_name(void)
{
#if ENABLE_PMU && ENABLE_CACHE_PROBE
    return "pmu+cache";
#elif ENABLE_PMU
    return "pmu";
#elif ENABLE_CACHE_PROBE
    return "cache";
#else
    return "none";
#endif
}

static void print_results(const struct options *options,
                          const struct retention_stats *stats)
{
#if ENABLE_PMU
    double denominator =
        (double)options->repeats * (double)options->probe_batch;
#endif

    printf("# BOP best-offset retention test\n");
    printf("# phase_order=train,decay,endpoint\n");
    printf("# decay targets are never demand-accessed\n");
    printf("# all endpoint triggers complete before target probes\n");
    printf("mode\toffset_lines\trepeats\ttrain_pairs\tdecay_triggers\t"
           "probe_batch\tprobes\tcache_hits\thits_per_1000\t"
           "avg_latency_ns\tl1_total\tl1_per_trigger\tl2_total\t"
           "l2_per_trigger\n");
    printf("%s\t%zu\t%zu\t%zu\t%zu\t%zu\t",
           mode_name(), options->offset_lines, options->repeats,
           options->train_pairs, options->decay_triggers,
           options->probe_batch);
#if ENABLE_CACHE_PROBE
    printf("%llu\t%llu\t%llu\t%.2f\t",
           (unsigned long long)stats->probes,
           (unsigned long long)stats->cache_hits,
           (unsigned long long)(stats->probes == 0 ? 0 :
               stats->cache_hits * 1000u / stats->probes),
           stats->probes == 0 ? 0.0 :
               (double)stats->latency_ns / (double)stats->probes);
#else
    printf("-\t-\t-\t-\t");
#endif
#if ENABLE_PMU
    printf("%llu\t%.6f\t%llu\t%.6f\n",
           (unsigned long long)stats->l1_events,
           (double)stats->l1_events / denominator,
           (unsigned long long)stats->l2_events,
           (double)stats->l2_events / denominator);
#else
    printf("-\t-\t-\t-\n");
#endif
}

static int parse_size(const char *text, size_t *value)
{
    char *end;
    unsigned long long parsed;

    if (text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        parsed > (unsigned long long)SIZE_MAX)
        return -1;
    *value = (size_t)parsed;
    return 0;
}

static void usage(const char *program)
{
    printf("usage: %s [options]\n", program);
    printf("  --repeats N          independent repetitions (default 10)\n");
    printf("  --threshold-ns N     cache-hit threshold (default 80)\n");
    printf("  --train-pairs N      fixed-offset training pairs (default 3000)\n");
    printf("  --decay-triggers N   unrelated triggers after training\n");
    printf("  --probe-batch N      endpoint trigger count (default 64)\n");
    printf("  --offset-lines N     trained offset in cache lines (default 16)\n");
    printf("  --cpu N              pin to this logical CPU (default 6)\n");
    printf("  --describe           print phase semantics without running\n");
}

static int parse_options(int argc, char **argv, struct options *options)
{
    enum {
        OPT_REPEATS = 1000,
        OPT_THRESHOLD,
        OPT_TRAIN_PAIRS,
        OPT_DECAY_TRIGGERS,
        OPT_PROBE_BATCH,
        OPT_OFFSET_LINES,
        OPT_CPU,
        OPT_DESCRIBE,
    };
    static const struct option long_options[] = {
        {"repeats", required_argument, NULL, OPT_REPEATS},
        {"threshold-ns", required_argument, NULL, OPT_THRESHOLD},
        {"train-pairs", required_argument, NULL, OPT_TRAIN_PAIRS},
        {"decay-triggers", required_argument, NULL, OPT_DECAY_TRIGGERS},
        {"probe-batch", required_argument, NULL, OPT_PROBE_BATCH},
        {"offset-lines", required_argument, NULL, OPT_OFFSET_LINES},
        {"cpu", required_argument, NULL, OPT_CPU},
        {"describe", no_argument, NULL, OPT_DESCRIBE},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int choice;

    while ((choice = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
        size_t *destination = NULL;

        switch (choice) {
        case OPT_REPEATS:
            destination = &options->repeats;
            break;
        case OPT_TRAIN_PAIRS:
            destination = &options->train_pairs;
            break;
        case OPT_DECAY_TRIGGERS:
            destination = &options->decay_triggers;
            break;
        case OPT_PROBE_BATCH:
            destination = &options->probe_batch;
            break;
        case OPT_OFFSET_LINES:
            destination = &options->offset_lines;
            break;
        case OPT_CPU:
            destination = &options->cpu;
            break;
        case OPT_THRESHOLD: {
            size_t parsed;

            if (parse_size(optarg, &parsed) != 0) {
                fprintf(stderr, "invalid numeric option: %s\n", optarg);
                return -1;
            }
            options->threshold_ns = parsed;
            continue;
        }
        case OPT_DESCRIBE:
            options->describe = 1;
            continue;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            return -1;
        }
        if (parse_size(optarg, destination) != 0) {
            fprintf(stderr, "invalid numeric option: %s\n", optarg);
            return -1;
        }
    }
    if (optind != argc) {
        fprintf(stderr, "unexpected positional argument: %s\n", argv[optind]);
        return -1;
    }
    return 0;
}

static int address_sets_fit(const struct options *options)
{
    size_t used = 0;
    size_t values[] = {
        options->train_pairs,
        noise_count(options->train_pairs),
        options->decay_triggers,
        options->probe_batch,
    };

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (values[i] > POOL_PAGES - used)
            return 0;
        used += values[i];
    }
    return 1;
}

static int pin_to_cpu(size_t cpu)
{
    cpu_set_t allowed;
    cpu_set_t selected;

    if (cpu >= CPU_SETSIZE || sched_getaffinity(0, sizeof(allowed), &allowed) != 0 ||
        !CPU_ISSET(cpu, &allowed)) {
        fprintf(stderr, "cpu must be present in the current affinity mask\n");
        return -1;
    }
    CPU_ZERO(&selected);
    CPU_SET(cpu, &selected);
    if (sched_setaffinity(0, sizeof(selected), &selected) != 0) {
        fprintf(stderr, "sched_setaffinity failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct options options = {
        .repeats = DEFAULT_REPEATS,
        .threshold_ns = DEFAULT_THRESHOLD_NS,
        .train_pairs = DEFAULT_TRAIN_PAIRS,
        .decay_triggers = DEFAULT_DECAY_TRIGGERS,
        .probe_batch = DEFAULT_PROBE_BATCH,
        .offset_lines = DEFAULT_OFFSET_LINES,
        .cpu = DEFAULT_CPU,
    };
    struct retention_stats stats = {0};
    size_t mapping_size = (size_t)POOL_PAGES * PAGE_SIZE;

    if (parse_options(argc, argv, &options) != 0) {
        usage(argv[0]);
        return 1;
    }
    if (options.describe) {
        printf("phase\tpurpose\tdemand_reads_of_predicted_targets\n");
        printf("train\tlearn the selected offset\tyes\n");
        printf("decay\tconsume unrelated triggers\tno\n");
        printf("endpoint\tmeasure retained prefetching\tafter all triggers\n");
        return 0;
    }
    if (options.repeats == 0 || options.train_pairs == 0 ||
        options.threshold_ns == 0) {
        fprintf(stderr, "repeats, train-pairs, and threshold-ns must be positive\n");
        return 1;
    }
    if (options.probe_batch == 0) {
        fprintf(stderr, "probe-batch must be positive\n");
        return 1;
    }
    if (options.offset_lines == 0 || options.offset_lines >= LINES_PER_PAGE) {
        fprintf(stderr, "offset-lines must be in 1..63\n");
        return 1;
    }
    if (!address_sets_fit(&options)) {
        fprintf(stderr, "address sets exceed mapped pool (%d pages)\n",
                POOL_PAGES);
        return 1;
    }
    if (pin_to_cpu(options.cpu) != 0)
        return 1;

    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }
    init_page_order();
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    prefault_mapping();

#if ENABLE_PMU
    if (pmu_setup() != 0) {
        munmap(mapping, mapping_size);
        return 2;
    }
#endif

    run_experiment(&options, &stats);
    print_results(&options, &stats);

#if ENABLE_PMU
    close(pmu_fd);
#endif
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
