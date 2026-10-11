#define _GNU_SOURCE

/* Reuse test-bop.c's randomized training and AArch64 cache-timing helpers. */
#ifndef ENABLE_PMU
#define ENABLE_PMU 0
#endif
#ifndef ENABLE_CACHE_PROBE
#define ENABLE_CACHE_PROBE 1
#endif
#define main bop_original_main
#include "test-bop.c"
#undef main

#define OFFSET_COUNT 32

static void cc4_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [rounds threshold_ns train_pairs random_seed]\n",
            program);
}

static int parse_cc4_value(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (text[0] == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || *end != '\0')
        return -1;
    *value = parsed;
    return 0;
}

static void shuffle_offsets(size_t *order, uint64_t *rng)
{
    for (size_t i = 0; i < OFFSET_COUNT; i++)
        order[i] = i;
    for (size_t i = OFFSET_COUNT - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(rng) % (i + 1));
        size_t temporary = order[i];

        order[i] = order[j];
        order[j] = temporary;
    }
}

static void prepare_cc4_trigger(size_t round, size_t train_pairs,
                                void **trigger, void **targets)
{
    uint64_t rng = PAGE_SEED ^ 0x13198a2e03707344ULL
                   ^ ((uint64_t)round * 0x94d049bb133111ebULL);
    size_t first_slot = train_pairs + noise_count(train_pairs);
    size_t page = round_slot_page(round, first_slot, 1);
    size_t base_line = (size_t)(splitmix64_next(&rng) % (LINES_PER_PAGE - 32));

    *trigger = page_addr(page, base_line);
    flush(*trigger);
    for (size_t i = 0; i < OFFSET_COUNT; i++) {
        targets[i] = page_addr(page, base_line + i + 1u);
        flush(targets[i]);
    }
    mfence();
}

int main(int argc, char **argv)
{
    size_t rounds = DEFAULT_ROUNDS;
    uint64_t threshold_ns = DEFAULT_THRESHOLD_NS;
    size_t train_pairs = DEFAULT_TRAIN_PAIRS;
    uint64_t random_seed = PAGE_SEED;
    size_t span_pages = offset_span_pages(OFFSET_COUNT);
    size_t pages_needed;
    uint64_t decision_rng;
    uint64_t correct = 0;
    struct timespec rounds_start;
    struct timespec rounds_end;
    void *trigger;
    void *targets[OFFSET_COUNT];

    if (argc != 1 && argc != 5) {
        cc4_usage(argv[0]);
        return 1;
    }
    if (argc == 5) {
        uint64_t values[3];

        for (int i = 0; i < 3; i++) {
            if (parse_cc4_value(argv[i + 1], &values[i]) != 0) {
                cc4_usage(argv[0]);
                return 1;
            }
        }
        rounds = (size_t)values[0];
        threshold_ns = values[1];
        train_pairs = (size_t)values[2];
        if (parse_cc4_value(argv[4], &random_seed) != 0) {
            cc4_usage(argv[0]);
            return 1;
        }
    }
    pages_needed = train_pairs + noise_count(train_pairs) + 1u;
    if (rounds == 0 || threshold_ns == 0 || train_pairs == 0 ||
        pages_needed * span_pages > POOL_PAGES) {
        cc4_usage(argv[0]);
        return 1;
    }

    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }
    init_page_order();
    init_slot_order(POOL_PAGES / span_pages, OFFSET_COUNT);
    mapping_size = (size_t)POOL_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    prefault_mapping();
    cpp_rctx();

    printf("# BOP random 5-bit offset inference test\n");
    printf("# random_seed=%llu rounds=%zu train_pairs=%zu "
           "random_value_range=1..32 probe_offsets=1..32 "
           "threshold_ns=%llu\n", (unsigned long long)random_seed, rounds,
           train_pairs, (unsigned long long)threshold_ns);
    printf("# probe order is shuffled independently each round\n");

    decision_rng = random_seed;
    clock_gettime(CLOCK_MONOTONIC_RAW, &rounds_start);
    for (size_t round = 0; round < rounds; round++) {
        size_t actual = (size_t)(splitmix64_next(&decision_rng) % OFFSET_COUNT);
        size_t order[OFFSET_COUNT];
        uint64_t order_rng = splitmix64_next(&decision_rng);
        uint64_t best_latency = UINT64_MAX;
        size_t inferred = 0;

        cpp_rctx();
        train_bop(TRAIN_FIXED_OFFSET, round, train_pairs, actual + 1u);
        prepare_cc4_trigger(round, train_pairs, &trigger, targets);
        bop_trigger_access(trigger);
        wait_for_prefetch();
        shuffle_offsets(order, &order_rng);
        for (size_t k = 0; k < OFFSET_COUNT; k++) {
            size_t candidate = order[k];
            uint64_t latency = timed_reload_ns(targets[candidate]);

            if (latency < best_latency) {
                best_latency = latency;
                inferred = candidate;
            }
        }
        if (actual == inferred)
            correct++;
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &rounds_end);
    {
        int64_t seconds = rounds_end.tv_sec - rounds_start.tv_sec;
        int64_t nanoseconds = rounds_end.tv_nsec - rounds_start.tv_nsec;
        uint64_t elapsed_ns;
        double bit_s;

        if (nanoseconds < 0) {
            seconds--;
            nanoseconds += 1000000000LL;
        }
        elapsed_ns = (uint64_t)seconds * 1000000000ULL +
                     (uint64_t)nanoseconds;
        bit_s = (double)rounds * 5.0 * 1e9 / (double)elapsed_ns;
        printf("# rounds_elapsed_ns=%llu bits_transferred=%llu "
               "bandwidth_bit_s=%.3f bandwidth_kbit_s=%.3f\n",
               (unsigned long long)elapsed_ns,
               (unsigned long long)rounds * 5ULL, bit_s, bit_s / 1000.0);
    }

    printf("# correct=%llu errors=%llu error_rate=%.6f "
           "error_rate_percent=%.3f\n", (unsigned long long)correct,
           (unsigned long long)(rounds - correct),
           (double)(rounds - correct) / rounds,
           100.0 * (double)(rounds - correct) / rounds);
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
