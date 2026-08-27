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

#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"

#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define ACCESS_COUNT 7
#define POOL_PAGES 257
#define DEFAULT_ROUNDS_PER_LINE 200
#define DEFAULT_THRESHOLD_NS 120
#define PAGE_SEED 0x6a09e667f3bcc909ULL

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

enum stream_case {
    CASE_FORWARD,
    CASE_NONDIRECTIONAL,
};

struct line_stats {
    uint64_t probes;
    uint64_t hits;
    uint64_t latency_ns;
};

struct case_stats {
    struct line_stats lines[LINES_PER_PAGE];
    uint64_t samples;
    uint64_t l1_events;
    uint64_t l2_events;
};

typedef void (*access_function)(void *);

/*
 * The forward sequence is monotonic but deliberately non-strided. Its six
 * deltas are 7, 3, 11, 5, 9, and 4 cache lines. The control accesses the
 * same set of lines while alternating direction. Therefore neither case
 * differs in working set, number of misses, or load PCs.
 *
 * Keeping the first and last accesses away from page boundaries lets the
 * resulting cache map show whether prefetches occur only in the observed
 * direction. Stream-prefetcher existence is indicated by multiple adjacent
 * hits after accesses in the forward case, absent from the nondirectional
 * control. A single adjacent hit is not sufficient evidence because an
 * adjacent-cache-line prefetcher can produce it.
 */
static const uint8_t forward_lines[ACCESS_COUNT] = {
    8, 15, 18, 29, 34, 43, 47,
};

static const uint8_t nondirectional_lines[ACCESS_COUNT] = {
    8, 29, 15, 43, 18, 47, 34,
};

static uint8_t *mapping;
static size_t mapping_size;
static size_t page_order[POOL_PAGES];
static uint64_t timer_freq_hz;

#if ENABLE_PMU
static int pmu_fd = -1;
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
    asm volatile(".rept " STRINGIFY(PREFETCH_WAIT_NOPS) "\n\t"
                 "nop\n\t"
                 ".endr\n\t"
                 ::: "memory");
}

/* Distinct noinline functions guarantee a distinct load PC per access. */
#define DEFINE_ACCESS(number, reg)                                     \
    __attribute__((noinline)) static void stream_access##number(void *addr) \
    {                                                                  \
        asm volatile("ldr " #reg ", [%0]"                            \
                     :: "r"(addr) : #reg, "memory");                  \
    }

DEFINE_ACCESS(0, x1)
DEFINE_ACCESS(1, x2)
DEFINE_ACCESS(2, x3)
DEFINE_ACCESS(3, x4)
DEFINE_ACCESS(4, x5)
DEFINE_ACCESS(5, x6)
DEFINE_ACCESS(6, x7)

static access_function const access_pcs[ACCESS_COUNT] = {
    stream_access0, stream_access1, stream_access2, stream_access3,
    stream_access4, stream_access5, stream_access6,
};

static uint8_t *page_addr(size_t page)
{
    return mapping + page * PAGE_SIZE;
}

static void init_page_order(void)
{
    uint64_t rng = PAGE_SEED;

    for (size_t i = 0; i < POOL_PAGES; i++)
        page_order[i] = i;
    for (size_t i = POOL_PAGES - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(&rng) % (i + 1));
        size_t temporary = page_order[i];

        page_order[i] = page_order[j];
        page_order[j] = temporary;
    }
}

static size_t sample_page(enum stream_case which, size_t probe_line,
                          size_t round)
{
    size_t position = round * LINES_PER_PAGE + probe_line;

    position += which == CASE_FORWARD ? 0u : 113u;
    return page_order[(position * 151u) % POOL_PAGES];
}

static void prefault_mapping(void)
{
    for (size_t i = 0; i < POOL_PAGES; i++) {
        size_t page = page_order[i];

        mapping[page * PAGE_SIZE] = (uint8_t)i;
    }
    mfence();
}

static void flush_page(uint8_t *page)
{
    for (size_t line = 0; line < LINES_PER_PAGE; line++)
        flush(page + line * LINE_SIZE);
    mfence();
}

static void run_access_sequence(uint8_t *page, const uint8_t *lines)
{
    for (size_t i = 0; i < ACCESS_COUNT; i++) {
        access_pcs[i](page + (size_t)lines[i] * LINE_SIZE);
        /* Preserve ordering while allowing the prefetcher to observe misses. */
        nops();
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

#if ENABLE_PMU
static int pmu_setup(void)
{
    pmu_fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
    if (pmu_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n",
                PREFETCH_PMU_DEVICE_PATH, strerror(errno));
        return -1;
    }
#if PMU_CORE_X925
    printf("# PMU source=direct-mrs core=Cortex-X925 "
           "counter4=0x8154 counter5=0x8155\n");
#else
    printf("# PMU source=direct-mrs core=Cortex-A725 "
           "counter4=0x8154 counter5=0x8285\n");
#endif
    if (ioctl(pmu_fd, PREFETCH_PMU_IOC_REARM) != 0) {
        fprintf(stderr, "PMU rearm ioctl failed: %s\n", strerror(errno));
        close(pmu_fd);
        pmu_fd = -1;
        return -1;
    }
    return 0;
}

static void pmu_cleanup(void)
{
    if (pmu_fd >= 0)
        close(pmu_fd);
}
#endif

static void run_case(const char *name, enum stream_case which,
                     int rounds_per_line, uint64_t threshold_ns)
{
    const uint8_t *lines = which == CASE_FORWARD ?
                           forward_lines : nondirectional_lines;
    struct case_stats stats = {0};

    /*
     * Rerun the complete experiment for every probed line. The probe itself
     * can train/trigger a prefetcher, so probing a whole page after one run
     * would corrupt the cache map.
     */
    for (size_t probe_line = 0; probe_line < LINES_PER_PAGE; probe_line++) {
        for (int round = 0; round < rounds_per_line; round++) {
            size_t page_index = sample_page(which, probe_line, (size_t)round);
            uint8_t *page = page_addr(page_index);
#if ENABLE_PMU
            struct prefetch_pmu_snapshot before = {0};
            struct prefetch_pmu_snapshot after = {0};
#endif

            cpp_rctx();
            flush_page(page);

#if ENABLE_PMU
            if (pmu_ready)
                before = prefetch_pmu_read();
#endif
            run_access_sequence(page, lines);
            wait_for_prefetch();
#if ENABLE_PMU
            if (pmu_ready) {
                after = prefetch_pmu_read();
                stats.l1_events +=
                    prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
                stats.l2_events +=
                    prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
            }
#endif

#if ENABLE_CACHE_PROBE
            {
                uint64_t latency =
                    timed_reload_ns(page + probe_line * LINE_SIZE);
                struct line_stats *line = &stats.lines[probe_line];

                line->latency_ns += latency;
                line->probes++;
                if (latency <= threshold_ns)
                    line->hits++;
            }
#endif
            stats.samples++;
        }
    }

    printf("\n[%s]\n", name);
    printf("# pattern=");
    for (size_t i = 0; i < ACCESS_COUNT; i++)
        printf("%s%u", i == 0 ? "" : ",", lines[i]);
    printf(" samples=%llu\n", (unsigned long long)stats.samples);
#if ENABLE_PMU
    if (pmu_ready) {
        double denominator = stats.samples == 0 ? 1.0 :
                             (double)stats.samples;

        printf("# PMU %-24s per_sample=%.4f total=%llu\n",
               "l1d_cache_hwprf",
               (double)stats.l1_events / denominator,
               (unsigned long long)stats.l1_events);
        printf("# PMU %-24s per_sample=%.4f total=%llu\n",
               "l2d_cache_hwprf",
               (double)stats.l2_events / denominator,
               (unsigned long long)stats.l2_events);
    }
#endif
#if ENABLE_CACHE_PROBE
    printf("# line\trole\tprobes\thits\tper_1000\tavg_latency_ns\n");
    for (size_t probe_line = 0; probe_line < LINES_PER_PAGE; probe_line++) {
        const struct line_stats *line = &stats.lines[probe_line];
        const char *role = "candidate";
        uint64_t per_1000 = line->probes == 0 ? 0 :
            line->hits * 1000 / line->probes;
        uint64_t average = line->probes == 0 ? 0 :
            line->latency_ns / line->probes;

        for (size_t i = 0; i < ACCESS_COUNT; i++) {
            if (probe_line == lines[i]) {
                role = "accessed";
                break;
            }
        }
        printf("%2zu\t%-9s\t%5llu\t%5llu\t%4llu\t\t%5llu\n",
               probe_line, role,
               (unsigned long long)line->probes,
               (unsigned long long)line->hits,
               (unsigned long long)per_1000,
               (unsigned long long)average);
    }
#endif
}

static void print_usage(const char *program)
{
    fprintf(stderr, "usage: %s [rounds_per_line threshold_ns]\n", program);
}

int main(int argc, char **argv)
{
    int rounds_per_line = DEFAULT_ROUNDS_PER_LINE;
    uint64_t threshold_ns = DEFAULT_THRESHOLD_NS;
    int result = 0;

    if (argc != 1 && argc != 3) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 3) {
        rounds_per_line = atoi(argv[1]);
        threshold_ns = strtoull(argv[2], NULL, 0);
    }
    if (rounds_per_line <= 0 || threshold_ns == 0) {
        print_usage(argv[0]);
        return 1;
    }

    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }

    init_page_order();
    mapping_size = (size_t)POOL_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    prefault_mapping();

    printf("# Stream Prefetcher existence test\n");
    printf("# rounds_per_line=%d threshold_ns=%llu accesses=%d "
           "pool_pages=%d wait_nops=%d cpp_rctx=each_sample\n",
           rounds_per_line, (unsigned long long)threshold_ns,
           ACCESS_COUNT, POOL_PAGES, PREFETCH_WAIT_NOPS);
    printf("# timer=cntvct_el0 cntfrq_el0=%lluHz "
           "enable_pmu=%d enable_cache_probe=%d\n",
           (unsigned long long)timer_freq_hz,
           ENABLE_PMU, ENABLE_CACHE_PROBE);
    printf("# Evidence: forward-only consecutive candidate hits that are "
           "absent in the nondirectional control.\n");

#if ENABLE_PMU
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        fprintf(stderr, "# direct PMU counter setup failed; "
                "cache probe still runs\n");
        result = 2;
    }
#else
    printf("# PMU disabled by ENABLE_PMU=0\n");
#endif

    run_case("nondirectional_control", CASE_NONDIRECTIONAL,
             rounds_per_line, threshold_ns);
    run_case("forward_irregular", CASE_FORWARD,
             rounds_per_line, threshold_ns);

#if ENABLE_PMU
    pmu_cleanup();
#endif
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
