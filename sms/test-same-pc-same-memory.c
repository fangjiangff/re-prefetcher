#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "This test uses AArch64 DC CIVAC and load instructions."
#endif

#define CACHE_LINE_SIZE 64
#define PAGE_SIZE 4096
#define EXTRA_REGION_PAGES 16
#define DEFAULT_ROUNDS 40000
#define DEFAULT_THRESHOLD_NS 150

#ifndef USE_SW_PREFETCH
#define USE_SW_PREFETCH 0
#endif

static const size_t training_lines[] = {4, 1, 6, 7, 3, 11, 15};
static const size_t trigger_line = 4;

static uint8_t *mapping;
static uint8_t *extra_regions;
static uint8_t *test_region;
static size_t mapping_size;

static inline void mfence(void) {
    asm volatile("DSB SY\nISB" ::: "memory");
}
// 把某个地址所在 cache line clean + invalidate，等价于“从缓存里赶出去”
static inline void flush(void *addr) {
    asm volatile("DC CIVAC, %0" :: "r"(addr) : "memory");
}

static inline void maccess(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

/*
 * Keep the training and trigger memory accesses at the same instruction PC.
 * Every call enters this function and executes this one load instruction.
 */
__attribute__((noinline, unused)) static void same_pc_load(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

/*
 * Software-prefetch version with a stable instruction PC.
 * Use -DUSE_SW_PREFETCH=1 to train and trigger with this PRFM instruction.
 */
__attribute__((noinline, unused)) static void same_sw_prefetch(void *addr) {
    asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory");
}

static inline void train_or_trigger_access(void *addr) {
#if USE_SW_PREFETCH
    same_sw_prefetch(addr);
#else
    same_pc_load(addr);
#endif
}

// 测一次访问 addr 花了多久
static uint64_t timestamp_ns(void) {
    struct timespec t;
    mfence();
    clock_gettime(CLOCK_MONOTONIC, &t);
    mfence();
    return t.tv_sec * 1000ULL * 1000ULL * 1000ULL + t.tv_nsec;
}

static uint64_t reload_time_ns(void *addr) {
    uint64_t start;
    uint64_t end;
    mfence();
    start = timestamp_ns();
    maccess(addr);
    end = timestamp_ns();
    mfence();
    flush(addr);
    return end - start;
}

static void flush_region(uint8_t *base, size_t size) {
    for (size_t offset = 0; offset < size; offset += CACHE_LINE_SIZE) {
        flush(base + offset);
    }
    mfence();
}

static void touch_region(uint8_t *base, size_t size) {
    for (size_t offset = 0; offset < size; offset += CACHE_LINE_SIZE) {
        maccess(base + offset);
    }
    mfence();
}

static int is_training_line(size_t line) {
    for (size_t i = 0; i < sizeof(training_lines) / sizeof(training_lines[0]); i++) {
        if (training_lines[i] == line) {
            return 1;
        }
    }
    return 0;
}

static void run_test(const char *name, int touch_extra_regions,
                     int rounds, uint64_t hit_threshold_ns) {
    const size_t lines_per_page = PAGE_SIZE / CACHE_LINE_SIZE;
    int hits[PAGE_SIZE / CACHE_LINE_SIZE];

    memset(hits, 0, sizeof(hits));

    for (int round = 0; round < rounds; round++) {
        flush_region(extra_regions, EXTRA_REGION_PAGES * PAGE_SIZE);
        flush_region(test_region, PAGE_SIZE);

        for (size_t i = 0; i < sizeof(training_lines) / sizeof(training_lines[0]); i++) {
            train_or_trigger_access(test_region + training_lines[i] * CACHE_LINE_SIZE);
        }
        mfence();

        if (touch_extra_regions) {
            for (size_t page = 0; page < EXTRA_REGION_PAGES; page++) {
                maccess(extra_regions + page * PAGE_SIZE);
            }
            mfence();
        }

        flush_region(test_region, PAGE_SIZE);

        train_or_trigger_access(test_region + trigger_line * CACHE_LINE_SIZE);
        mfence();

        size_t probe_line = (size_t)round % lines_per_page;
        uint64_t t = reload_time_ns(test_region + probe_line * CACHE_LINE_SIZE);
        if (t <= hit_threshold_ns) {
            hits[probe_line]++;
        }
    }

    printf("\n[%s]\n", name);
    printf("# rounds=%d threshold_ns=%lu\n", rounds, (unsigned long)hit_threshold_ns);
    printf("# line\trole\thits\tper_1000\n");

    int probes_per_line = rounds / (int)lines_per_page;
    if (probes_per_line <= 0) {
        probes_per_line = 1;
    }

    for (size_t line = 0; line < lines_per_page; line++) {
        const char *role = ".";
        if (line == trigger_line) {
            role = "trigger";
        } else if (is_training_line(line)) {
            role = "expected_prefetch";
        }
        printf("%2zu\t%-17s\t%5d\t%4d\n",
               line, role, hits[line], hits[line] * 1000 / probes_per_line);
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr, "usage: %s [rounds threshold_ns]\n", prog);
    fprintf(stderr, "default: rounds=%d threshold_ns=%d\n",
            DEFAULT_ROUNDS, DEFAULT_THRESHOLD_NS);
}

int main(int argc, char **argv) {
    int rounds = DEFAULT_ROUNDS;
    uint64_t hit_threshold_ns = DEFAULT_THRESHOLD_NS;

    if (argc != 1 && argc != 3) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 3) {
        rounds = atoi(argv[1]);
        hit_threshold_ns = strtoull(argv[2], NULL, 0);
    }
    if (rounds <= 0 || hit_threshold_ns == 0) {
        print_usage(argv[0]);
        return 1;
    }

    mapping_size = (EXTRA_REGION_PAGES + 1) * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }

    memset(mapping, 0xff, mapping_size);
    extra_regions = mapping;
    test_region = mapping + EXTRA_REGION_PAGES * PAGE_SIZE;

    touch_region(mapping, mapping_size);
    flush_region(mapping, mapping_size);

    printf("# SMS same-PC same-memory test\n");
    printf("# access mode: %s\n", USE_SW_PREFETCH ? "software prefetch (prfm pldl1keep)" : "load (ldrb)");
    printf("# training lines: 4, 1, 6, 7, 3, 11, 15; trigger line: 4\n");
    printf("# expected SMS prefetch lines after trigger: 1, 3, 6, 7, 11, 15\n");

    run_test("without_extra_region_accesses", 0, rounds, hit_threshold_ns);
    run_test("with_16_extra_region_accesses", 1, rounds, hit_threshold_ns);

    munmap(mapping, mapping_size);
    return 0;
}
