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
#ifndef PMU_CORE_X925
#define PMU_CORE_X925 1
#endif
#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 1000
#endif
#ifndef RANDOMIZE_TARGET_PAGES
#define RANDOMIZE_TARGET_PAGES 0
#endif
#ifndef FLUSH_ALL_ARRAY_TARGETS
#define FLUSH_ALL_ARRAY_TARGETS 1
#endif
#ifndef FLUSH_NOISE_LINES
#define FLUSH_NOISE_LINES 0
#endif

#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"

/*
 * This is a DMP/AoP existence test derived from two complementary tests.
 *
 * GoFetch's no_deref test loads a cache line containing pointer-looking
 * values, without architecturally dereferencing them, then tests whether the
 * pointed-to line became cached.  FetchBench's pointer-array test uses a
 * sparse array of pointer targets and compares it with a non-pointer control.
 *
 * Here every source line contains eight independently placed target pointers.
 * The source and target are in one allocation (< 4 GiB), satisfying Apple's
 * documented same-4-GiB restriction while remaining a valid test on other
 * implementations.  The target pages/lines are randomized, so a hit cannot
 * be explained by next-line, stride, stream, or spatial locality prefetching.
 */
#define POINTERS_PER_SOURCE 8
#define SOURCE_PAGE_COUNT 256
#define TARGET_PAGE_COUNT (SOURCE_PAGE_COUNT * POINTERS_PER_SOURCE)
#define MAPPING_PAGES (SOURCE_PAGE_COUNT + TARGET_PAGE_COUNT)
#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define SOURCE_LINE 8
#define DEFAULT_ROUNDS 4096
#define DEFAULT_THRESHOLD_NS 120
#define LAYOUT_SEED 0x9e3779b97f4a7c15ULL

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

enum dmp_case {
    /* No source access: isolates cache maintenance, probing, and threshold. */
    CASE_IDLE_CONTROL,
    CASE_IDLE_AHEAD2,
    CASE_NONPOINTER_CONTROL,
    /* Same non-pointer source access, but break the target-page access stream. */
    CASE_NONPOINTER_SHUFFLED,
    CASE_POINTER_CONTENT,
    /* Same pointer-array source loads, with zero or one architectural dereference. */
    CASE_POINTER_ARRAY_LOADS_ONLY,
    CASE_POINTER_ARRAY_ONE_DEREFERENCE,
    CASE_POINTER_ARRAY,
    /* Destination probes for identifying dereference-triggered prefetches. */
    CASE_POINTER_ARRAY_FIRST_NEXTLINE,
    CASE_POINTER_ARRAY_SECOND_NEXTLINE,
    CASE_POINTER_ARRAY_REMOTE_TARGET,
    CASE_POINTER_ARRAY_AHEAD2,
    CASE_POINTER_ARRAY_AHEAD3,
    CASE_POINTER_ARRAY_AHEAD5,
    CASE_POINTER_ARRAY_AHEAD6,
    CASE_POINTER_ARRAY_LOADS_ONLY_AHEAD2,
    CASE_POINTER_ARRAY_ONE_DEREFERENCE_AHEAD2,
};

struct target {
    size_t page;
    size_t line;
};

struct slot_stats {
    uint64_t probes;
    uint64_t hits;
    uint64_t latency_ns;
    uint64_t pmu_samples;
    uint64_t l1_events;
    uint64_t l2_events;
};

struct case_stats {
    uint64_t samples;
    uint64_t l1_events;
    uint64_t l2_events;
    struct slot_stats slots[POINTERS_PER_SOURCE];
};

static uint8_t *mapping;
static size_t mapping_size;
static struct target targets[SOURCE_PAGE_COUNT][POINTERS_PER_SOURCE];
static size_t source_order[SOURCE_PAGE_COUNT];
static size_t target_page_order[TARGET_PAGE_COUNT];
static uint64_t timer_freq_hz;
static volatile uint64_t sink;
static uint8_t maintenance_noise[POINTERS_PER_SOURCE * LINE_SIZE]
    __attribute__((aligned(LINE_SIZE)));

#if ENABLE_PMU
static int pmu_device_fd = -1;
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

static inline __attribute__((always_inline)) void wait_for_prefetch(void)
{
    /* The PMU window contains only the activation accesses and this wait. */
    asm volatile(".rept " STRINGIFY(PREFETCH_WAIT_NOPS) "\n\t"
                 "nop\n\t"
                 ".endr\n\t"
                 ::: "memory");
}

/*
 * This deliberately reads a source word but never uses it as an address.
 * A hit in CASE_POINTER_CONTENT therefore demonstrates content-dependent
 * hardware behavior, not an architectural pointer dereference.
 */
__attribute__((noinline)) static uint64_t dmp_content_load(void *addr)
{
    uint64_t value;

    asm volatile("ldr %0, [%1]" : "=r"(value) : "r"(addr) : "memory");
    return value;
}

/* FetchBench's pointer-array training path: load a pointer then dereference. */
__attribute__((noinline)) static void dmp_pointer_dereference(void *addr)
{
    asm volatile("ldrb w9, [%0]" :: "r"(addr) : "w9", "memory");
}

static uint8_t *source_addr(size_t source_index)
{
    return mapping + source_index * PAGE_SIZE + SOURCE_LINE * LINE_SIZE;
}

static uint8_t *target_addr(size_t source_index, size_t slot)
{
    const struct target *target = &targets[source_index][slot];

    return mapping + target->page * PAGE_SIZE + target->line * LINE_SIZE;
}

static void initialize_layout(void)
{
    uint64_t rng = LAYOUT_SEED;

    for (size_t index = 0; index < TARGET_PAGE_COUNT; index++)
        target_page_order[index] = index;
#if RANDOMIZE_TARGET_PAGES
    for (size_t index = TARGET_PAGE_COUNT - 1; index > 0; index--) {
        size_t swap_index = (size_t)(splitmix64_next(&rng) % (index + 1));
        size_t temporary = target_page_order[index];

        target_page_order[index] = target_page_order[swap_index];
        target_page_order[swap_index] = temporary;
    }
#endif

    for (size_t source_index = 0; source_index < SOURCE_PAGE_COUNT;
         source_index++) {
        for (size_t slot = 0; slot < POINTERS_PER_SOURCE; slot++) {
            struct target *target = &targets[source_index][slot];
            uint8_t *target_pointer;

            target->page = SOURCE_PAGE_COUNT +
                           target_page_order[source_index * POINTERS_PER_SOURCE + slot];
            /* Keep line zero for TLB warming, well away from the target. */
            target->line = 8 +
                (size_t)(splitmix64_next(&rng) % (LINES_PER_PAGE - 9));
            target_pointer = target_addr(source_index, slot);
            *(uint64_t *)target_pointer = splitmix64_next(&rng);

            if (((uintptr_t)source_addr(source_index) >> 32) !=
                ((uintptr_t)target_pointer >> 32)) {
                fprintf(stderr,
                        "source and target do not share a 4-GiB region\n");
                exit(EXIT_FAILURE);
            }
        }
    }

    /* A true permutation, not a modular fixed-stride walk. */
    for (size_t i = 0; i < SOURCE_PAGE_COUNT; i++)
        source_order[i] = i;
    for (size_t i = SOURCE_PAGE_COUNT - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(&rng) % (i + 1));
        size_t temporary = source_order[i];

        source_order[i] = source_order[j];
        source_order[j] = temporary;
    }

    /* Populate translations without touching a line that will be probed. */
    for (size_t page = 0; page < MAPPING_PAGES; page++)
        mLoad_inline(mapping + page * PAGE_SIZE);
    mfence();
}

static void set_source_payload(size_t source_index, int pointers)
{
    uint64_t *source = (uint64_t *)source_addr(source_index);

    for (size_t slot = 0; slot < POINTERS_PER_SOURCE; slot++) {
        if (pointers) {
            source[slot] = (uint64_t)(uintptr_t)target_addr(source_index, slot);
        } else {
            /* Deliberately non-canonical AArch64 values, never valid pointers. */
            source[slot] = 0xdeadc0de00000000ULL ^
                           ((uint64_t)source_index << 8) ^ slot;
        }
    }
    mfence();
}

static void prepare_sample(enum dmp_case which, size_t source_index,
                           size_t probe_slot, int pointers)
{
    uint8_t *source = source_addr(source_index);
    uint8_t *target = target_addr(source_index, probe_slot);

    set_source_payload(source_index, pointers);
    flush(source);
    flush(target);
    if (which >= CASE_POINTER_ARRAY_LOADS_ONLY) {
        size_t first = (probe_slot + POINTERS_PER_SOURCE - 2) %
                       POINTERS_PER_SOURCE;
        size_t second = (first + 1) % POINTERS_PER_SOURCE;

#if FLUSH_ALL_ARRAY_TARGETS
        /* Every potential pointer-array destination starts cold for PMU/cache. */
        for (size_t slot = 0; slot < POINTERS_PER_SOURCE; slot++)
            flush(target_addr(source_index, slot));
#endif
        for (size_t noise = 0; noise < FLUSH_NOISE_LINES; noise++)
            flush(maintenance_noise + (noise % POINTERS_PER_SOURCE) * LINE_SIZE);

        /* Ensure an adjacent-line hit can only be created after dereference. */
        flush(target_addr(source_index, first));
        flush(target_addr(source_index, first) + LINE_SIZE);
        flush(target_addr(source_index, second));
        flush(target_addr(source_index, second) + LINE_SIZE);
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
           "counter4=0x8154 counter5=0x8285 (HWPRF_OR_PRFM)\n");
#endif
    if (ioctl(pmu_device_fd, PREFETCH_PMU_IOC_REARM) != 0) {
        fprintf(stderr, "PMU rearm ioctl failed: %s\n", strerror(errno));
        close(pmu_device_fd);
        pmu_device_fd = -1;
        return -1;
    }
    return 0;
}

static void pmu_cleanup(void)
{
    if (pmu_device_fd >= 0)
        close(pmu_device_fd);
}
#endif

static const char *case_name(enum dmp_case which)
{
    switch (which) {
    case CASE_IDLE_CONTROL:
        return "idle_no_activation";
    case CASE_IDLE_AHEAD2:
        return "idle_ahead2_no_activation";
    case CASE_NONPOINTER_CONTROL:
        return "nonpointer_control";
    case CASE_NONPOINTER_SHUFFLED:
        return "nonpointer_shuffled_targets";
    case CASE_POINTER_CONTENT:
        return "pointer_content_no_deref";
    case CASE_POINTER_ARRAY_LOADS_ONLY:
        return "pointer_array_loads_only";
    case CASE_POINTER_ARRAY_ONE_DEREFERENCE:
        return "pointer_array_one_dereference";
    case CASE_POINTER_ARRAY:
        return "pointer_array_two_dereferences";
    case CASE_POINTER_ARRAY_FIRST_NEXTLINE:
        return "pointer_array_first_deref_nextline";
    case CASE_POINTER_ARRAY_SECOND_NEXTLINE:
        return "pointer_array_second_deref_nextline";
    case CASE_POINTER_ARRAY_REMOTE_TARGET:
        return "pointer_array_ahead4";
    case CASE_POINTER_ARRAY_AHEAD2:
        return "pointer_array_ahead2";
    case CASE_POINTER_ARRAY_AHEAD3:
        return "pointer_array_ahead3";
    case CASE_POINTER_ARRAY_AHEAD5:
        return "pointer_array_ahead5";
    case CASE_POINTER_ARRAY_AHEAD6:
        return "pointer_array_ahead6";
    case CASE_POINTER_ARRAY_LOADS_ONLY_AHEAD2:
        return "pointer_array_loads_only_ahead2";
    case CASE_POINTER_ARRAY_ONE_DEREFERENCE_AHEAD2:
        return "pointer_array_one_dereference_ahead2";
    }
    return "unknown";
}

static void activate_case(enum dmp_case which, size_t source_index,
                          size_t probe_slot)
{
    uint8_t *source = source_addr(source_index);

    if (which == CASE_IDLE_CONTROL || which == CASE_IDLE_AHEAD2)
        return;

    if (which == CASE_NONPOINTER_CONTROL ||
        which == CASE_NONPOINTER_SHUFFLED) {
        sink ^= dmp_content_load(source);
        return;
    }

    if (which == CASE_POINTER_CONTENT) {
        sink ^= dmp_content_load(source);
        return;
    }

    /*
     * Train on the two pointer-array entries immediately preceding the probe
     * slot, wrapping within this cache line. The probed pointer itself is
     * never architecturally dereferenced.
     */
    size_t first = (probe_slot + POINTERS_PER_SOURCE - 2) %
                   POINTERS_PER_SOURCE;
    size_t second = (first + 1) % POINTERS_PER_SOURCE;
    uint64_t pointer = dmp_content_load(source + first * sizeof(uint64_t));

    if (which == CASE_POINTER_ARRAY_LOADS_ONLY ||
        which == CASE_POINTER_ARRAY_LOADS_ONLY_AHEAD2) {
        sink ^= dmp_content_load(source + second * sizeof(uint64_t));
        return;
    }

    dmp_pointer_dereference((void *)(uintptr_t)pointer);
    pointer = dmp_content_load(source + second * sizeof(uint64_t));
    if (which == CASE_POINTER_ARRAY_ONE_DEREFERENCE ||
        which == CASE_POINTER_ARRAY_ONE_DEREFERENCE_AHEAD2) {
        sink ^= pointer;
        return;
    }

    dmp_pointer_dereference((void *)(uintptr_t)pointer);
    sink ^= pointer;
}

static void print_case(const struct case_stats *stats, enum dmp_case which)
{
    const char *name = case_name(which);

    printf("\n[%s]\n", name);
    printf("# samples=%llu\n", (unsigned long long)stats->samples);
#if ENABLE_PMU
    if (pmu_ready) {
        double denominator = stats->samples == 0 ? 1.0 :
                             (double)stats->samples;

        printf("# PMU %-24s per_sample=%.4f\n", "l1d_cache_hwprf",
               (double)stats->l1_events / denominator);
        printf("# PMU %-24s per_sample=%.4f\n", "l2d_cache_hwprf",
               (double)stats->l2_events / denominator);
    }
#endif
#if ENABLE_CACHE_PROBE
    printf("# slot\tprobes\thits\tper_1000\tavg_latency_ns");
#if ENABLE_PMU
    if (pmu_ready)
        printf("\tavg_l1_pmu\tavg_l2_pmu");
#endif
    printf("\n");
    for (size_t slot = 0; slot < POINTERS_PER_SOURCE; slot++) {
        const struct slot_stats *slot_stats = &stats->slots[slot];
        uint64_t per_1000 = slot_stats->probes == 0 ? 0 :
            slot_stats->hits * 1000 / slot_stats->probes;
        uint64_t average = slot_stats->probes == 0 ? 0 :
            slot_stats->latency_ns / slot_stats->probes;

        printf("%zu\t%5llu\t%5llu\t%4llu\t\t%5llu",
               slot,
               (unsigned long long)slot_stats->probes,
               (unsigned long long)slot_stats->hits,
               (unsigned long long)per_1000,
               (unsigned long long)average);
#if ENABLE_PMU
        if (pmu_ready) {
            double slot_denominator = slot_stats->pmu_samples == 0 ? 1.0 :
                (double)slot_stats->pmu_samples;

            printf("\t%10.3f\t%10.3f",
                   (double)slot_stats->l1_events / slot_denominator,
                   (double)slot_stats->l2_events / slot_denominator);
        }
#endif
        printf("\n");
    }
#endif
}

static void run_case(enum dmp_case which, int rounds, uint64_t threshold_ns)
{
    struct case_stats stats = {0};
    int pointers = which == CASE_POINTER_CONTENT ||
                   which == CASE_POINTER_ARRAY_LOADS_ONLY ||
                   which == CASE_POINTER_ARRAY_ONE_DEREFERENCE ||
                   which == CASE_POINTER_ARRAY ||
                   which == CASE_POINTER_ARRAY_FIRST_NEXTLINE ||
                   which == CASE_POINTER_ARRAY_SECOND_NEXTLINE ||
                   which == CASE_POINTER_ARRAY_REMOTE_TARGET ||
                   which == CASE_POINTER_ARRAY_AHEAD2 ||
                   which == CASE_POINTER_ARRAY_AHEAD3 ||
                   which == CASE_POINTER_ARRAY_AHEAD5 ||
                   which == CASE_POINTER_ARRAY_AHEAD6 ||
                   which == CASE_POINTER_ARRAY_LOADS_ONLY_AHEAD2 ||
                   which == CASE_POINTER_ARRAY_ONE_DEREFERENCE_AHEAD2;

#if !ENABLE_CACHE_PROBE
    (void)threshold_ns;
#endif
    for (int round = 0; round < rounds; round++) {
        size_t source_index = (size_t)round % SOURCE_PAGE_COUNT;
        size_t probe_slot = ((size_t)round / SOURCE_PAGE_COUNT) %
                            POINTERS_PER_SOURCE;
        uint8_t *target;

        /* Each epoch visits every target once in a non-affine random order. */
        if (which == CASE_NONPOINTER_SHUFFLED)
            source_index = source_order[(size_t)round % SOURCE_PAGE_COUNT];
        target = target_addr(source_index, probe_slot);
        if (which == CASE_POINTER_ARRAY_FIRST_NEXTLINE) {
            size_t first = (probe_slot + POINTERS_PER_SOURCE - 2) %
                           POINTERS_PER_SOURCE;
            target = target_addr(source_index, first) + LINE_SIZE;
        } else if (which == CASE_POINTER_ARRAY_SECOND_NEXTLINE) {
            size_t second = (probe_slot + POINTERS_PER_SOURCE - 1) %
                            POINTERS_PER_SOURCE;
            target = target_addr(source_index, second) + LINE_SIZE;
        } else if (which == CASE_POINTER_ARRAY_REMOTE_TARGET ||
                   which == CASE_POINTER_ARRAY_AHEAD2 ||
                   which == CASE_POINTER_ARRAY_AHEAD3 ||
                   which == CASE_POINTER_ARRAY_AHEAD5 ||
                   which == CASE_POINTER_ARRAY_AHEAD6 ||
                   which == CASE_POINTER_ARRAY_LOADS_ONLY_AHEAD2 ||
                   which == CASE_POINTER_ARRAY_ONE_DEREFERENCE_AHEAD2) {
            size_t ahead = 4;

            if (which == CASE_IDLE_AHEAD2 ||
                which == CASE_POINTER_ARRAY_AHEAD2 ||
                which == CASE_POINTER_ARRAY_LOADS_ONLY_AHEAD2 ||
                which == CASE_POINTER_ARRAY_ONE_DEREFERENCE_AHEAD2)
                ahead = 2;
            else if (which == CASE_POINTER_ARRAY_AHEAD3)
                ahead = 3;
            else if (which == CASE_POINTER_ARRAY_AHEAD5)
                ahead = 5;
            else if (which == CASE_POINTER_ARRAY_AHEAD6)
                ahead = 6;
            target = target_addr(source_index,
                                 (probe_slot + ahead - 1) % POINTERS_PER_SOURCE);
        }
#if ENABLE_CACHE_PROBE || ENABLE_PMU
        struct slot_stats *slot_stats = &stats.slots[probe_slot];
#endif
#if ENABLE_PMU
        struct prefetch_pmu_snapshot before = {0};
        struct prefetch_pmu_snapshot after = {0};
#endif

        /* Clear per-context predictor state and start from cold source/target. */
        cpp_rctx();
        prepare_sample(which, source_index, probe_slot, pointers);

#if ENABLE_PMU
        if (pmu_ready)
            before = prefetch_pmu_read();
#endif
        activate_case(which, source_index, probe_slot);
        wait_for_prefetch();
#if ENABLE_PMU
        if (pmu_ready) {
            uint64_t l1_delta;
            uint64_t l2_delta;

            after = prefetch_pmu_read();
            l1_delta = prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
            l2_delta = prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
            stats.l1_events += l1_delta;
            stats.l2_events += l2_delta;
            slot_stats->pmu_samples++;
            slot_stats->l1_events += l1_delta;
            slot_stats->l2_events += l2_delta;
        }
#endif

#if ENABLE_CACHE_PROBE
        {
            uint64_t latency = timed_reload_ns(target);

            slot_stats->latency_ns += latency;
            slot_stats->probes++;
            if (latency <= threshold_ns)
                slot_stats->hits++;
        }
#else
        (void)target;
#endif
        stats.samples++;
    }

    print_case(&stats, which);
}

static void print_usage(const char *program)
{
    fprintf(stderr, "usage: %s [rounds threshold_ns]\n", program);
}

int main(int argc, char **argv)
{
    int rounds = DEFAULT_ROUNDS;
    uint64_t threshold_ns = DEFAULT_THRESHOLD_NS;
    int result = 0;

    if (argc != 1 && argc != 3) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 3) {
        rounds = atoi(argv[1]);
        threshold_ns = strtoull(argv[2], NULL, 0);
    }
    if (rounds <= 0 || threshold_ns == 0) {
        print_usage(argv[0]);
        return 1;
    }

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
    initialize_layout();

    printf("# DMP/AoP existence test (GoFetch no-deref + FetchBench pointer array)\n");
    printf("# rounds=%d threshold_ns=%llu sources=%d pointers_per_source=%d "
           "mapping_pages=%d wait_nops=%d cpp_rctx=each_sample\n",
           rounds, (unsigned long long)threshold_ns, SOURCE_PAGE_COUNT,
           POINTERS_PER_SOURCE, MAPPING_PAGES, PREFETCH_WAIT_NOPS);
    printf("# source and pointer target share one <4-GiB mapping; targets are "
           "sparse randomized cache lines.\n");
    printf("# DMP evidence: pointer_content_no_deref has target cache hits above "
           "nonpointer_control; pointer_array_two_dereferences is supporting evidence.\n");
    printf("# timer=cntvct_el0 cntfrq_el0=%lluHz enable_pmu=%d "
           "enable_cache_probe=%d\n",
           (unsigned long long)timer_freq_hz, ENABLE_PMU, ENABLE_CACHE_PROBE);

#if ENABLE_PMU
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        fprintf(stderr, "# direct PMU counter setup failed; cache probe still runs\n");
        result = 2;
    }
#else
    printf("# PMU disabled by ENABLE_PMU=0\n");
#endif

    run_case(CASE_IDLE_CONTROL, rounds, threshold_ns);
    run_case(CASE_IDLE_AHEAD2, rounds, threshold_ns);
    run_case(CASE_NONPOINTER_CONTROL, rounds, threshold_ns);
    run_case(CASE_NONPOINTER_SHUFFLED, rounds, threshold_ns);
    run_case(CASE_POINTER_CONTENT, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_LOADS_ONLY, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_ONE_DEREFERENCE, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_FIRST_NEXTLINE, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_SECOND_NEXTLINE, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_REMOTE_TARGET, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_AHEAD2, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_AHEAD3, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_AHEAD5, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_AHEAD6, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_LOADS_ONLY_AHEAD2, rounds, threshold_ns);
    run_case(CASE_POINTER_ARRAY_ONE_DEREFERENCE_AHEAD2, rounds, threshold_ns);

#if ENABLE_PMU
    pmu_cleanup();
#endif
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
