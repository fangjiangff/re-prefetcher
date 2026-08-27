#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef __aarch64__
#error "AArch64 is required"
#endif

#include "../until.h"

#ifndef ENABLE_PMU
#define ENABLE_PMU 0
#endif

#if ENABLE_PMU
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"
#endif

#ifndef NODE_COUNT
#define NODE_COUNT 64
#endif
#define MAPPING_PAGES 256
#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define NODE_SEED 0x9e3779b97f4a7c15ULL
#define WAIT_NOPS 10000

struct node {
    size_t page;
    size_t line;
};

struct result {
    uint64_t probes;
    uint64_t hits;
    uint64_t latency;
    uint64_t l1_events;
    uint64_t l2_events;
};

static struct node nodes[NODE_COUNT];
static struct node dummy_nodes[MAPPING_PAGES];
static uint8_t *mapping;
static uint8_t *dummy_mapping;
static uint64_t timer_frequency;
#if ENABLE_PMU
static int pmu_fd = -1;
#endif

static int pmu_setup(void)
{
#if ENABLE_PMU
    pmu_fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
    if (pmu_fd < 0) {
        fprintf(stderr, "open %s: %s\n", PREFETCH_PMU_DEVICE_PATH,
                strerror(errno));
        return -1;
    }
    if (ioctl(pmu_fd, PREFETCH_PMU_IOC_REARM) != 0) {
        fprintf(stderr, "PMU rearm: %s\n", strerror(errno));
        return -1;
    }
#endif
    return 0;
}

static inline uint64_t splitmix64_next(uint64_t *state)
{
    uint64_t value;
    *state += 0x9e3779b97f4a7c15ULL;
    value = *state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static inline uint64_t counter(void)
{
    uint64_t value;
    asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb"
                 : "=r"(value) :: "memory");
    return value;
}

static inline uint64_t ticks_to_ns(uint64_t ticks)
{
    __uint128_t product = (__uint128_t)ticks * 1000000000ULL;
    product += timer_frequency / 2;
    return (uint64_t)(product / timer_frequency);
}

static inline void access_node(void *address)
{
    asm volatile("ldr x9, [%0]" :: "r"(address) : "x9", "memory");
}

typedef void (*access_function)(void *);
#define DEFINE_PC_ACCESS(number, reg)                                  \
    __attribute__((noinline)) static void access_pc##number(void *p)   \
    {                                                                  \
        asm volatile("ldr " #reg ", [%0]" :: "r"(p) : #reg, "memory"); \
    }

DEFINE_PC_ACCESS(0, x9)
DEFINE_PC_ACCESS(1, x10)
DEFINE_PC_ACCESS(2, x11)
DEFINE_PC_ACCESS(3, x12)
DEFINE_PC_ACCESS(4, x13)
DEFINE_PC_ACCESS(5, x14)
DEFINE_PC_ACCESS(6, x15)
DEFINE_PC_ACCESS(7, x16)
DEFINE_PC_ACCESS(8, x17)

static access_function const pc_accesses[] = {
    access_pc0, access_pc1, access_pc2, access_pc3, access_pc4,
    access_pc5, access_pc6, access_pc7, access_pc8,
};

static unsigned pattern_pc(unsigned pattern, unsigned position)
{
    if (pattern <= 1)
        return pattern;
    if (pattern == 2)
        return 1 + position % 2;
    return 1 + position % 8;
}

static inline void wait_for_prefetch(void)
{
    asm volatile(".rept 10000\n\tnop\n\t.endr" ::: "memory");
}

static uint8_t *node_address(size_t index)
{
    return mapping + nodes[index].page * PAGE_SIZE +
           nodes[index].line * LINE_SIZE;
}

static uint8_t *dummy_address(size_t index)
{
    index %= MAPPING_PAGES;
    return dummy_mapping + dummy_nodes[index].page * PAGE_SIZE +
           dummy_nodes[index].line * LINE_SIZE;
}

static void initialize_nodes(void)
{
    uint64_t rng = NODE_SEED;
    uint64_t dummy_rng = NODE_SEED ^ 0xd1b54a32d192ed03ULL;
    size_t pages[MAPPING_PAGES];
    size_t dummy_pages[MAPPING_PAGES];

    for (size_t i = 0; i < MAPPING_PAGES; ++i)
        pages[i] = dummy_pages[i] = i;
    for (size_t i = MAPPING_PAGES - 1; i > 0; --i) {
        size_t j = splitmix64_next(&rng) % (i + 1);
        size_t temporary = pages[i];
        pages[i] = pages[j];
        pages[j] = temporary;
    }
    for (size_t i = 0; i < NODE_COUNT; ++i) {
        nodes[i].page = pages[i];
        nodes[i].line = splitmix64_next(&rng) % LINES_PER_PAGE;
    }
    for (size_t i = MAPPING_PAGES - 1; i > 0; --i) {
        size_t j = splitmix64_next(&dummy_rng) % (i + 1);
        size_t temporary = dummy_pages[i];
        dummy_pages[i] = dummy_pages[j];
        dummy_pages[j] = temporary;
    }
    for (size_t i = 0; i < MAPPING_PAGES; ++i) {
        dummy_nodes[i].page = dummy_pages[i];
        dummy_nodes[i].line = splitmix64_next(&dummy_rng) % LINES_PER_PAGE;
    }
}

static void flush_nodes(void)
{
    for (size_t i = 0; i < NODE_COUNT; ++i)
        flush(node_address(i));
    mfence();
}

static void flush_dummies(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        flush(dummy_address(i));
    mfence();
}

static size_t sequence_index(unsigned start, unsigned position)
{
    return (start + position) % NODE_COUNT;
}

static void train(unsigned replays, unsigned start)
{
    for (unsigned replay = 0; replay < replays; ++replay) {
        flush_nodes();
        for (size_t i = 0; i < NODE_COUNT; ++i) {
            access_node(node_address(sequence_index(start, i)));
            nops();
        }
    }
}

static void train_with_pc(unsigned replays, unsigned start, unsigned pattern)
{
    for (unsigned replay = 0; replay < replays; ++replay) {
        flush_nodes();
        for (unsigned position = 0; position < NODE_COUNT; ++position) {
            pc_accesses[pattern_pc(pattern, position)](
                node_address(sequence_index(start, position)));
            nops();
        }
    }
}

static uint64_t probe_load(void *address)
{
    uint64_t start, end;
    mfence();
    start = counter();
    mLoad_inline(address);
    end = counter();
    mfence();
    flush(address);
    mfence();
    return ticks_to_ns(end - start);
}

static void run_sample(int trained, unsigned prefix, unsigned replays,
                       uint64_t threshold, unsigned start, unsigned mode,
                       struct result *result)
{
#if ENABLE_PMU
    struct prefetch_pmu_snapshot before, after;
#endif
    cpp_rctx();
    if (trained)
        train(replays, start);
    flush_nodes();

    if (mode == 2)
        flush_dummies(prefix - 1);

    /* Supply prefix-1 context accesses, then the exact trained anchor. */
    for (unsigned i = 0; i + 1 < prefix; ++i) {
        size_t index;
        if (mode == 0)
            index = sequence_index(start, i);
        else if (mode == 1)
            index = sequence_index(start, prefix - 2 - i);
        else {
            access_node(dummy_address(i));
            nops();
            continue;
        }
        access_node(node_address(index));
        nops();
    }
#if ENABLE_PMU
    before = prefetch_pmu_read();
#endif
    access_node(node_address(sequence_index(start, prefix - 1)));
    nops();
    wait_for_prefetch();
#if ENABLE_PMU
    after = prefetch_pmu_read();
    result->l1_events += prefetch_pmu_delta(after.l1_hwprf,
                                             before.l1_hwprf);
    result->l2_events += prefetch_pmu_delta(after.l2_prefetch,
                                             before.l2_prefetch);
#endif

    uint64_t latency = probe_load(
        node_address(sequence_index(start, prefix)));
    result->probes++;
    result->latency += latency;
    result->hits += latency <= threshold;
}

static void run_target_sample(int trained, unsigned prefix,
                              unsigned target_offset, unsigned replays,
                              uint64_t threshold, unsigned start,
                              struct result *result)
{
#if ENABLE_PMU
    struct prefetch_pmu_snapshot before, after;
#endif
    size_t target = sequence_index(start, prefix - 1 + target_offset);

    cpp_rctx();
    if (trained)
        train(replays, start);
    flush_nodes();
    for (unsigned i = 0; i + 1 < prefix; ++i) {
        access_node(node_address(sequence_index(start, i)));
        nops();
    }

    /* Remove any earlier lookahead: only the final anchor may refill target. */
    flush(node_address(target));
    mfence();
#if ENABLE_PMU
    before = prefetch_pmu_read();
#endif
    access_node(node_address(sequence_index(start, prefix - 1)));
    nops();
    wait_for_prefetch();
#if ENABLE_PMU
    after = prefetch_pmu_read();
    result->l1_events += prefetch_pmu_delta(after.l1_hwprf,
                                             before.l1_hwprf);
    result->l2_events += prefetch_pmu_delta(after.l2_prefetch,
                                             before.l2_prefetch);
#endif
    {
        uint64_t latency = probe_load(node_address(target));
        result->probes++;
        result->latency += latency;
        result->hits += latency <= threshold;
    }
}

static void run_target_scan(unsigned samples, unsigned replays,
                            uint64_t threshold, unsigned start)
{
    struct result calibration[NODE_COUNT];
    unsigned calibration_samples = samples < 20 ? samples : 20;
    unsigned chosen_prefix = 1;
    double best_l2 = -1.0;

    memset(calibration, 0, sizeof(calibration));
    for (unsigned prefix = 1; prefix + 1 < NODE_COUNT; ++prefix) {
        for (unsigned sample = 0; sample < calibration_samples; ++sample)
            run_sample(1, prefix, replays, threshold, start, 0,
                       &calibration[prefix]);
        double average = (double)calibration[prefix].l2_events /
                         calibration[prefix].probes;
        if (average > best_l2) {
            best_l2 = average;
            chosen_prefix = prefix;
        }
    }

    unsigned maximum_offset = NODE_COUNT - 1;
    printf("# target scan replays=%u chosen_prefix=%u anchor=%zu "
           "calibration_l2=%.4f max_offset=%u wrap=cyclic\n",
           replays, chosen_prefix,
           sequence_index(start, chosen_prefix - 1), best_l2,
           maximum_offset);
    printf("# offset target_node u_hits t_hits u_per_1000 t_per_1000 "
           "u_avg_ns t_avg_ns u_l1 t_l1 u_l2 t_l2\n");
    for (unsigned offset = 1; offset <= maximum_offset; ++offset) {
        struct result untrained = {0};
        struct result trained = {0};
        for (unsigned sample = 0; sample < samples; ++sample) {
            if (sample & 1) {
                run_target_sample(1, chosen_prefix, offset, replays,
                                  threshold, start, &trained);
                run_target_sample(0, chosen_prefix, offset, replays,
                                  threshold, start, &untrained);
            } else {
                run_target_sample(0, chosen_prefix, offset, replays,
                                  threshold, start, &untrained);
                run_target_sample(1, chosen_prefix, offset, replays,
                                  threshold, start, &trained);
            }
        }
        printf("%2u %11zu %6llu %6llu %10.2f %10.2f %8.2f %8.2f "
               "%6.3f %6.3f %6.3f %6.3f\n",
               offset,
               sequence_index(start, chosen_prefix - 1 + offset),
               (unsigned long long)untrained.hits,
               (unsigned long long)trained.hits,
               1000.0 * untrained.hits / untrained.probes,
               1000.0 * trained.hits / trained.probes,
               (double)untrained.latency / untrained.probes,
               (double)trained.latency / trained.probes,
               (double)untrained.l1_events / untrained.probes,
               (double)trained.l1_events / trained.probes,
               (double)untrained.l2_events / untrained.probes,
               (double)trained.l2_events / trained.probes);
    }
}

static void run_pc_sample(int trained, unsigned prefix, unsigned replays,
                          unsigned start, unsigned train_pattern,
                          unsigned prefix_pattern, unsigned trigger_pc,
                          struct result *result)
{
#if ENABLE_PMU
    struct prefetch_pmu_snapshot before, after;
#endif
    cpp_rctx();
    if (trained)
        train_with_pc(replays, start, train_pattern);
    flush_nodes();
    for (unsigned i = 0; i + 1 < prefix; ++i) {
        pc_accesses[pattern_pc(prefix_pattern, i)](
            node_address(sequence_index(start, i)));
        nops();
    }
#if ENABLE_PMU
    before = prefetch_pmu_read();
#endif
    pc_accesses[trigger_pc](
        node_address(sequence_index(start, prefix - 1)));
    nops();
    wait_for_prefetch();
#if ENABLE_PMU
    after = prefetch_pmu_read();
    result->l1_events += prefetch_pmu_delta(after.l1_hwprf,
                                             before.l1_hwprf);
    result->l2_events += prefetch_pmu_delta(after.l2_prefetch,
                                             before.l2_prefetch);
#endif
    result->probes++;
}

struct pc_case {
    const char *name;
    unsigned train_pattern;
    unsigned prefix_pattern;
    unsigned trigger_pc;
};

static void run_pc_scan(unsigned samples, unsigned replays, unsigned start)
{
    static const struct pc_case cases[] = {
        {"all_same_pc0",       0, 0, 0},
        {"training_pc1",       1, 0, 0},
        {"train_trigger_pc1",  1, 0, 1},
        {"all_same_pc1",       1, 1, 1},
        {"train_trig1_pref2",  1, 2, 1},
        {"prefix_pc1",         0, 1, 0},
        {"trigger_pc1",        0, 0, 1},
        {"three_stage_pcs",    1, 2, 3},
        {"training_alternate2",2, 0, 0},
        {"alternate2_match1",  2, 0, 1},
        {"alternate2_match2",  2, 0, 2},
        {"training_round8",    3, 0, 0},
        {"round8_match_pc2",   3, 0, 2},
        {"all_multipc",        3, 3, 0},
    };
    unsigned calibration_samples = samples < 20 ? samples : 20;
    unsigned chosen_prefix = 1;
    double best_delta = -1.0;

    for (unsigned prefix = 1; prefix + 1 < NODE_COUNT; ++prefix) {
        struct result u = {0}, t = {0};
        for (unsigned sample = 0; sample < calibration_samples; ++sample) {
            run_pc_sample(0, prefix, replays, start, 0, 0, 0, &u);
            run_pc_sample(1, prefix, replays, start, 0, 0, 0, &t);
        }
        double delta = (double)t.l2_events / t.probes -
                       (double)u.l2_events / u.probes;
        if (delta > best_delta) {
            best_delta = delta;
            chosen_prefix = prefix;
        }
    }

    printf("# PC scan replays=%u chosen_prefix=%u calibration_l2_delta=%.4f\n",
           replays, chosen_prefix, best_delta);
    printf("# case train_pattern prefix_pattern trigger_pc u_l1 t_l1 "
           "u_l2 t_l2 l2_delta\n");
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        struct result u = {0}, t = {0};
        for (unsigned sample = 0; sample < samples; ++sample) {
            run_pc_sample(0, chosen_prefix, replays, start,
                          cases[c].train_pattern, cases[c].prefix_pattern,
                          cases[c].trigger_pc, &u);
            run_pc_sample(1, chosen_prefix, replays, start,
                          cases[c].train_pattern, cases[c].prefix_pattern,
                          cases[c].trigger_pc, &t);
        }
        printf("%-20s %13u %14u %10u %6.3f %6.3f %6.3f %6.3f %+8.3f\n",
               cases[c].name, cases[c].train_pattern,
               cases[c].prefix_pattern, cases[c].trigger_pc,
               (double)u.l1_events / u.probes,
               (double)t.l1_events / t.probes,
               (double)u.l2_events / u.probes,
               (double)t.l2_events / t.probes,
               (double)t.l2_events / t.probes -
               (double)u.l2_events / u.probes);
    }

    unsigned sweep_samples = samples < 30 ? samples : 30;
    printf("# PC full-prefix sweep samples_per_prefix=%u\n", sweep_samples);
    printf("# case active_prefixes mean_l2_delta max_l2_delta max_prefix\n");
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        unsigned active = 0, maximum_prefix = 0;
        double sum_delta = 0.0, maximum_delta = -1.0;
        for (unsigned prefix = 1; prefix + 1 < NODE_COUNT; ++prefix) {
            struct result u = {0}, t = {0};
            for (unsigned sample = 0; sample < sweep_samples; ++sample) {
                run_pc_sample(0, prefix, replays, start,
                              cases[c].train_pattern,
                              cases[c].prefix_pattern,
                              cases[c].trigger_pc, &u);
                run_pc_sample(1, prefix, replays, start,
                              cases[c].train_pattern,
                              cases[c].prefix_pattern,
                              cases[c].trigger_pc, &t);
            }
            double delta = (double)t.l2_events / t.probes -
                           (double)u.l2_events / u.probes;
            sum_delta += delta;
            if (delta >= 0.5)
                active++;
            if (delta > maximum_delta) {
                maximum_delta = delta;
                maximum_prefix = prefix;
            }
        }
        printf("%-20s %15u %14.4f %13.4f %10u\n",
               cases[c].name, active,
               sum_delta / (NODE_COUNT - 2), maximum_delta,
               maximum_prefix);
    }
}

static void run_fixed_anchor_sample(int trained, unsigned prelude_count,
                                    unsigned anchor, int dummy_anchor,
                                    unsigned replays, unsigned start,
                                    struct result *result)
{
#if ENABLE_PMU
    struct prefetch_pmu_snapshot before, after;
#endif
    void *anchor_address = dummy_anchor ? dummy_address(200) :
                           node_address(sequence_index(start, anchor));

    cpp_rctx();
    if (trained)
        train_with_pc(replays, start, 0);
    flush_nodes();
    flush_dummies(prelude_count);
    flush(anchor_address);
    mfence();
    for (unsigned i = 0; i < prelude_count; ++i) {
        pc_accesses[0](dummy_address(i));
        nops();
    }
#if ENABLE_PMU
    before = prefetch_pmu_read();
#endif
    pc_accesses[0](anchor_address);
    nops();
    wait_for_prefetch();
#if ENABLE_PMU
    after = prefetch_pmu_read();
    result->l1_events += prefetch_pmu_delta(after.l1_hwprf,
                                             before.l1_hwprf);
    result->l2_events += prefetch_pmu_delta(after.l2_prefetch,
                                             before.l2_prefetch);
#endif
    result->probes++;
}

static void run_fixed_target_sample(int trained, unsigned prelude_count,
                                    unsigned anchor, void *target_address,
                                    unsigned replays, uint64_t threshold,
                                    unsigned start, struct result *result)
{
#if ENABLE_PMU
    struct prefetch_pmu_snapshot before, after;
#endif
    cpp_rctx();
    if (trained)
        train_with_pc(replays, start, 0);
    flush_nodes();
    flush_dummies(prelude_count);
    for (unsigned i = 0; i < prelude_count; ++i) {
        pc_accesses[0](dummy_address(i));
        nops();
    }
    flush(target_address);
    mfence();
#if ENABLE_PMU
    before = prefetch_pmu_read();
#endif
    pc_accesses[0](node_address(sequence_index(start, anchor)));
    nops();
    wait_for_prefetch();
#if ENABLE_PMU
    after = prefetch_pmu_read();
    result->l1_events += prefetch_pmu_delta(after.l1_hwprf,
                                             before.l1_hwprf);
    result->l2_events += prefetch_pmu_delta(after.l2_prefetch,
                                             before.l2_prefetch);
#endif
    {
        uint64_t latency = probe_load(target_address);
        result->probes++;
        result->latency += latency;
        result->hits += latency <= threshold;
    }
}

static void run_activation_scan(unsigned samples, unsigned replays,
                                unsigned start)
{
    unsigned calibration_samples = samples < 20 ? samples : 20;
    unsigned anchor = 0;
    double best_anchor_delta = -1.0;

    for (unsigned prefix = 1; prefix + 1 < NODE_COUNT; ++prefix) {
        struct result u = {0}, t = {0};
        for (unsigned sample = 0; sample < calibration_samples; ++sample) {
            run_pc_sample(0, prefix, replays, start, 0, 0, 0, &u);
            run_pc_sample(1, prefix, replays, start, 0, 0, 0, &t);
        }
        double delta = (double)t.l2_events / t.probes -
                       (double)u.l2_events / u.probes;
        if (delta > best_anchor_delta) {
            best_anchor_delta = delta;
            anchor = prefix - 1;
        }
    }

    unsigned sweep_samples = samples < 50 ? samples : 50;
    unsigned first_active = 0, best_count = 0;
    double best_delta = -1.0;
    printf("# activation scan replays=%u fixed_anchor=%u "
           "anchor_calibration_l2=%.4f\n", replays, anchor,
           best_anchor_delta);
    printf("# prelude_misses u_l2 trained_anchor_l2 delta\n");
    for (unsigned count = 0; count < NODE_COUNT; ++count) {
        struct result u = {0}, t = {0};
        for (unsigned sample = 0; sample < sweep_samples; ++sample) {
            run_fixed_anchor_sample(0, count, anchor, 0, replays, start, &u);
            run_fixed_anchor_sample(1, count, anchor, 0, replays, start, &t);
        }
        double u_l2 = (double)u.l2_events / u.probes;
        double t_l2 = (double)t.l2_events / t.probes;
        double delta = t_l2 - u_l2;
        if (!first_active && u_l2 <= 0.05 && delta >= 0.5)
            first_active = count;
        if (delta > best_delta) {
            best_delta = delta;
            best_count = count;
        }
        if (delta >= 0.2)
            printf("%14u %6.3f %17.3f %+8.3f\n",
                   count, u_l2, t_l2, delta);
    }

    struct result no_train_known = {0}, train_known = {0};
    struct result no_train_dummy = {0}, train_dummy = {0};
    for (unsigned sample = 0; sample < samples; ++sample) {
        run_fixed_anchor_sample(0, best_count, anchor, 0, replays, start,
                                &no_train_known);
        run_fixed_anchor_sample(1, best_count, anchor, 0, replays, start,
                                &train_known);
        run_fixed_anchor_sample(0, best_count, anchor, 1, replays, start,
                                &no_train_dummy);
        run_fixed_anchor_sample(1, best_count, anchor, 1, replays, start,
                                &train_dummy);
    }
    printf("# activation summary first_active=%u best_count=%u\n",
           first_active, best_count);
    printf("# anchor_case avg_l2\n");
    printf("no_train_known %.4f\n", (double)no_train_known.l2_events / samples);
    printf("train_known    %.4f\n", (double)train_known.l2_events / samples);
    printf("no_train_dummy %.4f\n", (double)no_train_dummy.l2_events / samples);
    printf("train_dummy    %.4f\n", (double)train_dummy.l2_events / samples);

    static const unsigned replay_values[] = {0, 1, 2, 3, 4, 5, 6, 8, 12, 16};
    printf("# fixed anchor/prelude training replay sweep\n");
    printf("# replays avg_l1 avg_l2\n");
    for (unsigned r = 0; r < sizeof(replay_values) / sizeof(replay_values[0]);
         ++r) {
        struct result value = {0};
        unsigned replay = replay_values[r];
        for (unsigned sample = 0; sample < samples; ++sample)
            run_fixed_anchor_sample(replay != 0, best_count, anchor, 0,
                                    replay, start, &value);
        printf("%2u %7.4f %7.4f\n", replay,
               (double)value.l1_events / value.probes,
               (double)value.l2_events / value.probes);
    }

    unsigned target_samples = samples < 200 ? samples : 200;
    printf("# fixed anchor target scan samples=%u\n", target_samples);
    printf("# offset target_node u_per_1000 t_per_1000 delta "
           "u_avg_ns t_avg_ns avg_l2\n");
    for (unsigned offset = 1; offset < NODE_COUNT; ++offset) {
        struct result u = {0}, t = {0};
        for (unsigned sample = 0; sample < target_samples; ++sample) {
            void *target_address = node_address(
                sequence_index(start, anchor + offset));
            run_fixed_target_sample(0, best_count, anchor, target_address,
                                    replays,
                                    80, start, &u);
            run_fixed_target_sample(1, best_count, anchor, target_address,
                                    replays,
                                    80, start, &t);
        }
        double up = 1000.0 * u.hits / u.probes;
        double tp = 1000.0 * t.hits / t.probes;
        if (tp - up >= 20.0)
            printf("%2u %11zu %10.2f %10.2f %+8.2f %8.2f %8.2f %7.3f\n",
                   offset, sequence_index(start, anchor + offset),
                   up, tp, tp - up,
                   (double)u.latency / u.probes,
                   (double)t.latency / t.probes,
                   (double)t.l2_events / t.probes);
    }

    size_t anchor_index = sequence_index(start, anchor);
    size_t anchor_line = nodes[anchor_index].line;
    uint8_t *anchor_page = mapping + nodes[anchor_index].page * PAGE_SIZE;
    printf("# fixed anchor spatial-line scan anchor_line=%zu samples=%u\n",
           anchor_line, target_samples);
    printf("# line relative_line u_per_1000 t_per_1000 delta "
           "u_avg_ns t_avg_ns avg_l2\n");
    for (size_t line = 0; line < LINES_PER_PAGE; ++line) {
        if (line == anchor_line)
            continue;
        struct result u = {0}, t = {0};
        void *target_address = anchor_page + line * LINE_SIZE;
        for (unsigned sample = 0; sample < target_samples; ++sample) {
            run_fixed_target_sample(0, best_count, anchor, target_address,
                                    replays, 80, start, &u);
            run_fixed_target_sample(1, best_count, anchor, target_address,
                                    replays, 80, start, &t);
        }
        double up = 1000.0 * u.hits / u.probes;
        double tp = 1000.0 * t.hits / t.probes;
        if (tp - up >= 20.0)
            printf("%4zu %+13zd %10.2f %10.2f %+8.2f %8.2f %8.2f %7.3f\n",
                   line, (ssize_t)line - (ssize_t)anchor_line,
                   up, tp, tp - up,
                   (double)u.latency / u.probes,
                   (double)t.latency / t.probes,
                   (double)t.l2_events / t.probes);
    }
}

static void run_replay_prefix_grid(unsigned samples, uint64_t threshold,
                                   unsigned start)
{
    static const unsigned replay_values[] = {1, 2, 3, 4, 5, 6, 8, 12, 16};
    enum { REPLAY_VALUE_COUNT = sizeof(replay_values) / sizeof(replay_values[0]) };
    struct result untrained[REPLAY_VALUE_COUNT][NODE_COUNT];
    struct result trained[REPLAY_VALUE_COUNT][NODE_COUNT];

    memset(untrained, 0, sizeof(untrained));
    memset(trained, 0, sizeof(trained));
    for (unsigned r = 0; r < REPLAY_VALUE_COUNT; ++r) {
        for (unsigned prefix = 1; prefix < NODE_COUNT; ++prefix) {
            for (unsigned sample = 0; sample < samples; ++sample) {
                if (sample & 1) {
                    run_sample(1, prefix, replay_values[r], threshold, start,
                               0, &trained[r][prefix]);
                    run_sample(0, prefix, replay_values[r], threshold, start,
                               0, &untrained[r][prefix]);
                } else {
                    run_sample(0, prefix, replay_values[r], threshold, start,
                               0, &untrained[r][prefix]);
                    run_sample(1, prefix, replay_values[r], threshold, start,
                               0, &trained[r][prefix]);
                }
            }
        }
    }

    printf("# replay-prefix grid samples=%u threshold_ns=%llu start=%u\n",
           samples, (unsigned long long)threshold, start);
    printf("# replays first_50pct first_80pct first_l2_delta_0.5 "
           "best_prefix best_t_per_1000 best_u_per_1000 best_delta "
           "best_l2_prefix best_l2_delta\n");
    for (unsigned r = 0; r < REPLAY_VALUE_COUNT; ++r) {
        unsigned first50 = 0, first80 = 0, best_prefix = 0;
        unsigned first_l2 = 0, best_l2_prefix = 0;
        double best_delta = -1000.0, best_t = 0.0, best_u = 0.0;
        double best_l2_delta = -1000.0;
        for (unsigned prefix = 1; prefix < NODE_COUNT; ++prefix) {
            double up = 1000.0 * untrained[r][prefix].hits /
                        untrained[r][prefix].probes;
            double tp = 1000.0 * trained[r][prefix].hits /
                        trained[r][prefix].probes;
            double delta = tp - up;
            double u_l2 = (double)untrained[r][prefix].l2_events /
                          untrained[r][prefix].probes;
            double t_l2 = (double)trained[r][prefix].l2_events /
                          trained[r][prefix].probes;
            double l2_delta = t_l2 - u_l2;
            if (!first50 && up <= 50.0 && tp >= 500.0)
                first50 = prefix;
            if (!first80 && up <= 50.0 && tp >= 800.0)
                first80 = prefix;
            if (!first_l2 && u_l2 <= 0.05 && l2_delta >= 0.5)
                first_l2 = prefix;
            if (delta > best_delta) {
                best_delta = delta;
                best_prefix = prefix;
                best_t = tp;
                best_u = up;
            }
            if (l2_delta > best_l2_delta) {
                best_l2_delta = l2_delta;
                best_l2_prefix = prefix;
            }
        }
        printf("%2u %11u %11u %18u %11u %15.2f %15.2f %+10.2f "
               "%14u %+13.4f\n",
               replay_values[r], first50, first80, first_l2, best_prefix,
               best_t, best_u, best_delta, best_l2_prefix, best_l2_delta);
    }
}

int main(int argc, char **argv)
{
    unsigned samples = 100;
    unsigned replays = 8;
    uint64_t threshold = 80;
    unsigned sequence_start = 0;
    unsigned context_mode = 0;
    struct result untrained[3][NODE_COUNT] = {{{0}}};
    struct result trained[3][NODE_COUNT] = {{{0}}};

    if (argc == 6) {
        samples = strtoul(argv[1], NULL, 0);
        replays = strtoul(argv[2], NULL, 0);
        threshold = strtoull(argv[3], NULL, 0);
        sequence_start = strtoul(argv[4], NULL, 0) % NODE_COUNT;
        context_mode = strtoul(argv[5], NULL, 0);
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [samples training_replays threshold_ns start mode]\n",
                argv[0]);
        return 1;
    }

    timer_frequency = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(timer_frequency));
    initialize_nodes();
    if (context_mode > 7) {
        fprintf(stderr, "mode must be 0=exact, 1=reverse, 2=unrelated, 3=all, 4=grid, 5=targets, 6=pcs, or 7=activation\n");
        return 1;
    }
    mapping = mmap(NULL, MAPPING_PAGES * PAGE_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        return 1;
    }
    dummy_mapping = mmap(NULL, MAPPING_PAGES * PAGE_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_mapping == MAP_FAILED) {
        fprintf(stderr, "dummy mmap: %s\n", strerror(errno));
        return 1;
    }
    memset(mapping, 0xa5, MAPPING_PAGES * PAGE_SIZE);
    memset(dummy_mapping, 0x5a, MAPPING_PAGES * PAGE_SIZE);
    flush_nodes();
    if (pmu_setup() != 0)
        return 2;

    if (context_mode == 4) {
        run_replay_prefix_grid(samples, threshold, sequence_start);
        return 0;
    }
    if (context_mode == 5) {
        run_target_scan(samples, replays, threshold, sequence_start);
        return 0;
    }
    if (context_mode == 6) {
        run_pc_scan(samples, replays, sequence_start);
        return 0;
    }
    if (context_mode == 7) {
        run_activation_scan(samples, replays, sequence_start);
        return 0;
    }

    unsigned first_mode = context_mode == 3 ? 0 : context_mode;
    unsigned last_mode = context_mode == 3 ? 2 : context_mode;
    for (unsigned mode = first_mode; mode <= last_mode; ++mode) {
      for (unsigned prefix = 1; prefix < NODE_COUNT; ++prefix) {
        for (unsigned sample = 0; sample < samples; ++sample) {
            /* Alternate order to reduce temperature/frequency bias. */
            if (sample & 1) {
                run_sample(1, prefix, replays, threshold, sequence_start,
                           mode, &trained[mode][prefix]);
                run_sample(0, prefix, replays, threshold, sequence_start,
                           mode, &untrained[mode][prefix]);
            } else {
                run_sample(0, prefix, replays, threshold, sequence_start,
                           mode, &untrained[mode][prefix]);
                run_sample(1, prefix, replays, threshold, sequence_start,
                           mode, &trained[mode][prefix]);
            }
        }
      }
    }

    printf("# X925 CMC prefix-replay experiment\n");
    printf("# samples=%u replays=%u threshold_ns=%llu wait_nops=%d start=%u mode=%u\n",
           samples, replays, (unsigned long long)threshold, WAIT_NOPS,
           sequence_start, context_mode);
    for (unsigned mode = first_mode; mode <= last_mode; ++mode) {
      printf("# mode=%u (%s)\n", mode,
             mode == 0 ? "exact" : mode == 1 ? "reverse" : "unrelated");
      printf("# prefix target_page target_line u_hits t_hits u_per_1000 "
             "t_per_1000 delta u_avg_ns t_avg_ns u_l1 t_l1 u_l2 t_l2\n");
      for (unsigned prefix = 1; prefix < NODE_COUNT; ++prefix) {
        struct result *u = &untrained[mode][prefix];
        struct result *t = &trained[mode][prefix];
        double up = 1000.0 * u->hits / u->probes;
        double tp = 1000.0 * t->hits / t->probes;
        size_t target = sequence_index(sequence_start, prefix);
        printf("%2u %3zu %2zu %5llu %5llu %10.2f %10.2f %+8.2f "
               "%8.2f %8.2f %6.3f %6.3f %6.3f %6.3f\n",
               prefix, nodes[target].page, nodes[target].line,
               (unsigned long long)u->hits,
               (unsigned long long)t->hits, up, tp, tp - up,
               (double)u->latency / u->probes,
               (double)t->latency / t->probes,
               (double)u->l1_events / u->probes,
               (double)t->l1_events / t->probes,
               (double)u->l2_events / u->probes,
               (double)t->l2_events / t->probes);
      }
    }
#if ENABLE_PMU
    close(pmu_fd);
#endif
    return 0;
}
