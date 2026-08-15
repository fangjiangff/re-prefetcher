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
#define TRAIN_REGIONS 8
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
#define PREFETCH_WAIT_NS 0
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
#define FOOTPRINT_LINES {4, 1, 6, 12}
#endif
#ifndef TRIGGER_LINES
#define TRIGGER_LINES {3}
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
 * Fixed, collision-free permutation of all 257 pages. It is generated
 * offline, not by a runtime PRNG. The first 13 entries are deliberately
 * non-equidistant:
 *   177,235,22,80,180,93,104,241,18,234,146,39,198
 */
static const uint16_t page_order[REGION_POOL_PAGES] = {
    177,235,22,80,180,93,104,241,18,234,146,39,198,137,141,238,
    199,52,82,60,14,229,219,12,150,147,31,87,86,56,25,21,
    72,206,194,64,101,76,17,209,166,173,1,192,3,233,139,6,
    119,249,167,158,250,214,78,127,132,121,122,163,15,220,35,2,
    149,247,67,151,13,195,211,89,114,57,203,66,188,83,36,75,
    228,9,217,109,227,85,225,161,178,47,53,224,74,145,208,34,
    165,115,169,98,187,207,186,152,133,157,252,189,253,215,103,202,
    26,117,246,184,216,113,160,128,45,196,95,27,20,102,44,131,
    134,81,0,176,123,126,213,155,237,230,162,61,212,129,97,144,
    41,73,11,140,231,55,154,42,4,68,5,100,124,105,71,50,
    70,159,88,142,92,223,49,112,183,33,204,210,79,96,32,172,
    30,148,40,248,29,182,221,174,69,191,54,200,143,168,46,62,
    244,106,190,10,108,255,222,37,242,94,135,136,125,130,179,43,
    7,99,90,8,138,251,118,24,254,65,256,84,91,48,240,181,
    156,193,63,51,185,236,232,201,171,170,226,110,107,245,38,28,
    243,197,175,205,58,19,116,120,59,218,111,23,239,16,153,164,
    77,
};

static size_t page_index(uint64_t round, size_t position)
{
    // return (size_t)((round * 151u + position * 73u) % REGION_POOL_PAGES);
    // size_t order_index = (size_t)((round * 151u + position) % REGION_POOL_PAGES);
    size_t order_index =
        (size_t)((round * 151u + position * 73u) % REGION_POOL_PAGES);
    return page_order[order_index];
}

static void flush_training_pages(uint64_t round)
{
    for (size_t region = 0; region < TRAIN_REGIONS; region++) {
        uint8_t *page = region_pool + page_index(round, region) * PAGE_SIZE;
        flush_page(page);
    }
}

static void train_repeated_footprint(uint64_t round)
{
    for (size_t region = 0; region < TRAIN_REGIONS; region++) {
        uint8_t *page = region_pool + page_index(round, region) * PAGE_SIZE;

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

        int pmu_running = 0;
        if (*pmu_ready) {
            pmu_running = (pmu_start() == 0);
            if (!pmu_running) {
                fprintf(stderr,
                        "# PMU unavailable: counter group could not be started\n");
                *pmu_ready = 0;
            }
        }

    for (uint64_t round = 0; round < ROUNDS; round++) {
        /* Reset all context prefetcher state, then retrain within this round. */

        flush_training_pages(round);
        uint8_t *target = region_pool +
            page_index(round, TRAIN_REGIONS + EXTRA_REGIONS) * PAGE_SIZE;
        /* Keep all cache maintenance outside the measured PMU window. */
        flush_page(target);

        cpp_rctx();
        /* Control and pattern share this training window; only pattern triggers. */
        train_repeated_footprint(round);

        for (size_t i = 0; i < trigger_accesses; i++) {
            trigger(target + (size_t)trigger_lines[i] * LINE_SIZE);
            nops();
        }

        nops();
        wait_for_prefetch();
        if (PREFETCH_WAIT_NS > 0) {
            (void)nanosleep(&prefetch_wait, NULL);
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

    if (pmu_running) {
            pmu_stop_and_accumulate();
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
    printf("# CACHE_PROBE timestamp=%s unit=%s threshold=%d\n",
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
    printf("# PMU interpretation: control is training-only; "
           "pattern adds the trained-PC trigger sequence\n");
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

    char control_name[128];
    char pattern_name[128];
    int offset = snprintf(control_name, sizeof(control_name),
                          "untrained_pc_control_trigger");
    int pattern_offset = snprintf(pattern_name, sizeof(pattern_name),
                                  "pattern_trigger");
    for (size_t i = 0; i < sizeof(trigger_lines) / sizeof(trigger_lines[0]); i++) {
        if (offset > 0 && (size_t)offset < sizeof(control_name)) {
            offset += snprintf(control_name + offset,
                               sizeof(control_name) - (size_t)offset,
                               "_%u", trigger_lines[i]);
        }
        if (pattern_offset > 0 && (size_t)pattern_offset < sizeof(pattern_name)) {
            pattern_offset += snprintf(pattern_name + pattern_offset,
                                       sizeof(pattern_name) - (size_t)pattern_offset,
                                       "_%u", trigger_lines[i]);
        }
    }

    run_case(control_name, control_trigger_load, trigger_lines,
             0, &pmu_ready);
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
