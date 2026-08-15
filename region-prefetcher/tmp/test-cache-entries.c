
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "../store-stride-prefetcher/until.h"

#ifndef ROUNDS
#define ROUNDS 2000
#endif
#ifndef REGION_POOL_PAGES
#define REGION_POOL_PAGES 8191
#endif
#ifndef TRAIN_REGIONS
#define TRAIN_REGIONS 32
#endif
#ifndef CACHE_HIT_THRESHOLD_TICKS
#define CACHE_HIT_THRESHOLD_TICKS 100
#endif
#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 1000
#endif
#ifndef PREFETCH_WAIT_NS
#define PREFETCH_WAIT_NS 10000
#endif
#ifndef ENTRY_BASE_LINE
#define ENTRY_BASE_LINE 20
#endif
#ifndef ENTRY_SECOND_DELTA
#define ENTRY_SECOND_DELTA 15
#endif
#ifndef ENTRY_PROBE_DELTA_FROM_SECOND
#define ENTRY_PROBE_DELTA_FROM_SECOND 2
#endif
#ifndef MAX_ACTIVE_REGIONS
#define MAX_ACTIVE_REGIONS 128
#endif

#define LINES_PER_REGION (PAGE_SIZE / LINE_SIZE)
#define REGION_POOL_SIZE ((size_t)REGION_POOL_PAGES * PAGE_SIZE)

static const int footprint_offsets[] = {0, 15, 2, 23, 9, -3, 11};
static uint8_t *region_pool;

__attribute__((noinline)) static void sms_load(void *addr)
{
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

__attribute__((noinline)) static void control_load(void *addr)
{
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

static void flush_page(uint8_t *page)
{
    for (size_t line = 0; line < LINES_PER_REGION; line++) {
        flush(page + line * LINE_SIZE);
    }
    mfence();
}

static size_t page_index(uint64_t round, size_t position)
{
    return (size_t)((round * 151u + position * 73u) % REGION_POOL_PAGES);
}

static inline __attribute__((always_inline)) void wait_for_prefetch(void)
{
    for (int i = 0; i < PREFETCH_WAIT_NOPS; i++) nop();
    if (PREFETCH_WAIT_NS > 0) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = PREFETCH_WAIT_NS};
        (void)nanosleep(&ts, NULL);
    }
}

static int init_pool(void)
{
    region_pool = mmap(NULL, REGION_POOL_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (region_pool == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return -1;
    }
    memset(region_pool, 0xff, REGION_POOL_SIZE);
    for (size_t offset = 0; offset < REGION_POOL_SIZE; offset += LINE_SIZE) {
        mLoad_inline(region_pool + offset);
    }
    mfence();
    return 0;
}

static void cleanup_pool(void)
{
    if (region_pool && region_pool != MAP_FAILED) munmap(region_pool, REGION_POOL_SIZE);
}

static void train_entry_survival(uint64_t round, int active_regions)
{
    for (size_t episode = 0; episode < TRAIN_REGIONS; episode++) {
        size_t base_pos = episode * (size_t)(MAX_ACTIVE_REGIONS + 1);
        uint8_t *subject = region_pool + page_index(round, base_pos) * PAGE_SIZE;
        flush_page(subject);

        sms_load(subject + (size_t)ENTRY_BASE_LINE * LINE_SIZE);
        nops();

        for (int i = 1; i < active_regions; i++) {
            uint8_t *page = region_pool + page_index(round, base_pos + (size_t)i) * PAGE_SIZE;
            flush_page(page);
            sms_load(page + (size_t)ENTRY_BASE_LINE * LINE_SIZE);
            nops();
        }

        for (size_t i = 1; i < sizeof(footprint_offsets) / sizeof(footprint_offsets[0]); i++) {
            int line = ENTRY_BASE_LINE + footprint_offsets[i];
            sms_load(subject + (size_t)line * LINE_SIZE);
            nops();
        }
        mfence();
    }
}

typedef void (*load_fn_t)(void *);

static void run_case(const char *name, load_fn_t trigger, int active_regions,
                     uint64_t *sum, uint64_t *hits)
{
    *sum = 0;
    *hits = 0;
    const int second_line = ENTRY_BASE_LINE + ENTRY_SECOND_DELTA;
    const int probe_line = second_line + ENTRY_PROBE_DELTA_FROM_SECOND;

    for (uint64_t round = 0; round < ROUNDS; round++) {
        cpp_rctx();
        train_entry_survival(round, active_regions);

        uint8_t *target = region_pool + page_index(round, TRAIN_REGIONS * (size_t)(MAX_ACTIVE_REGIONS + 1) + 1) * PAGE_SIZE;
        flush_page(target);
        trigger(target + (size_t)ENTRY_BASE_LINE * LINE_SIZE);
        nops();
        trigger(target + (size_t)second_line * LINE_SIZE);
        nops();
        wait_for_prefetch();

        uint8_t *probe_addr = target + (size_t)probe_line * LINE_SIZE;
        uint64_t start = timestamp();
        mLoad_inline(probe_addr);
        uint64_t elapsed = timestamp() - start;
        *sum += elapsed;
        *hits += (elapsed <= CACHE_HIT_THRESHOLD_TICKS);
        flush(probe_addr);
    }
    printf("# %s avg=%llu hits=%llu/%d\n", name,
           (unsigned long long)(*sum / ROUNDS),
           (unsigned long long)*hits, ROUNDS);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <active_regions>\n", argv[0]);
        return 2;
    }
    int active_regions = atoi(argv[1]);
    const int second_line = ENTRY_BASE_LINE + ENTRY_SECOND_DELTA;
    const int probe_line = second_line + ENTRY_PROBE_DELTA_FROM_SECOND;
    if (active_regions < 1 || active_regions > MAX_ACTIVE_REGIONS ||
        TRAIN_REGIONS * (MAX_ACTIVE_REGIONS + 1) + 2 >= REGION_POOL_PAGES ||
        ENTRY_BASE_LINE - 3 < 0 || ENTRY_BASE_LINE + 23 >= (int)LINES_PER_REGION ||
        probe_line < 0 || probe_line >= (int)LINES_PER_REGION) {
        fprintf(stderr, "invalid parameters\n");
        return 2;
    }
    if (init_pool() != 0) return 1;
    uint64_t trained_sum, trained_hits, control_sum, control_hits;
    run_case("trained", sms_load, active_regions, &trained_sum, &trained_hits);
    run_case("control", control_load, active_regions, &control_sum, &control_hits);
    printf("RESULT active_regions=%d anchor=%d candidate=%d trained_avg=%llu control_avg=%llu trained_hit_rate=%.4f control_hit_rate=%.4f\n",
           active_regions, second_line, probe_line,
           (unsigned long long)(trained_sum / ROUNDS),
           (unsigned long long)(control_sum / ROUNDS),
           (double)trained_hits / (double)ROUNDS,
           (double)control_hits / (double)ROUNDS);
    cleanup_pool();
    return 0;
}
