#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef __aarch64__
#error "This test requires AArch64 EL0 memory-access and cache-maintenance instructions."
#endif

#include "../until.h"

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef TRAIN_REGIONS
#define TRAIN_REGIONS 8
#endif

#ifndef REGION_POOL_PAGES
#define REGION_POOL_PAGES 16705
#endif
#define REGION_POOL_BASE 0x600000000ull

#ifndef EXTRA_REGIONS
#define EXTRA_REGIONS 0
#endif

/*
 * Number of independent region-pattern entries inserted between the trainer
 * and the target trigger. Each entry has its own PC[17:3] key. X925 trains
 * each entry on TRAIN_REGIONS regions; A725 needs one region per entry.
 */
#ifndef ENTRIES
#define ENTRIES 0
#endif

#define MAX_ENTRY_PCS 16384
#if ENTRIES < 0 || ENTRIES > MAX_ENTRY_PCS
#error "ENTRIES must be in [0, MAX_ENTRY_PCS]"
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
#ifndef PROBE_FOCUS_LINE0
#define PROBE_FOCUS_LINE0 -1
#endif
#ifndef PROBE_FOCUS_LINE1
#define PROBE_FOCUS_LINE1 -1
#endif


#ifndef ACCESS_TYPE_STORE
#define ACCESS_TYPE_STORE 0
#endif

#if ACCESS_TYPE_STORE
#define REGION_ACCESS_TYPE_NAME "store"
#else
#define REGION_ACCESS_TYPE_NAME "load"
#endif

#ifndef PMU_CORE_X925
#define PMU_CORE_X925 1
#endif
#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif
#if PMU_CORE_X925
#define ENTRY_TRAIN_REGIONS TRAIN_REGIONS
#else
#define ENTRY_TRAIN_REGIONS 1
#endif

#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"

static uint64_t pmu_l1_accumulated;
static uint64_t pmu_l2_accumulated;
static struct prefetch_pmu_snapshot pmu_before;
static int pmu_device_fd = -1;

static int pmu_setup(void)
{
#if ENABLE_PMU
    pmu_device_fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
    if (pmu_device_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n",
                PREFETCH_PMU_DEVICE_PATH, strerror(errno));
        return -1;
    }
#if PMU_CORE_X925
    printf("# PMU source=direct-mrs core=Cortex-X925 "
           "counter4=0x8154 counter5=0x8155 "
           "window=trigger-plus-wait-per-round "
           "rearm=once-per-experiment\n");
#else
    printf("# PMU source=direct-mrs core=Cortex-A725 "
           "counter4=0x8154 counter5=0x8285 "
           "window=trigger-plus-wait-per-round "
           "rearm=once-per-experiment\n");
#endif
    if (ioctl(pmu_device_fd, PREFETCH_PMU_IOC_REARM) != 0) {
        fprintf(stderr, "PMU rearm ioctl failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
#else
    return -1;
#endif
}

static void pmu_reset_accumulated(void)
{
    pmu_l1_accumulated = 0;
    pmu_l2_accumulated = 0;
}

static int pmu_start(void)
{
#if ENABLE_PMU
    pmu_before = prefetch_pmu_read();
    return 0;
#else
    return -1;
#endif
}

static void pmu_stop_and_accumulate(void)
{
#if ENABLE_PMU
    struct prefetch_pmu_snapshot after = prefetch_pmu_read();

    pmu_l1_accumulated +=
        prefetch_pmu_delta(after.l1_hwprf, pmu_before.l1_hwprf);
    pmu_l2_accumulated +=
        prefetch_pmu_delta(after.l2_prefetch, pmu_before.l2_prefetch);
#endif
}

static void pmu_print_accumulated(uint64_t rounds)
{
    double denominator = rounds == 0 ? 1.0 : (double)rounds;

    printf("# PMU %-24s per_round=%.2f\n", "l1d_cache_hwprf",
           (double)pmu_l1_accumulated / denominator);
    printf("# PMU %-24s per_round=%.2f\n", "l2d_cache_hwprf",
           (double)pmu_l2_accumulated / denominator);
}

static void pmu_cleanup(void)
{
    if (pmu_device_fd >= 0) {
        close(pmu_device_fd);
        pmu_device_fd = -1;
    }
}

#if REGION_POOL_PAGES <= (TRAIN_REGIONS + ENTRIES * ENTRY_TRAIN_REGIONS + EXTRA_REGIONS)
#error "REGION_POOL_PAGES must cover trainer, inserted, extra, and target regions"
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

/* Label the actual memory-access instruction, not merely the function entry. */
extern char region_pattern_access_pc[];

/* Training and treatment trigger execute this exact same access PC. */
__attribute__((noinline)) static void region_pattern_access(void *addr)
{
#if ACCESS_TYPE_STORE
    asm volatile(".global region_pattern_access_pc\n"
                 "region_pattern_access_pc:\n\t"
                 "strb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
#else
    asm volatile(".global region_pattern_access_pc\n"
                 "region_pattern_access_pc:\n\t"
                 "ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
#endif
}

/* The control trigger has no matching multi-line footprint history. */
__attribute__((noinline)) static void control_trigger_access(void *addr)
{
#if ACCESS_TYPE_STORE
    asm volatile("strb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
#else
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
#endif
}

typedef void (*region_access_fn_t)(void *);

#define ENTRY_PC_KEY_SHIFT 3
#define ENTRY_PC_KEY_BITS 15
#define ENTRY_PC_KEY_COUNT (1u << ENTRY_PC_KEY_BITS)
#define ENTRY_GADGET_BASE 0x510000000ull
#define MAX_MAPPED_GADGET_PAGES 64
#define ENTRY_FOOTPRINT_LENGTH 3

#if MAX_ENTRY_PCS >= ENTRY_PC_KEY_COUNT
#error "MAX_ENTRY_PCS must leave one PC[17:3] key for the trainer"
#endif

static const uint8_t entry_footprint[ENTRY_FOOTPRINT_LENGTH] = {32, 41, 55};
static region_access_fn_t entry_accesses[MAX_ENTRY_PCS];
static size_t gadget_page_size;
static uintptr_t mapped_gadget_pages[MAX_MAPPED_GADGET_PAGES];
static size_t mapped_gadget_page_count;

extern char _entry_gadget_start[];
extern char _entry_gadget_end[];

#if ACCESS_TYPE_STORE
#define ENTRY_GADGET_ASM "    strb w1, [x0]\n"
#else
#define ENTRY_GADGET_ASM "    ldrb w1, [x0]\n"
#endif

asm(
    ".global _entry_gadget_start\n"
    ".global _entry_gadget_end\n"
    "_entry_gadget_start:\n"
    ENTRY_GADGET_ASM
    "    ret\n"
    "_entry_gadget_end:\n"
    "    nop\n"
);

static unsigned pc_key_3_17(uintptr_t pc)
{
    return (unsigned)((pc >> ENTRY_PC_KEY_SHIFT) &
                      (ENTRY_PC_KEY_COUNT - 1u));
}

static uintptr_t gadget_page_base(uintptr_t address)
{
    return address - address % gadget_page_size;
}

static int ensure_gadget_page(uintptr_t page)
{
    for (size_t i = 0; i < mapped_gadget_page_count; i++) {
        if (mapped_gadget_pages[i] == page) {
            return 0;
        }
    }
    if (mapped_gadget_page_count >= MAX_MAPPED_GADGET_PAGES) {
        fprintf(stderr, "too many mapped entry-gadget pages\n");
        return -1;
    }

    void *mapping = mmap((void *)page, gadget_page_size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_FIXED_NOREPLACE | MAP_ANONYMOUS |
                         MAP_PRIVATE | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap entry-gadget page 0x%016lx failed: %s\n",
                (unsigned long)page, strerror(errno));
        return -1;
    }
    mapped_gadget_pages[mapped_gadget_page_count++] = page;
    return 0;
}

static region_access_fn_t map_entry_gadget(uintptr_t address)
{
    uintptr_t page = gadget_page_base(address);
    size_t page_offset = (size_t)(address - page);
    size_t gadget_size = (size_t)(_entry_gadget_end - _entry_gadget_start);

    if ((address & 7u) != 0 || gadget_size != 8 ||
        page_offset + gadget_size > gadget_page_size) {
        fprintf(stderr, "invalid entry-gadget address 0x%016lx size=%zu\n",
                (unsigned long)address, gadget_size);
        return NULL;
    }
    if (ensure_gadget_page(page) != 0) {
        return NULL;
    }
    memcpy((void *)address, _entry_gadget_start, gadget_size);
    __builtin___clear_cache((char *)address, (char *)(address + gadget_size));
    return (region_access_fn_t)(void *)address;
}

static int setup_entry_gadgets(size_t entries)
{
    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect page size\n");
        return -1;
    }
    gadget_page_size = (size_t)detected_page_size;

    unsigned trainer_key = pc_key_3_17((uintptr_t)region_pattern_access_pc);
    for (size_t entry = 0; entry < entries; entry++) {
        size_t slot = entry;
        if (slot >= trainer_key) {
            slot++;
        }
        uintptr_t address = ENTRY_GADGET_BASE +
            (slot << ENTRY_PC_KEY_SHIFT);
        entry_accesses[entry] = map_entry_gadget(address);
        if (entry_accesses[entry] == NULL) {
            return -1;
        }
        unsigned entry_key = pc_key_3_17(address);
        if (entry_key != slot) {
            fprintf(stderr,
                    "entry %zu has unexpected PC[17:3] key %u (wanted %zu)\n",
                    entry, entry_key, slot);
            return -1;
        }
        if (entry_key == trainer_key) {
            fprintf(stderr, "entry %zu aliases trainer PC[17:3]\n", entry);
            return -1;
        }
    }
    return 0;
}

#undef ENTRY_GADGET_ASM
#if EXTRA_REGIONS > 0
/* A third PC creates region turnover without adding to the learned pattern. */
__attribute__((noinline)) static void extra_region_access(void *addr)
{
#if ACCESS_TYPE_STORE
    asm volatile("strb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
#else
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
#endif
}
#endif

static void flush_page(uint8_t *page)
{
    for (size_t line = 0; line < LINES_PER_REGION; line++) {
        flush(page + line * LINE_SIZE);
    }
    mfence();
}

#define PAGE_ORDER_LENGTH 257
#if (REGION_POOL_PAGES % PAGE_ORDER_LENGTH) != 0
#error "REGION_POOL_PAGES must be a multiple of PAGE_ORDER_LENGTH"
#endif
#if (REGION_POOL_PAGES % 73) == 0
#error "REGION_POOL_PAGES must be coprime with the position multiplier"
#endif

/*
 * Fixed, collision-free permutation within each 257-page block. It is generated
 * offline, not by a runtime PRNG. The first 13 entries are deliberately
 * non-equidistant:
 *   177,235,22,80,180,93,104,241,18,234,146,39,198
 */
static const uint16_t page_order[PAGE_ORDER_LENGTH] = {
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
    size_t order_index =
        (size_t)((round * 151u + position * 73u) % REGION_POOL_PAGES);
    size_t block = order_index / PAGE_ORDER_LENGTH;
    size_t within_block = order_index % PAGE_ORDER_LENGTH;

    return block * PAGE_ORDER_LENGTH + page_order[within_block];
}

static void flush_training_pages(uint64_t round, size_t entries)
{
    size_t region_count = (size_t)TRAIN_REGIONS + entries * ENTRY_TRAIN_REGIONS;

    for (size_t position = 0; position < region_count; position++) {
        uint8_t *page = region_pool +
            page_index(round, position) * PAGE_SIZE;
        const uint8_t *lines = position < TRAIN_REGIONS
            ? footprint : entry_footprint;
        size_t line_count = position < TRAIN_REGIONS
            ? sizeof(footprint) / sizeof(footprint[0])
            : ENTRY_FOOTPRINT_LENGTH;

        for (size_t i = 0; i < line_count; i++) {
            flush(page + (size_t)lines[i] * LINE_SIZE);
        }
    }
    mfence();
}

static void train_repeated_footprint(uint64_t round)
{
    for (size_t region = 0; region < TRAIN_REGIONS; region++) {
        uint8_t *page = region_pool + page_index(round, region) * PAGE_SIZE;

        for (size_t i = 0; i < sizeof(footprint) / sizeof(footprint[0]); i++) {
            region_pattern_access(page + (size_t)footprint[i] * LINE_SIZE);
            nops();
        }
        mfence();
    }
}

static void train_other_regions(uint64_t round, size_t entries)
{
    for (size_t entry = 0; entry < entries; entry++) {
        region_access_fn_t access = entry_accesses[entry];
        size_t base_position = (size_t)TRAIN_REGIONS +
            entry * ENTRY_TRAIN_REGIONS;

        for (size_t region = 0; region < ENTRY_TRAIN_REGIONS; region++) {
            uint8_t *page = region_pool +
                page_index(round, base_position + region) * PAGE_SIZE;

            for (size_t i = 0; i < ENTRY_FOOTPRINT_LENGTH; i++) {
                access(page + (size_t)entry_footprint[i] * LINE_SIZE);
                nops();
            }
            mfence();
        }
    }
}

static void touch_extra_regions(uint64_t round)
{
#if EXTRA_REGIONS > 0
    for (size_t region = 0; region < EXTRA_REGIONS; region++) {
        size_t position =
            (size_t)TRAIN_REGIONS + ENTRIES * ENTRY_TRAIN_REGIONS + region;
        uint8_t *page = region_pool + page_index(round, position) * PAGE_SIZE;

        flush_page(page);
        extra_region_access(page);
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

    for (uint64_t round = 0; round < ROUNDS; round++) {
        /* Reset all context prefetcher state, then retrain within this round. */

        flush_training_pages(round, ENTRIES);
        uint8_t *target = region_pool +
            page_index(round, REGION_POOL_PAGES - 1u) * PAGE_SIZE;
        /* Keep all cache maintenance outside the measured PMU window. */
        flush_page(target);

        cpp_rctx();
        /* Control and pattern share this training window; only pattern triggers. */
        train_repeated_footprint(round);

        train_other_regions(round, ENTRIES);
        /* Drain prefetch activity caused by competing PCs before PMU starts. */
        wait_for_prefetch();
        if (PREFETCH_WAIT_NS > 0) {
            (void)nanosleep(&prefetch_wait, NULL);
        }
        mfence();
        /* Remove target-cache pollution from competing-region prefetches. */
        flush_page(target);
        mfence();



        int pmu_running = 0;
        if (*pmu_ready) {
            pmu_running = (pmu_start() == 0);
            if (!pmu_running) {
                fprintf(stderr, "# direct PMU counter read failed\n");
                *pmu_ready = 0;
            }
        }

        for (size_t i = 0; i < trigger_accesses; i++) {
            trigger(target + (size_t)trigger_lines[i] * LINE_SIZE);
            nops();
        }

        nops();
        wait_for_prefetch();
        if (PREFETCH_WAIT_NS > 0) {
            (void)nanosleep(&prefetch_wait, NULL);
        }

        if (pmu_running) {
            pmu_stop_and_accumulate();
        }

#if ENABLE_CACHE_PROBE
#if PROBE_FOCUS_LINE0 >= 0 && PROBE_FOCUS_LINE1 >= 0
        size_t probe_line = (round & 1u) ? PROBE_FOCUS_LINE1 : PROBE_FOCUS_LINE0;
#else
        size_t probe_line = (size_t)((round * 37u + 11u) % LINES_PER_REGION);
#endif
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
    printf("# trigger_pc=%s direct_demand_accesses_per_round=%zu\n",
           trigger == region_pattern_access ? "trained" : "untrained-control",
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
    if (setup_entry_gadgets(ENTRIES) != 0) {
        return 1;
    }


    region_pool = mmap((void *)REGION_POOL_BASE, REGION_POOL_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS |
                       MAP_POPULATE, -1, 0);
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
    printf("# PC key model=PC[17:3] trainer_pc=0x%016lx trainer_key=%u\n",
           (unsigned long)(uintptr_t)region_pattern_access_pc,
           pc_key_3_17((uintptr_t)region_pattern_access_pc));
    if (ENTRIES > 0) {
        printf("# entry_pc_keys=unique first_pc=0x%016lx first_key=%u "
               "last_pc=0x%016lx last_key=%u\n",
               (unsigned long)(uintptr_t)entry_accesses[0],
               pc_key_3_17((uintptr_t)entry_accesses[0]),
               (unsigned long)(uintptr_t)entry_accesses[ENTRIES - 1],
               pc_key_3_17((uintptr_t)entry_accesses[ENTRIES - 1]));
    }
    printf("# region_bytes=%d lines_per_region=%d rounds=%d "
           "train_regions=%d entries=%d inserted_regions=%d "
           "extra_regions=%d pool_pages=%d "
           "wait_nops=%d wait_ns=%d cpp_rctx=each_round\n",
           PAGE_SIZE, LINES_PER_REGION, ROUNDS, TRAIN_REGIONS, ENTRIES,
           ENTRIES * ENTRY_TRAIN_REGIONS,
           EXTRA_REGIONS, REGION_POOL_PAGES, PREFETCH_WAIT_NOPS,
           PREFETCH_WAIT_NS);
    printf("# footprint_lines=");
    for (size_t i = 0; i < sizeof(footprint) / sizeof(footprint[0]); i++) {
        printf("%s%u", i == 0 ? "" : ",", footprint[i]);
    }
    printf("\n");
    printf("# entry_footprint_lines=");
    for (size_t i = 0; i < ENTRY_FOOTPRINT_LENGTH; i++) {
        printf("%s%u", i == 0 ? "" : ",", entry_footprint[i]);
    }
    printf("\n");
    printf("# access_type=%s\n", REGION_ACCESS_TYPE_NAME);
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
        fprintf(stderr, "# direct PMU counter setup failed\n");
    }
#else
    printf("# PMU disabled by ENABLE_PMU=0\n");
#endif

    char pattern_name[128];
    int pattern_offset = snprintf(pattern_name, sizeof(pattern_name),
                                  "pattern_trigger");
    for (size_t i = 0; i < sizeof(trigger_lines) / sizeof(trigger_lines[0]); i++) {
        if (pattern_offset > 0 && (size_t)pattern_offset < sizeof(pattern_name)) {
            pattern_offset += snprintf(pattern_name + pattern_offset,
                                       sizeof(pattern_name) - (size_t)pattern_offset,
                                       "_%u", trigger_lines[i]);
        }
    }

    run_case(pattern_name, region_pattern_access, trigger_lines,
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
