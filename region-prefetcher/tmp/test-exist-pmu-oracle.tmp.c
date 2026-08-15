#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#ifndef __aarch64__
#error "This test requires AArch64 EL0 load and cache-maintenance instructions."
#endif

#include "../store-stride-prefetcher/until.h"

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef TRAIN_REGIONS
#define TRAIN_REGIONS 32
#endif

#ifndef REGION_POOL_PAGES
#define REGION_POOL_PAGES 257
#endif

#ifndef EXTRA_REGIONS
#define EXTRA_REGIONS 0
#endif

#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 1000
#endif

#ifndef PREFETCH_WAIT_NS
#define PREFETCH_WAIT_NS 10000
#endif

#ifndef ENABLE_CACHE_PROBE
#define ENABLE_CACHE_PROBE 1
#endif

#ifndef ENABLE_PMU
#define ENABLE_PMU 1
#endif

#ifndef CACHE_HIT_THRESHOLD_TICKS
#define CACHE_HIT_THRESHOLD_TICKS 200
#endif


#ifndef PMU_CORE_X925
#define PMU_CORE_X925 1
#endif
#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif
#ifndef PMU_PRINT_ALL
#define PMU_PRINT_ALL 1
#endif
#ifndef PMU_PRINT_EVENT
#define PMU_PRINT_EVENT NULL
#endif
#define PMU_WINDOW_NAME "training-plus-trigger-per-round"
#include "../store-stride-prefetcher/pmu.h"

#if REGION_POOL_PAGES <= (TRAIN_REGIONS + EXTRA_REGIONS)
#error "REGION_POOL_PAGES must cover training, extra, and target regions"
#endif

#define LINES_PER_REGION (PAGE_SIZE / LINE_SIZE)
#define REGION_POOL_SIZE ((size_t)REGION_POOL_PAGES * PAGE_SIZE)
#define STORE_NOISE_PAGES 4
#define STORE_NOISE_SIZE ((size_t)STORE_NOISE_PAGES * PAGE_SIZE)

/*
 * An intentionally non-strided spatial footprint.  Line 4 is the trigger;
 * the remaining deltas are irregular so a simple next-line or stride
 * prefetcher should not explain all six predicted lines.
 */
#ifndef FOOTPRINT_LINES
#define FOOTPRINT_LINES {4, 17, 6, 27, 13, 1, 15}
#endif
#ifndef TRIGGER_LINES
#define TRIGGER_LINES {4, 17}
#endif

static const uint8_t footprint[] = FOOTPRINT_LINES;
static const uint8_t trigger_lines[] = TRIGGER_LINES;

static uint8_t *region_pool;
static uint8_t *store_noise;

/* Training and treatment trigger execute this exact same load PC. */
__attribute__((noinline)) static void region_pattern_load(void *addr)
{
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

/* The control trigger has no matching multi-line footprint history. */
__attribute__((noinline)) static void control_trigger_load(void *addr)
{
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

#if EXTRA_REGIONS > 0
/* A third PC creates region turnover without adding to the learned pattern. */
__attribute__((noinline)) static void extra_region_load(void *addr)
{
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}
#endif

static void flush_page(uint8_t *page)
{
    for (size_t line = 0; line < LINES_PER_REGION; line++) {
        flush(page + line * LINE_SIZE);
    }
    mfence();
}

/*
 * 73 is coprime with the 257-page pool.  This visits pages in a permutation
 * instead of ascending virtual-address order, suppressing page-stride and
 * adjacent-region explanations for the result.
 */
static size_t page_index(uint64_t round, size_t position)
{
    return (size_t)((round * 151u + position * 73u) % REGION_POOL_PAGES);
}

static void train_repeated_footprint(uint64_t round)
{
    for (size_t region = 0; region < TRAIN_REGIONS; region++) {
        uint8_t *page = region_pool + page_index(round, region) * PAGE_SIZE;

        flush_page(page);
        for (size_t i = 0; i < sizeof(footprint) / sizeof(footprint[0]); i++) {
            region_pattern_load(page + (size_t)footprint[i] * LINE_SIZE);
            nops();
        }
        mfence();
    }
}

static void touch_extra_regions(uint64_t round)
{
#if EXTRA_REGIONS > 0
    for (size_t region = 0; region < EXTRA_REGIONS; region++) {
        size_t position = TRAIN_REGIONS + region;
        uint8_t *page = region_pool + page_index(round, position) * PAGE_SIZE;

        flush_page(page);
        extra_region_load(page);
        nops();
    }
#else
    (void)round;
#endif
}

typedef void (*trigger_fn_t)(void *);

static int line_is_in(const uint8_t *lines, size_t count, size_t line)
{
    for (size_t i = 0; i < count; i++) {
        if (lines[i] == line) {
            return 1;
        }
    }
    return 0;
}

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

static inline __attribute__((always_inline)) void wait_for_prefetch(void)
{
    /* A branch-free wait adds no deliberate data accesses to the PMU window. */
    asm volatile(".rept " STRINGIFY(PREFETCH_WAIT_NOPS) "\n\t"
                 "nop\n\t"
                 ".endr\n\t"
                 ::: "memory");
}

static void unrelated_non_stride_stores(void)
{
    static const uint8_t pages[] = {0, 2, 1, 3};
    static const uint8_t lines[] = {5, 23, 41, 12};

    for (size_t i = 0; i < 4; i++) {
        mStore(store_noise + (size_t)pages[i] * PAGE_SIZE +
               (size_t)lines[i] * LINE_SIZE);
        nops();
    }
}

static void run_case(const char *name, trigger_fn_t trigger,
                     const uint8_t *trigger_lines, size_t trigger_accesses,
                     int *pmu_ready)
{
    struct timespec prefetch_wait = {
        .tv_sec = 0,
        .tv_nsec = PREFETCH_WAIT_NS,
    };
#if ENABLE_CACHE_PROBE
    uint64_t latency_sum[LINES_PER_REGION] = {0};
    uint64_t probe_count[LINES_PER_REGION] = {0};
    uint64_t probe_hits[LINES_PER_REGION] = {0};
#endif

    pmu_reset_accumulated();

    for (uint64_t round = 0; round < ROUNDS; round++) {
        /* Reset all context prefetcher state, then retrain within this round. */
        cpp_rctx();

        int pmu_running = 0;
        if (*pmu_ready) {
            pmu_running = (pmu_start() == 0);
            if (!pmu_running) {
                fprintf(stderr,
                        "# PMU unavailable: counter group could not be started\n");
                *pmu_ready = 0;
            }
        }

        /* Control and pattern share this training window; only pattern triggers. */
        train_repeated_footprint(round);

      // touch_extra_regions(round);
        uint8_t *target = region_pool + page_index(round, TRAIN_REGIONS + EXTRA_REGIONS) * PAGE_SIZE;
        flush_page(target);

        for (size_t i = 0; i < trigger_accesses; i++) {
            trigger(target + (size_t)trigger_lines[i] * LINE_SIZE);
            nops();
        }

        // unrelated_non_stride_stores();
        // mStore(target + 38 * LINE_SIZE);
        nops();
        wait_for_prefetch();
        if (PREFETCH_WAIT_NS > 0) {
            (void)nanosleep(&prefetch_wait, NULL);
        }

        // mfence();
        
        // int a = 10;
        // int b = a >>1 + 11;
        if (pmu_running) {
            pmu_stop_and_accumulate();
        }

#if ENABLE_CACHE_PROBE
        size_t probe_line = (size_t)((round * 37u + 11u) % LINES_PER_REGION);
        uint8_t *probe_addr = target + probe_line * LINE_SIZE;
        uint64_t start = timestamp();
        mStore_inline(probe_addr);
        uint64_t elapsed = timestamp() - start;
        latency_sum[probe_line] += elapsed;
        probe_count[probe_line]++;
        if (elapsed <= CACHE_HIT_THRESHOLD_TICKS) {
            probe_hits[probe_line]++;
        }
        flush(probe_addr);
#endif
    }

    printf("\n[%s]\n", name);
    printf("# trigger_pc=%s direct_demand_loads_per_round=%zu\n",
           trigger == region_pattern_load ? "trained" : "untrained-control",
           trigger_accesses);
    printf("# trigger_lines=");
    for (size_t i = 0; i < trigger_accesses; i++) {
        printf("%s%u", i == 0 ? "" : ",", trigger_lines[i]);
    }
    printf("\n");
    if (*pmu_ready) {
        pmu_print_accumulated(ROUNDS);
    }
#if ENABLE_CACHE_PROBE
    printf("# CACHE_PROBE timestamp=%s unit=%s threshold_ticks=%d\n",
           TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME, CACHE_HIT_THRESHOLD_TICKS);
    printf("# line\trole\tavg_latency\tsamples\thits_per_1000\n");
    for (size_t line = 0; line < LINES_PER_REGION; line++) {
        const char *role = "other";
        if (line_is_in(trigger_lines, trigger_accesses, line)) {
            role = "direct";
        } else if (line_is_in(footprint,
                              sizeof(footprint) / sizeof(footprint[0]), line)) {
            role = "footprint_candidate";
        }
        uint64_t average = probe_count[line] == 0
            ? 0 : latency_sum[line] / probe_count[line];
        uint64_t hits_per_1000 = probe_count[line] == 0
            ? 0 : probe_hits[line] * 1000 / probe_count[line];
        printf("# PROBE %2zu\t%-19s\t%llu\t%llu\t%llu\n",
               line, role, (unsigned long long)average,
               (unsigned long long)probe_count[line],
               (unsigned long long)hits_per_1000);
    }
#endif
}

int main(void)
{
    if (footprint[0] >= LINES_PER_REGION) {
        fprintf(stderr, "trigger line is outside the region\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(footprint) / sizeof(footprint[0]); i++) {
        if (footprint[i] >= LINES_PER_REGION) {
            fprintf(stderr, "footprint line %u is outside the region\n",
                    footprint[i]);
            return 1;
        }
    }

    region_pool = mmap(NULL, REGION_POOL_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (region_pool == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    store_noise = mmap(NULL, STORE_NOISE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (store_noise == MAP_FAILED) {
        fprintf(stderr, "store_noise mmap failed: %s\n", strerror(errno));
        munmap(region_pool, REGION_POOL_SIZE);
        return 1;
    }

    memset(region_pool, 0xff, REGION_POOL_SIZE);
    for (size_t offset = 0; offset < REGION_POOL_SIZE; offset += LINE_SIZE) {
        mLoad_inline(region_pool + offset);
    }
    memset(store_noise, 0xa5, STORE_NOISE_SIZE);
    for (size_t offset = 0; offset < STORE_NOISE_SIZE; offset += LINE_SIZE) {
        mLoad_inline(store_noise + offset);
    }
    mfence();

    printf("# irregular repeated-region hardware-prefetch existence test\n");
    printf("# region_bytes=%d lines_per_region=%d rounds=%d "
           "train_regions=%d extra_regions=%d pool_pages=%d "
           "wait_nops=%d wait_ns=%d cpp_rctx=each_round\n",
           PAGE_SIZE, LINES_PER_REGION, ROUNDS, TRAIN_REGIONS,
           EXTRA_REGIONS, REGION_POOL_PAGES, PREFETCH_WAIT_NOPS,
           PREFETCH_WAIT_NS);
    printf("# footprint_lines=");
    for (size_t i = 0; i < sizeof(footprint) / sizeof(footprint[0]); i++) {
        printf("%s%u", i == 0 ? "" : ",", footprint[i]);
    }
    printf("\n");
    printf("# trigger_lines=");
    for (size_t i = 0; i < sizeof(trigger_lines) / sizeof(trigger_lines[0]); i++) {
        printf("%s%u", i == 0 ? "" : ",", trigger_lines[i]);
    }
    printf("\n");
    printf("# PMU interpretation: train_only has no trigger; "
           "untrained_trigger uses an untrained PC; pattern uses the trained PC\n");
    printf("# PMU HWPRF values count implementation-defined prefetch events, "
           "not necessarily unique cache lines\n");

    int pmu_ready = 0;
#if ENABLE_PMU
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        fprintf(stderr,
                "# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
    }
#else
    printf("# PMU disabled by ENABLE_PMU=0\n");
#endif

    char train_only_name[128];
    char untrained_name[128];
    char pattern_name[128];
    int train_offset = snprintf(train_only_name, sizeof(train_only_name),
                                "train_only_trigger");
    int untrained_offset = snprintf(untrained_name, sizeof(untrained_name),
                                    "untrained_pc_control_trigger");
    int pattern_offset = snprintf(pattern_name, sizeof(pattern_name),
                                  "pattern_trigger");
    for (size_t i = 0; i < sizeof(trigger_lines) / sizeof(trigger_lines[0]); i++) {
        if (train_offset > 0 && (size_t)train_offset < sizeof(train_only_name)) {
            train_offset += snprintf(train_only_name + train_offset,
                                     sizeof(train_only_name) - (size_t)train_offset,
                                     "_%u", trigger_lines[i]);
        }
        if (untrained_offset > 0 && (size_t)untrained_offset < sizeof(untrained_name)) {
            untrained_offset += snprintf(untrained_name + untrained_offset,
                                         sizeof(untrained_name) - (size_t)untrained_offset,
                                         "_%u", trigger_lines[i]);
        }
        if (pattern_offset > 0 && (size_t)pattern_offset < sizeof(pattern_name)) {
            pattern_offset += snprintf(pattern_name + pattern_offset,
                                       sizeof(pattern_name) - (size_t)pattern_offset,
                                       "_%u", trigger_lines[i]);
        }
    }

    run_case(train_only_name, control_trigger_load, trigger_lines,
             0, &pmu_ready);
    run_case(untrained_name, control_trigger_load, trigger_lines,
             sizeof(trigger_lines) / sizeof(trigger_lines[0]), &pmu_ready);
    run_case(pattern_name, region_pattern_load, trigger_lines,
             sizeof(trigger_lines) / sizeof(trigger_lines[0]), &pmu_ready);

#if ENABLE_PMU
    pmu_cleanup();
#endif
    if (munmap(region_pool, REGION_POOL_SIZE) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    if (munmap(store_noise, STORE_NOISE_SIZE) != 0) {
        fprintf(stderr, "store_noise munmap failed: %s\n", strerror(errno));
        return 1;
    }
#if ENABLE_PMU
    return pmu_ready ? 0 : 2;
#else
    return 0;
#endif
}
