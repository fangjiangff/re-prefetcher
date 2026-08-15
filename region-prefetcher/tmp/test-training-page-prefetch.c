#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#ifndef __aarch64__
#error "This test requires AArch64."
#endif

#include "../../store-stride-prefetcher/until.h"

#ifndef ROUNDS
#define ROUNDS 4096
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

#define POOL_PAGES 257
#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define POOL_SIZE ((size_t)POOL_PAGES * PAGE_SIZE)

static uint8_t *pool;

__attribute__((noinline)) static void training_load(void *addr)
{
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

static void flush_page(uint8_t *page)
{
    for (size_t line = 0; line < LINES_PER_PAGE; line++) {
        flush(page + line * LINE_SIZE);
    }
    mfence();
}

static size_t page_index(uint64_t round)
{
    return (size_t)((round * 151u) % POOL_PAGES);
}
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)


static inline __attribute__((always_inline)) void wait_nops(void)
{
    asm volatile(".rept " STRINGIFY(PREFETCH_WAIT_NOPS) "\n\t"
                 "nop\n\t"
                 ".endr\n\t"
                 ::: "memory");
}

static int is_demand_line(size_t accesses, size_t line)
{
    return (accesses >= 1 && line == 4) || (accesses >= 2 && line == 1);
}

static void run_case(const char *name, size_t accesses)
{
    uint64_t latency_sum[LINES_PER_PAGE] = {0};
    uint64_t samples[LINES_PER_PAGE] = {0};
    uint64_t hits[LINES_PER_PAGE] = {0};
    const struct timespec wait_time = {.tv_sec = 0, .tv_nsec = PREFETCH_WAIT_NS};

    for (uint64_t round = 0; round < ROUNDS; round++) {
        uint8_t *page = pool + page_index(round) * PAGE_SIZE;
        size_t probe_line = (size_t)((round * 37u + 11u) % LINES_PER_PAGE);

        cpp_rctx();
        flush_page(page);

        if (accesses >= 1) {
            training_load(page + 4 * LINE_SIZE);
            nops();
        }
        if (accesses >= 2) {
            training_load(page + 1 * LINE_SIZE);
            nops();
        }

        wait_nops();
        if (PREFETCH_WAIT_NS > 0) {
            (void)nanosleep(&wait_time, NULL);
        }

        uint64_t start = timestamp();
        mLoad_inline(page + probe_line * LINE_SIZE);
        uint64_t elapsed = timestamp() - start;
        latency_sum[probe_line] += elapsed;
        samples[probe_line]++;
        hits[probe_line] += elapsed <= CACHE_HIT_THRESHOLD_TICKS;
    }

    printf("\n[%s]\n", name);
    printf("# training_accesses_per_round=%zu\n", accesses);
    printf("# line\trole\tavg_latency\tsamples\thits_per_1000\n");
    for (size_t line = 0; line < LINES_PER_PAGE; line++) {
        uint64_t average = samples[line] ? latency_sum[line] / samples[line] : 0;
        uint64_t hit_rate = samples[line] ? hits[line] * 1000 / samples[line] : 0;
        printf("# PROBE %2zu\t%-9s\t%llu\t%llu\t%llu\n",
               line, is_demand_line(accesses, line) ? "demand" : "candidate",
               (unsigned long long)average,
               (unsigned long long)samples[line],
               (unsigned long long)hit_rate);
    }
}

int main(void)
{
    pool = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (pool == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }

    memset(pool, 0xa5, POOL_SIZE);
    for (size_t offset = 0; offset < POOL_SIZE; offset += LINE_SIZE) {
        mLoad_inline(pool + offset);
    }
    mfence();

    printf("# same-training-page prefetch probe\n");
    printf("# rounds=%d wait_nops=%d wait_ns=%d timestamp=%s unit=%s threshold=%d\n",
           ROUNDS, PREFETCH_WAIT_NOPS, PREFETCH_WAIT_NS,
           TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME,
           CACHE_HIT_THRESHOLD_TICKS);
    printf("# each round: cpp_rctx; flush page; train; wait; probe one line\n");

    run_case("empty", 0);
    run_case("load_4", 1);
    run_case("load_4_1", 2);

    if (munmap(pool, POOL_SIZE) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
