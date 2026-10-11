#define _GNU_SOURCE

/* Reuse the established training, page layout and trigger/probe primitives. */
#ifndef ENABLE_PMU
#define ENABLE_PMU 1
#endif
#ifndef ENABLE_CACHE_PROBE
#define ENABLE_CACHE_PROBE 0
#endif
#define main bop_original_main
#include "test-bop.c"
#undef main

struct conditional_stats {
    uint64_t rounds;
    uint64_t l2_prefetch_events;
};

static void print_condition(const char *name,
                            const struct conditional_stats *stats)
{
    printf("condition=%s rounds=%llu l2_prefetch_events=%llu\n",
           name, (unsigned long long)stats->rounds,
           (unsigned long long)stats->l2_prefetch_events);
}

static int parse_value(const char *text, uint64_t *value)
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

static void cc_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [rounds threshold_ns train_pairs trigger_batch "
            "[offset_lines [random_seed]]]\n", program);
}

int main(int argc, char **argv)
{
    size_t rounds = DEFAULT_ROUNDS;
    uint64_t threshold_ns = DEFAULT_THRESHOLD_NS;
    size_t train_pairs = DEFAULT_TRAIN_PAIRS;
    size_t trigger_batch = DEFAULT_TRIGGER_BATCH;
    size_t offset_lines = DEFAULT_BOP_OFFSET_LINES;
    uint64_t random_seed = PAGE_SEED;
    size_t span_pages;
    size_t slots_needed;
    size_t pages_needed;
    uint64_t value;
    void *triggers[1];
    void *targets[1];
    struct conditional_stats stats[2] = {{0}};
    uint64_t decision_rng;
    struct timespec rounds_start;
    struct timespec rounds_end;
    uint64_t correct_decisions = 0;
    uint64_t error_decisions = 0;

    if (argc != 1 && argc != 5 && argc != 6 && argc != 7) {
        cc_usage(argv[0]);
        return 1;
    }
    if (argc >= 5) {
        uint64_t values[4];

        for (int i = 0; i < 4; i++) {
            if (parse_value(argv[i + 1], &values[i]) != 0) {
                cc_usage(argv[0]);
                return 1;
            }
        }
        rounds = (size_t)values[0];
        threshold_ns = values[1];
        train_pairs = (size_t)values[2];
        trigger_batch = (size_t)values[3];
    }
    if (argc >= 6 && parse_value(argv[5], &value) != 0) {
        cc_usage(argv[0]);
        return 1;
    }
    if (argc >= 6)
        offset_lines = (size_t)value;
    if (argc == 7 && parse_value(argv[6], &random_seed) != 0) {
        cc_usage(argv[0]);
        return 1;
    }

    span_pages = offset_span_pages(offset_lines);
    slots_needed = train_pairs + noise_count(train_pairs) + trigger_batch;
    pages_needed = slots_needed * span_pages;
    if (rounds == 0 || threshold_ns == 0 || train_pairs == 0 ||
        trigger_batch != 1 || offset_lines == 0 || offset_lines >= LINES_PER_PAGE ||
        pages_needed > POOL_PAGES) {
        cc_usage(argv[0]);
        fprintf(stderr, "invalid arguments or pages_needed=%zu (limit=%d)\n",
                pages_needed, POOL_PAGES);
        return 1;
    }

    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }
    init_page_order();
    init_slot_order(POOL_PAGES / span_pages, offset_lines);
    mapping_size = (size_t)POOL_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    prefault_mapping();
    cpp_rctx();

#if !ENABLE_PMU
    fprintf(stderr, "bop-cc requires ENABLE_PMU=1\n");
    munmap(mapping, mapping_size);
    return 1;
#else
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        fprintf(stderr, "bop-cc requires a working prefetch PMU\n");
        munmap(mapping, mapping_size);
        return 1;
    }
#endif

    decision_rng = random_seed;
    printf("# BOP conditional-training PMU test\n");
    printf("# random_seed=%llu rounds=%zu train_pairs=%zu trigger_batch=1 "
           "offset_lines=%zu threshold_ns=%llu\n",
           (unsigned long long)random_seed, rounds, train_pairs,
           offset_lines, (unsigned long long)threshold_ns);
    printf("# each round randomly trains BOP (bit=1) or skips training (bit=0)\n");
    printf("# trigger uses bop_trigger_access on pages after the training region\n");
    printf("# classification uses L2 PMU only: L2 > 0.2 means bit=1; "
           "otherwise bit=0\n");

    clock_gettime(CLOCK_MONOTONIC_RAW, &rounds_start);
    for (size_t round = 0; round < rounds; round++) {
        int trained = (int)(splitmix64_next(&decision_rng) & 1u);
#if ENABLE_PMU
        struct prefetch_pmu_snapshot before;
        struct prefetch_pmu_snapshot after;
#endif

        cpp_rctx();
        train_bop(trained ? TRAIN_FIXED_OFFSET : TRAIN_NONE, round,
                  train_pairs, offset_lines);
        prepare_trigger_batch(round, train_pairs, trigger_batch, offset_lines,
                              triggers, targets);
#if ENABLE_PMU
        before = prefetch_pmu_read();
#endif
        for (size_t i = 0; i < trigger_batch; i++)
            bop_trigger_access(triggers[i]);
        wait_for_prefetch();
#if ENABLE_PMU
        after = prefetch_pmu_read();
        {
            uint64_t l2_delta =
                prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
            int inferred_trained = (double)l2_delta > 0.2;
            struct conditional_stats *current = &stats[inferred_trained];

            current->l2_prefetch_events += l2_delta;
            current->rounds++;
            if (inferred_trained == trained)
                correct_decisions++;
            else
                error_decisions++;
        }
#else
        (void)trained;
#endif
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &rounds_end);
    {
        int64_t seconds = rounds_end.tv_sec - rounds_start.tv_sec;
        int64_t nanoseconds = rounds_end.tv_nsec - rounds_start.tv_nsec;
        uint64_t elapsed_ns;

        if (nanoseconds < 0) {
            seconds--;
            nanoseconds += 1000000000LL;
        }
        elapsed_ns = (uint64_t)seconds * 1000000000ULL +
                     (uint64_t)nanoseconds;

        printf("# rounds_elapsed_ns=%llu rounds_elapsed_ms=%.3f\n",
               (unsigned long long)elapsed_ns,
               (double)elapsed_ns / 1000000.0);
        {
            double bit_s = (double)rounds * 1e9 / (double)elapsed_ns;

            printf("# bits_transferred=%zu bandwidth_bit_s=%.3f "
                   "bandwidth_kbit_s=%.3f\n", rounds, bit_s,
                   bit_s / 1000.0);
        }
    }

    printf("# inferred condition counts sum to rounds: %llu + %llu = %llu\n",
           (unsigned long long)stats[0].rounds,
           (unsigned long long)stats[1].rounds,
           (unsigned long long)(stats[0].rounds + stats[1].rounds));
    printf("# bit_decisions=%zu correct=%llu errors=%llu error_rate=%.6f "
           "error_rate_percent=%.3f\n", rounds,
           (unsigned long long)correct_decisions,
           (unsigned long long)error_decisions,
           rounds == 0 ? 0.0 : (double)error_decisions / (double)rounds,
           rounds == 0 ? 0.0 : 100.0 * (double)error_decisions / rounds);
    print_condition("untrained", &stats[0]);
    print_condition("trained", &stats[1]);

#if ENABLE_PMU
    close(pmu_fd);
#endif
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
