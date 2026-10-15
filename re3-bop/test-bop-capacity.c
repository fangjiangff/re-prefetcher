#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef __aarch64__
#error "This test requires AArch64 DC CIVAC, CPP RCTX, and CNTVCT_EL0."
#endif

#include "../until.h"

#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 2000
#endif

#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define TRAIN_PC_COUNT 16
#define MAX_CANDIDATES 32
#define POOL_PAGES 65536
#define DEFAULT_ROUNDS 30
#define DEFAULT_THRESHOLD_NS 80
#define DEFAULT_TRAIN_PAIRS 700
#define DEFAULT_TRIGGER_BATCH 64
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

struct options {
    size_t rounds;
    size_t threshold_ns;
    size_t train_pairs;
    size_t trigger_batch;
    size_t cpu;
    size_t count;
    size_t offsets[MAX_CANDIDATES];
};

struct offset_stats {
    uint64_t hits;
    uint64_t latency_ns;
};

typedef void (*access_function)(void *);

static uint8_t *mapping;
static size_t mapping_size;
static size_t page_order[POOL_PAGES];
static uint64_t timer_freq_hz;

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
        asm volatile("ldr " #reg ", [%0]" :: "r"(addr) : #reg, "memory"); \
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

__attribute__((noinline)) static void bop_trigger_access(void *addr)
{
    asm volatile("ldr x17, [%0]" :: "r"(addr) : "x17", "memory");
}

static inline __attribute__((always_inline)) void wait_for_prefetch(void)
{
    asm volatile(".rept " STRINGIFY(PREFETCH_WAIT_NOPS) "\n\t"
                 "nop\n\t"
                 ".endr\n\t" ::: "memory");
}

static uint8_t *page_addr(size_t page, size_t line)
{
    return mapping + page * PAGE_SIZE + line * LINE_SIZE;
}

static size_t round_page(size_t round, size_t position)
{
    return page_order[(round * 977u + position) % POOL_PAGES];
}

static void init_page_order(void)
{
    uint64_t rng = 0x243f6a8885a308d3ULL;

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
    for (size_t i = 0; i < POOL_PAGES; i++)
        *page_addr(page_order[i], i % LINES_PER_PAGE) = (uint8_t)i;
    mfence();
}

static size_t noise_count(size_t pairs)
{
    return pairs / 7u + (pairs % 7u != 0);
}

static void train_all(const struct options *options, size_t round,
                      size_t total_pairs, size_t max_offset)
{
    uint64_t rng = 0x243f6a8885a308d3ULL ^
                   ((uint64_t)round * 0x94d049bb133111ebULL);
    size_t base_line_count = LINES_PER_PAGE - max_offset;
    size_t next_noise_page = total_pairs;

    for (size_t pair = 0; pair < options->train_pairs; pair++) {
        for (size_t candidate = 0; candidate < options->count; candidate++) {
            size_t position = pair * options->count + candidate;
            size_t page = round_page(round, position);
            size_t base_line =
                (size_t)(splitmix64_next(&rng) % base_line_count);
            uint8_t *base = page_addr(page, base_line);
            uint8_t *mate =
                page_addr(page, base_line + options->offsets[candidate]);

            flush(base);
            flush(mate);
            mfence();
            train_pcs[(position * 5u) % TRAIN_PC_COUNT](base);
            nops();
            train_pcs[(position * 7u + 3u) % TRAIN_PC_COUNT](mate);
            nops();

            if (position % 7u == 3u) {
                size_t noise_page = round_page(round, next_noise_page++);
                size_t noise_line =
                    (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);
                uint8_t *noise = page_addr(noise_page, noise_line);

                flush(noise);
                mfence();
                train_pcs[(position * 11u + 1u) % TRAIN_PC_COUNT](noise);
                nops();
            }
        }
    }
}

static void prepare_triggers(const struct options *options, size_t round,
                             size_t first_page, size_t max_offset,
                             void **triggers, void **targets)
{
    uint64_t rng = 0x13198a2e03707344ULL ^
                   ((uint64_t)round * 0x082efa98ec4e6c89ULL);
    size_t base_line_count = LINES_PER_PAGE - max_offset;

    for (size_t i = 0; i < options->trigger_batch; i++) {
        size_t page = round_page(round, first_page + i);
        size_t line = (size_t)(splitmix64_next(&rng) % base_line_count);

        triggers[i] = page_addr(page, line);
        flush(triggers[i]);
        for (size_t candidate = 0; candidate < options->count; candidate++) {
            targets[candidate * options->trigger_batch + i] =
                page_addr(page, line + options->offsets[candidate]);
            flush(targets[candidate * options->trigger_batch + i]);
        }
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

static int parse_size(const char *text, size_t *value)
{
    char *end;
    unsigned long long parsed;

    if (text[0] == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || *end != '\0' || parsed > SIZE_MAX)
        return -1;
    *value = (size_t)parsed;
    return 0;
}

static int parse_offsets(const char *text, struct options *options)
{
    char *copy = strdup(text);
    char *cursor;
    char *token;

    if (copy == NULL)
        return -1;
    cursor = copy;
    while ((token = strsep(&cursor, ",")) != NULL) {
        size_t parsed;

        if (options->count == MAX_CANDIDATES || parse_size(token, &parsed)) {
            free(copy);
            return -1;
        }
        if (parsed == 0 || parsed >= LINES_PER_PAGE) {
            free(copy);
            return -1;
        }
        for (size_t i = 0; i < options->count; i++) {
            if (options->offsets[i] == parsed) {
                free(copy);
                return -2;
            }
        }
        options->offsets[options->count++] = parsed;
    }
    free(copy);
    return options->count == 0 ? -1 : 0;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "usage: %s --offsets LIST [options]\n"
            "  --offsets LIST       comma-separated distinct offsets in cache lines\n"
            "  --rounds N           independent training/probe rounds (default %d)\n"
            "  --train-pairs N      pairs per candidate (default %d)\n"
            "  --trigger-batch N    shared triggers per round (default %d)\n"
            "  --threshold-ns N     cache-hit threshold (default %d)\n"
            "  --cpu N              pin to logical CPU\n",
            program, DEFAULT_ROUNDS, DEFAULT_TRAIN_PAIRS,
            DEFAULT_TRIGGER_BATCH, DEFAULT_THRESHOLD_NS);
}

static int parse_options(int argc, char **argv, struct options *options)
{
    enum { OPTION_OFFSETS = 1000, OPTION_ROUNDS, OPTION_TRAIN_PAIRS,
           OPTION_TRIGGER_BATCH, OPTION_THRESHOLD_NS, OPTION_CPU };
    static const struct option long_options[] = {
        {"offsets", required_argument, NULL, OPTION_OFFSETS},
        {"rounds", required_argument, NULL, OPTION_ROUNDS},
        {"train-pairs", required_argument, NULL, OPTION_TRAIN_PAIRS},
        {"trigger-batch", required_argument, NULL, OPTION_TRIGGER_BATCH},
        {"threshold-ns", required_argument, NULL, OPTION_THRESHOLD_NS},
        {"cpu", required_argument, NULL, OPTION_CPU},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;

    while ((option = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
        size_t parsed;

        if (option == 'h') {
            usage(stdout, argv[0]);
            exit(0);
        }
        if (option == OPTION_OFFSETS) {
            int result = parse_offsets(optarg, options);

            if (result == -2) {
                fprintf(stderr, "offsets must be distinct\n");
                return -1;
            }
            if (result != 0) {
                fprintf(stderr, "invalid --offsets list\n");
                return -1;
            }
            continue;
        }
        if (parse_size(optarg, &parsed) != 0)
            return -1;
        if (option == OPTION_ROUNDS)
            options->rounds = parsed;
        else if (option == OPTION_TRAIN_PAIRS)
            options->train_pairs = parsed;
        else if (option == OPTION_TRIGGER_BATCH)
            options->trigger_batch = parsed;
        else if (option == OPTION_THRESHOLD_NS)
            options->threshold_ns = parsed;
        else if (option == OPTION_CPU)
            options->cpu = parsed;
        else
            return -1;
    }
    return optind == argc ? 0 : -1;
}

static int validate_options(const struct options *options)
{
    size_t total_pairs;
    size_t pages_needed;

    if (options->count == 0 || options->rounds == 0 ||
        options->train_pairs == 0 || options->trigger_batch == 0 ||
        options->threshold_ns == 0) {
        fprintf(stderr, "offsets, rounds, train-pairs, trigger-batch, and "
                "threshold must be positive\n");
        return -1;
    }
    if (options->cpu != SIZE_MAX && options->cpu >= CPU_SETSIZE) {
        fprintf(stderr, "cpu must be smaller than %d\n", CPU_SETSIZE);
        return -1;
    }
    if (options->train_pairs > SIZE_MAX / options->count) {
        fprintf(stderr, "training page count overflows\n");
        return -1;
    }
    total_pairs = options->train_pairs * options->count;
    if (total_pairs > POOL_PAGES ||
        noise_count(total_pairs) > POOL_PAGES - total_pairs) {
        fprintf(stderr, "experiment exceeds the mapped pool\n");
        return -1;
    }
    pages_needed = total_pairs + noise_count(total_pairs);
    if (options->trigger_batch > POOL_PAGES - pages_needed) {
        fprintf(stderr, "experiment exceeds the mapped pool\n");
        return -1;
    }
    return 0;
}

static int pin_to_cpu(const struct options *options)
{
    cpu_set_t affinity;
    int cpu = options->cpu == SIZE_MAX ? sched_getcpu() : (int)options->cpu;

    if (cpu < 0)
        return -1;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    if (sched_setaffinity(0, sizeof(affinity), &affinity) != 0)
        return -1;
    return cpu;
}

int main(int argc, char **argv)
{
    struct options options = {
        .rounds = DEFAULT_ROUNDS,
        .threshold_ns = DEFAULT_THRESHOLD_NS,
        .train_pairs = DEFAULT_TRAIN_PAIRS,
        .trigger_batch = DEFAULT_TRIGGER_BATCH,
        .cpu = SIZE_MAX,
    };
    size_t total_pairs;
    size_t max_offset = 0;
    size_t first_trigger_page;
    void **triggers;
    void **targets;
    struct offset_stats *stats;
    uint64_t all_active_rounds = 0;
    int selected_cpu;

    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr, argv[0]);
        return 1;
    }
    if (validate_options(&options) != 0)
        return 1;
    selected_cpu = pin_to_cpu(&options);
    if (selected_cpu < 0)
        return 1;
    for (size_t i = 0; i < options.count; i++)
        if (options.offsets[i] > max_offset)
            max_offset = options.offsets[i];
    total_pairs = options.train_pairs * options.count;
    first_trigger_page = total_pairs + noise_count(total_pairs);
    triggers = calloc(options.trigger_batch, sizeof(*triggers));
    targets = calloc(options.count * options.trigger_batch, sizeof(*targets));
    stats = calloc(options.count, sizeof(*stats));
    if (triggers == NULL || targets == NULL || stats == NULL)
        return 1;

    timer_freq_hz = read_cntfrq_el0();
    init_page_order();
    mapping_size = (size_t)POOL_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED)
        return 1;
    prefault_mapping();

    for (size_t round = 0; round < options.rounds; round++) {
        uint64_t round_hits[MAX_CANDIDATES] = {0};

        cpp_rctx();
        train_all(&options, round, total_pairs, max_offset);
        prepare_triggers(&options, round, first_trigger_page, max_offset,
                         triggers, targets);
        for (size_t i = 0; i < options.trigger_batch; i++)
            bop_trigger_access(triggers[i]);
        wait_for_prefetch();
        for (size_t candidate = options.count; candidate-- > 0;) {
            for (size_t i = 0; i < options.trigger_batch; i++) {
                uint64_t latency =
                    timed_reload_ns(targets[candidate * options.trigger_batch + i]);

                stats[candidate].latency_ns += latency;
                if (latency <= options.threshold_ns) {
                    stats[candidate].hits++;
                    round_hits[candidate]++;
                }
            }
        }
        {
            int all_active = 1;

            for (size_t candidate = 0; candidate < options.count; candidate++)
                if (round_hits[candidate] * 4u <
                    options.trigger_batch * 3u)
                    all_active = 0;
            if (all_active)
                all_active_rounds++;
        }
    }

    printf("# BOP multi-offset capacity test; cpu=%d rounds=%zu train_pairs_per_candidate=%zu trigger_batch=%zu threshold_ns=%zu\n",
           selected_cpu, options.rounds, options.train_pairs,
           options.trigger_batch, options.threshold_ns);
    printf("# active means >=75%% hits in a round; all_active requires every candidate active\n");
    printf("candidate_count\tall_active_rounds");
    for (size_t candidate = 0; candidate < options.count; candidate++)
        printf("\toffset_%zu_hits_per_1000", options.offsets[candidate]);
    putchar('\n');
    printf("%zu\t%llu", options.count,
           (unsigned long long)all_active_rounds);
    for (size_t candidate = 0; candidate < options.count; candidate++) {
        double hits = 1000.0 * (double)stats[candidate].hits /
                      (double)(options.rounds * options.trigger_batch);

        printf("\t%.2f", hits);
    }
    putchar('\n');

    free(stats);
    free(targets);
    free(triggers);
    munmap(mapping, mapping_size);
    return 0;
}
