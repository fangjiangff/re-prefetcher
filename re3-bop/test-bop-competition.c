#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "This test requires AArch64 DC CIVAC, CPP RCTX, and CNTVCT_EL0."
#endif

#include "../until.h"

#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 2000
#endif

#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define TRAIN_PC_COUNT 16
#define POOL_PAGES 8192
#define DEFAULT_ROUNDS 100
#define DEFAULT_THRESHOLD_NS 80
#define DEFAULT_TRAIN_PAIRS 3000
#define DEFAULT_TRIGGER_BATCH 64
#define DEFAULT_OFFSET_A 8
#define DEFAULT_OFFSET_B 16
#define PAGE_SEED 0x243f6a8885a308d3ULL

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

enum first_offset {
    FIRST_NONE,
    FIRST_A,
    FIRST_B,
};

struct competition_case {
    const char *name;
    unsigned int a_weight;
    unsigned int b_weight;
    enum first_offset first;
    size_t phase_pairs[2];
    int sequential;
};

struct offset_stats {
    uint64_t probes;
    uint64_t hits;
    uint64_t latency_ns;
};

struct case_stats {
    struct offset_stats offset[2];
    uint64_t round_wins[2];
    uint64_t round_ties;
    uint64_t rounds_a_only;
    uint64_t rounds_b_only;
    uint64_t rounds_both;
    uint64_t rounds_neither;
    uint64_t rounds_mixed;
    uint64_t samples_a_only;
    uint64_t samples_b_only;
    uint64_t samples_both;
    uint64_t samples_neither;
};

struct options {
    size_t rounds;
    uint64_t threshold_ns;
    size_t train_pairs;
    size_t trigger_batch;
    size_t offset[2];
    size_t transition_pairs[2];
    int transition_specified[2];
    size_t cpu;
    uint64_t seed;
    int only_transition;
    int isolated_probes;
    int compact_training;
    int describe;
};

typedef void (*access_function)(void *);

static const struct competition_case competition_cases[] = {
    {"untrained", 0, 0, FIRST_NONE, {0, 0}, 0},
    {"only_a", 1, 0, FIRST_A, {0, 0}, 0},
    {"only_b", 0, 1, FIRST_B, {0, 0}, 0},
    {"balanced_a_first", 1, 1, FIRST_A, {0, 0}, 0},
    {"balanced_b_first", 1, 1, FIRST_B, {0, 0}, 0},
    {"a_dominant", 3, 1, FIRST_A, {0, 0}, 0},
    {"b_dominant", 1, 3, FIRST_B, {0, 0}, 0},
};

static uint8_t *mapping;
static size_t mapping_size;
static size_t page_order[POOL_PAGES];
static uint64_t timer_freq_hz;
static uint64_t page_seed;

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

static size_t round_page(size_t round, size_t position)
{
    return page_order[(round * 977u + position) % POOL_PAGES];
}

static void init_page_order(void)
{
    uint64_t rng = page_seed;

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
    uint64_t rng = page_seed ^ 0xa4093822299f31d0ULL;

    for (size_t i = 0; i < POOL_PAGES; i++) {
        size_t page = page_order[i];
        size_t line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);

        *page_addr(page, line) = (uint8_t)i;
    }
    mfence();
}

static size_t noise_count(size_t train_pairs)
{
    return train_pairs / 7u + (train_pairs % 7u != 0);
}

static size_t compact_page_count(size_t train_pairs, size_t offset)
{
    size_t chain_pairs = (LINES_PER_PAGE - 1u) / offset;
    size_t chain_loads = chain_pairs + 1u;
    size_t loads = train_pairs + 1u;

    if (train_pairs == 0)
        return 0;
    return (loads + chain_loads - 1u) / chain_loads;
}

static int training_offset_index(const struct competition_case *test_case,
                                 size_t position)
{
    unsigned int period = test_case->a_weight + test_case->b_weight;
    unsigned int phase;

    if (period == 0)
        return -1;
    phase = (unsigned int)(position % period);
    if (test_case->first == FIRST_B)
        return phase < test_case->b_weight ? 1 : 0;
    return phase < test_case->a_weight ? 0 : 1;
}

static void count_training_pairs(const struct competition_case *test_case,
                                 size_t train_pairs, size_t count[2])
{
    count[0] = 0;
    count[1] = 0;
    for (size_t i = 0; i < train_pairs; i++) {
        int index = training_offset_index(test_case, i);

        if (index >= 0)
            count[index]++;
    }
}

static void train_bop(const struct competition_case *test_case,
                      size_t round, size_t train_pairs,
                      const size_t offset[2])
{
    uint64_t rng = page_seed
                   ^ ((uint64_t)round * 0x94d049bb133111ebULL);
    size_t max_offset = offset[0] > offset[1] ? offset[0] : offset[1];
    size_t base_line_count = LINES_PER_PAGE - max_offset;
    size_t next_noise_page = train_pairs;

    if (test_case->first == FIRST_NONE)
        return;

    for (size_t i = 0; i < train_pairs; i++) {
        int index = training_offset_index(test_case, i);
        size_t page = round_page(round, i);
        size_t base_line =
            (size_t)(splitmix64_next(&rng) % base_line_count);
        uint8_t *base = page_addr(page, base_line);
        uint8_t *mate = page_addr(page, base_line + offset[index]);

        flush(base);
        flush(mate);
        mfence();
        train_pcs[(i * 5u) % TRAIN_PC_COUNT](base);
        nops();
        train_pcs[(i * 7u + 3u) % TRAIN_PC_COUNT](mate);
        nops();

        /* Break periodic alignment with the hardware's candidate walk. */
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

/* Train a single offset in a disjoint page range.  The sequential experiment
 * uses this to distinguish replacement by later evidence from simultaneous
 * competition in one interleaved stream. */
static void train_bop_segment(size_t index, size_t round, size_t train_pairs,
                              size_t first_page, const size_t offset[2])
{
    uint64_t rng = page_seed
                   ^ ((uint64_t)round * 0x94d049bb133111ebULL)
                   ^ ((uint64_t)(index + 1u) * 0xd6e8feb86659fd93ULL);
    size_t max_offset = offset[0] > offset[1] ? offset[0] : offset[1];
    size_t base_line_count = LINES_PER_PAGE - max_offset;
    size_t next_noise_page = first_page + train_pairs;

    for (size_t i = 0; i < train_pairs; i++) {
        size_t page = round_page(round, first_page + i);
        size_t base_line = (size_t)(splitmix64_next(&rng) % base_line_count);
        uint8_t *base = page_addr(page, base_line);
        uint8_t *mate = page_addr(page, base_line + offset[index]);

        flush(base);
        flush(mate);
        mfence();
        train_pcs[(i * 5u) % TRAIN_PC_COUNT](base);
        nops();
        train_pcs[(i * 7u + 3u) % TRAIN_PC_COUNT](mate);
        nops();
        if (i % 7u == 3u) {
            size_t noise_page = round_page(round, next_noise_page++);
            size_t noise_line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);
            uint8_t *noise = page_addr(noise_page, noise_line);

            flush(noise);
            mfence();
            train_pcs[(i * 11u + 1u) % TRAIN_PC_COUNT](noise);
            nops();
        }
    }
}

/* Compact training chains consecutive addresses so each adjacent load carries
 * the selected offset.  Different training PCs are rotated per load to keep
 * this experiment from relying on one PC's ordinary stride state. */
static void train_bop_segment_compact(size_t index, size_t round,
                                      size_t train_pairs, size_t first_page,
                                      const size_t offset[2])
{
    uint64_t rng = page_seed
                   ^ ((uint64_t)round * 0x94d049bb133111ebULL)
                   ^ ((uint64_t)(index + 1u) * 0xd6e8feb86659fd93ULL);
    size_t selected_offset = offset[index];
    size_t chain_pairs = (LINES_PER_PAGE - 1u) / selected_offset;
    size_t pages = compact_page_count(train_pairs, selected_offset);
    size_t next_noise_page = first_page + pages;
    size_t remaining_pairs = train_pairs;
    size_t load_number = 0;

    for (size_t page_index = 0; page_index < pages; page_index++) {
        size_t pairs = remaining_pairs < chain_pairs ? remaining_pairs : chain_pairs;
        size_t loads = pairs + 1u;
        size_t max_base = LINES_PER_PAGE - 1u - pairs * selected_offset;
        size_t base_line = (size_t)(splitmix64_next(&rng) % (max_base + 1u));

        for (size_t j = 0; j < loads; j++)
            flush(page_addr(round_page(round, first_page + page_index),
                            base_line + j * selected_offset));
        mfence();
        for (size_t j = 0; j < loads; j++) {
            uint8_t *address = page_addr(
                round_page(round, first_page + page_index),
                base_line + j * selected_offset);

            train_pcs[load_number % TRAIN_PC_COUNT](address);
            nops();
            load_number++;
        }
        if (remaining_pairs > pairs)
            remaining_pairs -= pairs;
        else
            remaining_pairs = 0;
    }

    for (size_t i = 0; i < train_pairs; i++) {
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

static void prepare_shared_trigger_batch(size_t round,
                                         size_t first_page,
                                         size_t trigger_batch,
                                         const size_t offset[2],
                                         void **triggers, void **targets)
{
    uint64_t rng = page_seed ^ 0x13198a2e03707344ULL
                   ^ ((uint64_t)round * 0x082efa98ec4e6c89ULL);
    size_t max_offset = offset[0] > offset[1] ? offset[0] : offset[1];
    size_t base_line_count = LINES_PER_PAGE - max_offset;
    for (size_t i = 0; i < trigger_batch; i++) {
        size_t page = round_page(round, first_page + i);
        size_t line =
            (size_t)(splitmix64_next(&rng) % base_line_count);

        triggers[i] = page_addr(page, line);
        flush(triggers[i]);
        for (size_t index = 0; index < 2; index++) {
            targets[index * trigger_batch + i] =
                page_addr(page, line + offset[index]);
            flush(targets[index * trigger_batch + i]);
        }
    }
    mfence();
}

static void shuffle_indices(size_t *indices, size_t count, uint64_t *rng)
{
    for (size_t i = 0; i < count; i++)
        indices[i] = i;
    for (size_t i = count - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(rng) % (i + 1));
        size_t temporary = indices[i];

        indices[i] = indices[j];
        indices[j] = temporary;
    }
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

static const char *first_offset_name(enum first_offset first)
{
    switch (first) {
    case FIRST_NONE:
        return "none";
    case FIRST_A:
        return "a";
    case FIRST_B:
        return "b";
    }
    return "unknown";
}

static void describe_cases(void)
{
    puts("case\ta_weight\tb_weight\tfirst\taddress_stream");
    for (size_t i = 0;
         i < sizeof(competition_cases) / sizeof(competition_cases[0]); i++) {
        const struct competition_case *test_case = &competition_cases[i];

        printf("%s\t%u\t%u\t%s\tpaired\n", test_case->name,
               test_case->a_weight, test_case->b_weight,
               first_offset_name(test_case->first));
    }
}

static void run_case(const struct competition_case *test_case,
                     const struct options *options)
{
    struct case_stats stats = {0};
    size_t group_size = options->trigger_batch;
    size_t training_count[2];
    size_t first_trigger_page;
    size_t high_index = options->offset[0] > options->offset[1] ? 0 : 1;
    size_t low_index = 1u - high_index;
    void **triggers = calloc(group_size, sizeof(*triggers));
    void **targets = calloc(2u * group_size, sizeof(*targets));
    size_t *order = calloc(group_size, sizeof(*order));
    uint8_t *sample_hits = calloc(2u * group_size, sizeof(*sample_hits));

    if (triggers == NULL || targets == NULL || order == NULL ||
        sample_hits == NULL) {
        fprintf(stderr, "allocating trigger arrays failed\n");
        exit(1);
    }

    if (test_case->sequential) {
        training_count[0] = test_case->phase_pairs[0];
        training_count[1] = test_case->phase_pairs[1];
        first_trigger_page =
            (options->compact_training
             ? compact_page_count(training_count[0], options->offset[0])
             : training_count[0]) + noise_count(training_count[0]) +
            (options->compact_training
             ? compact_page_count(training_count[1], options->offset[1])
             : training_count[1]) + noise_count(training_count[1]);
    } else {
        count_training_pairs(test_case, options->train_pairs, training_count);
        first_trigger_page = options->train_pairs +
                             noise_count(options->train_pairs);
    }
    for (size_t round = 0; round < options->rounds; round++) {
        uint64_t rng = page_seed
                       ^ ((uint64_t)round * 0xbe5466cf34e90c6cULL);
        uint64_t round_hits[2] = {0, 0};
        int measured_offset = options->isolated_probes ? (int)(round % 2u) : -1;

        cpp_rctx();
        if (test_case->sequential) {
            size_t second_first_page =
                (options->compact_training
                 ? compact_page_count(training_count[0], options->offset[0])
                 : training_count[0]) + noise_count(training_count[0]);

            if (options->compact_training) {
                train_bop_segment_compact(0, round, training_count[0], 0,
                                          options->offset);
                train_bop_segment_compact(1, round, training_count[1],
                                          second_first_page, options->offset);
            } else {
                train_bop_segment(0, round, training_count[0], 0,
                                  options->offset);
                train_bop_segment(1, round, training_count[1],
                                  second_first_page, options->offset);
            }
        } else {
            train_bop(test_case, round, options->train_pairs,
                      options->offset);
        }
        prepare_shared_trigger_batch(round, first_trigger_page, group_size,
                                     options->offset, triggers, targets);
        memset(sample_hits, 0, 2u * group_size * sizeof(*sample_hits));

        shuffle_indices(order, group_size, &rng);
        for (size_t k = 0; k < group_size; k++)
            bop_trigger_access(triggers[order[k]]);
        wait_for_prefetch();

        /* Probe in two phases. Random high-target accesses cannot train the
         * repeated high-to-low delta that adjacent paired probes would. */
        for (size_t phase = 0; phase < 2; phase++) {
            size_t index = phase == 0 ? high_index : low_index;

            if (measured_offset >= 0 && index != (size_t)measured_offset)
                continue;
            shuffle_indices(order, group_size, &rng);
            for (size_t k = 0; k < group_size; k++) {
                size_t sample = order[k];
                uint64_t latency = timed_reload_ns(
                    targets[index * group_size + sample]);
                int hit = latency <= options->threshold_ns;

                sample_hits[index * group_size + sample] = (uint8_t)hit;
                stats.offset[index].probes++;
                stats.offset[index].latency_ns += latency;
                if (hit) {
                    stats.offset[index].hits++;
                    round_hits[index]++;
                }
            }
        }
        if (options->isolated_probes)
            continue;
        for (size_t sample = 0; sample < group_size; sample++) {
            int a_hit = sample_hits[sample];
            int b_hit = sample_hits[group_size + sample];

            if (a_hit && b_hit)
                stats.samples_both++;
            else if (a_hit)
                stats.samples_a_only++;
            else if (b_hit)
                stats.samples_b_only++;
            else
                stats.samples_neither++;
        }

        if (round_hits[0] > round_hits[1])
            stats.round_wins[0]++;
        else if (round_hits[1] > round_hits[0])
            stats.round_wins[1]++;
        else
            stats.round_ties++;

        {
            int a_active = round_hits[0] * 4u >= group_size * 3u;
            int b_active = round_hits[1] * 4u >= group_size * 3u;
            int a_inactive = round_hits[0] * 4u <= group_size;
            int b_inactive = round_hits[1] * 4u <= group_size;

            if (a_active && b_inactive)
                stats.rounds_a_only++;
            else if (b_active && a_inactive)
                stats.rounds_b_only++;
            else if (a_active && b_active)
                stats.rounds_both++;
            else if (a_inactive && b_inactive)
                stats.rounds_neither++;
            else
                stats.rounds_mixed++;
        }
    }

    {
        double hit_rate[2];
        double average_latency[2];
        const char *winner;

        for (size_t index = 0; index < 2; index++) {
            hit_rate[index] = 1000.0 * (double)stats.offset[index].hits /
                              (double)stats.offset[index].probes;
            average_latency[index] =
                (double)stats.offset[index].latency_ns /
                (double)stats.offset[index].probes;
        }
        if (hit_rate[0] > hit_rate[1])
            winner = "a";
        else if (hit_rate[1] > hit_rate[0])
            winner = "b";
        else
            winner = "tie";

        printf("%s\t%zu\t%zu\t%.2f\t%.2f\t%.2f\t%.2f\t"
               "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t"
               "%llu\t%llu\t%llu\t%llu\t%s\t%.2f\n",
               test_case->name, training_count[0], training_count[1],
               hit_rate[0], hit_rate[1], average_latency[0],
               average_latency[1],
               (unsigned long long)stats.round_wins[0],
               (unsigned long long)stats.round_wins[1],
               (unsigned long long)stats.round_ties,
               (unsigned long long)stats.rounds_a_only,
               (unsigned long long)stats.rounds_b_only,
               (unsigned long long)stats.rounds_both,
               (unsigned long long)stats.rounds_neither,
               (unsigned long long)stats.rounds_mixed,
               (unsigned long long)stats.samples_a_only,
               (unsigned long long)stats.samples_b_only,
               (unsigned long long)stats.samples_both,
               (unsigned long long)stats.samples_neither, winner,
               hit_rate[0] - hit_rate[1]);
    }

    free(sample_hits);
    free(order);
    free(targets);
    free(triggers);
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "usage: %s [options]\n"
            "  --rounds N          independent training/probe rounds "
            "(default %d)\n"
            "  --threshold-ns N    cache-hit threshold (default %d)\n"
            "  --train-pairs N     total pairs in every trained case "
            "(default %d)\n"
            "  --trigger-batch N   shared triggers per round "
            "(default %d)\n"
            "  --offset-a N        first candidate, cache lines "
            "(default %d)\n"
            "  --offset-b N        second candidate, cache lines "
            "(default %d)\n"
            "  --transition-a-pairs N  phase-A pairs before phase B "
            "(requires --transition-b-pairs)\n"
            "  --transition-b-pairs N  phase-B pairs after phase A "
            "(requires --transition-a-pairs)\n"
            "  --only-transition   skip the seven simultaneous cases\n"
            "  --isolated-probes   odd/even rounds probe only one offset\n"
            "  --training-mode M   paired or compact transition training "
            "(default paired)\n"
            "  --seed N            random page/order seed (default 0x%llx)\n"
            "  --cpu N             pin to this logical CPU (default current)\n"
            "  --describe          print the case matrix without running\n"
            "  --help              show this help\n",
            program, DEFAULT_ROUNDS, DEFAULT_THRESHOLD_NS,
            DEFAULT_TRAIN_PAIRS, DEFAULT_TRIGGER_BATCH,
            DEFAULT_OFFSET_A, DEFAULT_OFFSET_B,
            (unsigned long long)PAGE_SEED);
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

static int parse_options(int argc, char **argv, struct options *options)
{
    enum {
        OPTION_ROUNDS = 1000,
        OPTION_THRESHOLD_NS,
        OPTION_TRAIN_PAIRS,
        OPTION_TRIGGER_BATCH,
        OPTION_OFFSET_A,
        OPTION_OFFSET_B,
        OPTION_TRANSITION_A_PAIRS,
        OPTION_TRANSITION_B_PAIRS,
        OPTION_ONLY_TRANSITION,
        OPTION_ISOLATED_PROBES,
        OPTION_TRAINING_MODE,
        OPTION_SEED,
        OPTION_CPU,
        OPTION_DESCRIBE,
    };
    static const struct option long_options[] = {
        {"rounds", required_argument, NULL, OPTION_ROUNDS},
        {"threshold-ns", required_argument, NULL, OPTION_THRESHOLD_NS},
        {"train-pairs", required_argument, NULL, OPTION_TRAIN_PAIRS},
        {"trigger-batch", required_argument, NULL, OPTION_TRIGGER_BATCH},
        {"offset-a", required_argument, NULL, OPTION_OFFSET_A},
        {"offset-b", required_argument, NULL, OPTION_OFFSET_B},
        {"transition-a-pairs", required_argument, NULL,
         OPTION_TRANSITION_A_PAIRS},
        {"transition-b-pairs", required_argument, NULL,
         OPTION_TRANSITION_B_PAIRS},
        {"only-transition", no_argument, NULL, OPTION_ONLY_TRANSITION},
        {"isolated-probes", no_argument, NULL, OPTION_ISOLATED_PROBES},
        {"training-mode", required_argument, NULL, OPTION_TRAINING_MODE},
        {"seed", required_argument, NULL, OPTION_SEED},
        {"cpu", required_argument, NULL, OPTION_CPU},
        {"describe", no_argument, NULL, OPTION_DESCRIBE},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;

    while ((option = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
        size_t parsed;

        switch (option) {
        case OPTION_ONLY_TRANSITION:
            options->only_transition = 1;
            break;
        case OPTION_ISOLATED_PROBES:
            options->isolated_probes = 1;
            break;
        case OPTION_TRAINING_MODE:
            if (strcmp(optarg, "paired") == 0)
                options->compact_training = 0;
            else if (strcmp(optarg, "compact") == 0)
                options->compact_training = 1;
            else {
                fprintf(stderr, "training-mode must be paired or compact\n");
                return -1;
            }
            break;
        case OPTION_DESCRIBE:
            options->describe = 1;
            break;
        case 'h':
            usage(stdout, argv[0]);
            exit(0);
        case OPTION_ROUNDS:
        case OPTION_THRESHOLD_NS:
        case OPTION_TRAIN_PAIRS:
        case OPTION_TRIGGER_BATCH:
        case OPTION_OFFSET_A:
        case OPTION_OFFSET_B:
        case OPTION_TRANSITION_A_PAIRS:
        case OPTION_TRANSITION_B_PAIRS:
        case OPTION_SEED:
        case OPTION_CPU:
            if (parse_size(optarg, &parsed) != 0) {
                fprintf(stderr, "invalid numeric argument: %s\n", optarg);
                return -1;
            }
            if (option == OPTION_ROUNDS)
                options->rounds = parsed;
            else if (option == OPTION_THRESHOLD_NS)
                options->threshold_ns = parsed;
            else if (option == OPTION_TRAIN_PAIRS)
                options->train_pairs = parsed;
            else if (option == OPTION_TRIGGER_BATCH)
                options->trigger_batch = parsed;
            else if (option == OPTION_OFFSET_A)
                options->offset[0] = parsed;
            else if (option == OPTION_OFFSET_B)
                options->offset[1] = parsed;
            else if (option == OPTION_TRANSITION_A_PAIRS) {
                options->transition_pairs[0] = parsed;
                options->transition_specified[0] = 1;
            } else if (option == OPTION_TRANSITION_B_PAIRS) {
                options->transition_pairs[1] = parsed;
                options->transition_specified[1] = 1;
            } else if (option == OPTION_SEED)
                options->seed = (uint64_t)parsed;
            else
                options->cpu = parsed;
            break;
        default:
            return -1;
        }
    }
    if (optind != argc) {
        fprintf(stderr, "unexpected positional argument: %s\n", argv[optind]);
        return -1;
    }
    return 0;
}

static int validate_options(const struct options *options)
{
    size_t noise_pages;
    size_t pages_needed;
    size_t transition_pages;

    if (options->offset[0] == 0 || options->offset[0] >= LINES_PER_PAGE ||
        options->offset[1] == 0 || options->offset[1] >= LINES_PER_PAGE) {
        fprintf(stderr, "offsets must be in cache lines 1..63\n");
        return -1;
    }
    if (options->offset[0] == options->offset[1]) {
        fprintf(stderr, "offset-a and offset-b must be distinct\n");
        return -1;
    }
    if (options->rounds == 0 || options->threshold_ns == 0 ||
        options->train_pairs == 0 || options->trigger_batch == 0) {
        fprintf(stderr, "rounds, threshold, train-pairs, and trigger-batch "
                "must be positive\n");
        return -1;
    }
    if (options->isolated_probes && options->rounds < 2) {
        fprintf(stderr, "isolated-probes requires at least two rounds\n");
        return -1;
    }
    if (options->transition_specified[0] !=
        options->transition_specified[1]) {
        fprintf(stderr, "transition-a-pairs and transition-b-pairs must be "
                "specified together\n");
        return -1;
    }
    if (options->transition_specified[0] &&
        options->transition_pairs[0] == 0) {
        fprintf(stderr, "transition-a-pairs must be positive\n");
        return -1;
    }
    if (options->only_transition && !options->transition_specified[0]) {
        fprintf(stderr, "only-transition requires both transition pair "
                "arguments\n");
        return -1;
    }
    if (options->cpu != SIZE_MAX && options->cpu >= CPU_SETSIZE) {
        fprintf(stderr, "cpu must be smaller than %d\n", CPU_SETSIZE);
        return -1;
    }
    if (options->train_pairs > POOL_PAGES ||
        options->trigger_batch > POOL_PAGES) {
        fprintf(stderr, "experiment exceeds the %d-page mapped pool\n",
                POOL_PAGES);
        return -1;
    }
    noise_pages = noise_count(options->train_pairs);
    if (noise_pages > POOL_PAGES - options->train_pairs) {
        fprintf(stderr, "experiment exceeds the %d-page mapped pool\n",
                POOL_PAGES);
        return -1;
    }
    pages_needed = options->train_pairs + noise_pages;
    if (options->trigger_batch > POOL_PAGES - pages_needed) {
        fprintf(stderr, "experiment exceeds the %d-page mapped pool\n",
                POOL_PAGES);
        return -1;
    }
    if (options->transition_specified[0]) {
        if (options->transition_pairs[0] > POOL_PAGES ||
            options->transition_pairs[1] > POOL_PAGES) {
            fprintf(stderr, "experiment exceeds the %d-page mapped pool\n",
                    POOL_PAGES);
            return -1;
        }
        transition_pages = options->transition_pairs[0] +
                           noise_count(options->transition_pairs[0]);
        if (transition_pages > POOL_PAGES - options->transition_pairs[1]) {
            fprintf(stderr, "experiment exceeds the %d-page mapped pool\n",
                    POOL_PAGES);
            return -1;
        }
        transition_pages += options->transition_pairs[1] +
                            noise_count(options->transition_pairs[1]);
        if (transition_pages > POOL_PAGES ||
            options->trigger_batch > POOL_PAGES - transition_pages) {
            fprintf(stderr, "experiment exceeds the %d-page mapped pool\n",
                    POOL_PAGES);
            return -1;
        }
    }
    return 0;
}

static int pin_to_cpu(const struct options *options)
{
    cpu_set_t affinity;
    int cpu;

    if (options->cpu == SIZE_MAX)
        cpu = sched_getcpu();
    else
        cpu = (int)options->cpu;
    if (cpu < 0) {
        fprintf(stderr, "sched_getcpu failed: %s\n", strerror(errno));
        return -1;
    }
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    if (sched_setaffinity(0, sizeof(affinity), &affinity) != 0) {
        fprintf(stderr, "pinning to CPU%d failed: %s\n", cpu,
                strerror(errno));
        return -1;
    }
    return cpu;
}

int main(int argc, char **argv)
{
    struct options options = {
        .rounds = DEFAULT_ROUNDS,
        .threshold_ns = DEFAULT_THRESHOLD_NS,
        .train_pairs = DEFAULT_TRAIN_PAIRS,
        .trigger_batch = DEFAULT_TRIGGER_BATCH,
        .offset = {DEFAULT_OFFSET_A, DEFAULT_OFFSET_B},
        .transition_pairs = {0, 0},
        .transition_specified = {0, 0},
        .cpu = SIZE_MAX,
        .seed = PAGE_SEED,
        .only_transition = 0,
        .isolated_probes = 0,
        .describe = 0,
    };
    int selected_cpu;

    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr, argv[0]);
        return 1;
    }
    if (validate_options(&options) != 0)
        return 1;
    if (options.describe) {
        describe_cases();
        return 0;
    }

    selected_cpu = pin_to_cpu(&options);
    if (selected_cpu < 0)
        return 1;

    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }

    page_seed = options.seed;
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

    printf("# BOP-inspired black-box two-offset competition test\n");
    printf("# observed behavior is implementation-specific and does not prove "
           "conformance to the published BOP algorithm\n");
    printf("# cpu=%d (affinity pinned by this process)\n", selected_cpu);
    printf("# offset_a=%zu offset_b=%zu line_size=%d\n",
           options.offset[0], options.offset[1], LINE_SIZE);
    printf("# seed=%llu\n", (unsigned long long)options.seed);
    printf("# probe_mode=%s\n",
           options.isolated_probes ? "isolated" : "joint");
    printf("# training_mode=%s\n",
           options.compact_training ? "compact" : "paired");
    printf("# rounds=%zu train_pairs=%zu shared_trigger_batch=%zu "
           "threshold_ns=%llu wait_nops=%d\n",
           options.rounds, options.train_pairs, options.trigger_batch,
           (unsigned long long)options.threshold_ns, PREFETCH_WAIT_NOPS);
    printf("# each trained case keeps total train_pairs constant; every trigger "
           "shares one page with both targets\n");
    printf("# CPP RCTX is not assumed to clear BOP state; 3000 default pairs "
           "favor convergence over reset-dependent interpretation\n");
    if (options.isolated_probes)
        printf("# odd/even rounds probe only one offset; target probes cannot "
               "prefetch each other within a round; joint/round outcome "
               "columns are not applicable and remain zero\n");
    else
        printf("# all larger-offset targets are probed before any smaller-offset "
               "target to avoid repeated high-to-low training pairs\n");
    printf("# round activity: active >=75%%, inactive <=25%%, otherwise mixed\n");
    if (!options.isolated_probes)
        printf("# samples_* are joint cache outcomes for one shared trigger; "
               "each row sums to rounds*shared_trigger_batch\n");
    printf("# winner means higher observed hit rate, not a hardware best-offset "
           "register read\n");
    printf("case\ttrain_a\ttrain_b\thits_a_per_1000\thits_b_per_1000\t"
           "avg_ns_a\tavg_ns_b\tround_wins_a\tround_wins_b\t"
           "round_ties\trounds_a_only\trounds_b_only\trounds_both\t"
           "rounds_neither\trounds_mixed\tsamples_a_only\tsamples_b_only\t"
           "samples_both\tsamples_neither\twinner\tdelta_a_minus_b\n");

    if (!options.only_transition) {
        for (size_t i = 0;
             i < sizeof(competition_cases) / sizeof(competition_cases[0]); i++)
            run_case(&competition_cases[i], &options);
    }

    if (options.transition_specified[0]) {
        const struct competition_case transition_case = {
            "transition_a_then_b", 0, 0, FIRST_NONE,
            {options.transition_pairs[0], options.transition_pairs[1]}, 1,
        };

        run_case(&transition_case, &options);
    }

    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
