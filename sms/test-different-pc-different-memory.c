#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "This test uses AArch64 DC CIVAC and load/PRFM instructions."
#endif

#define CACHE_LINE_SIZE 64
#define PAGE_SIZE 4096
#define EXTRA_REGION_PAGES 16
#define DEFAULT_ROUNDS 40000
#define DEFAULT_THRESHOLD_NS 150

static const size_t training_lines[] = {4, 1, 6, 7, 3, 11, 15};
static const size_t trigger_line = 4;

static uint8_t delay_array[100 * CACHE_LINE_SIZE] = {0};

static uint8_t *mapping;
static uint8_t *training_region;
static uint8_t *extra_regions;
static uint8_t *test_region;
static size_t mapping_size;
static int use_sw_prefetch;

static inline void mfence(void) {
    asm volatile("DSB SY\nISB" ::: "memory");
}

static inline void flush(void *addr) {
    asm volatile("DC CIVAC, %0" :: "r"(addr) : "memory");
}

static inline void maccess(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

static inline void nop(void) {
    asm volatile("nop");
}

/*
 * PC A: training instruction.
 */
__attribute__((noinline)) static void train_load(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

/*
 * PC B: trigger instruction.
 */
__attribute__((noinline)) static void trigger_load(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

/*
 * PC C: unrelated region pressure, same instruction type as load tests.
 */
__attribute__((noinline)) static void extra_load(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

__attribute__((noinline)) static void train_sw_prefetch(void *addr) {
    asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory");
}

__attribute__((noinline)) static void trigger_sw_prefetch(void *addr) {
    asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory");
}

__attribute__((noinline)) static void extra_sw_prefetch(void *addr) {
    asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory");
}

static inline void train_access(void *addr) {
    if (use_sw_prefetch) {
        train_sw_prefetch(addr);
    } else {
        train_load(addr);
    }
}

static inline void trigger_access(void *addr) {
    if (use_sw_prefetch) {
        trigger_sw_prefetch(addr);
    } else {
        trigger_load(addr);
    }
}

static inline void extra_region_access(void *addr) {
    if (use_sw_prefetch) {
        extra_sw_prefetch(addr);
    } else {
        extra_load(addr);
    }
}

static uint64_t timestamp_ns(void) {
    struct timespec t;
    mfence();
    clock_gettime(CLOCK_MONOTONIC, &t);
    mfence();
    return t.tv_sec * 1000ULL * 1000ULL * 1000ULL + t.tv_nsec;
}

static void delay_before_probe(void) {
    uint64_t dummy = 0;
    for (int k = 0; k < 100; k++) {
        dummy += delay_array[k * CACHE_LINE_SIZE];
        mfence();
    }
    for (int i = 0; i < 100; i++) {
        nop();
    }
    mfence();
    (void)dummy;
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
        flush_region(training_region, PAGE_SIZE);
        flush_region(extra_regions, EXTRA_REGION_PAGES * PAGE_SIZE);
        flush_region(test_region, PAGE_SIZE);

        for (size_t i = 0; i < sizeof(training_lines) / sizeof(training_lines[0]); i++) {
            train_access(training_region + training_lines[i] * CACHE_LINE_SIZE);
        }
        mfence();

        if (touch_extra_regions) {
            for (size_t page = 0; page < EXTRA_REGION_PAGES; page++) {
                extra_region_access(extra_regions + page * PAGE_SIZE);
            }
            mfence();
        }

        trigger_access(test_region + trigger_line * CACHE_LINE_SIZE);
        mfence();

        size_t probe_line = (size_t)round % lines_per_page;
        delay_before_probe();
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
    fprintf(stderr, "usage: %s [rounds threshold_ns [load|sw]]\n", prog);
    fprintf(stderr, "default: rounds=%d threshold_ns=%d access=load\n",
            DEFAULT_ROUNDS, DEFAULT_THRESHOLD_NS);
}

int main(int argc, char **argv) {
    int rounds = DEFAULT_ROUNDS;
    uint64_t hit_threshold_ns = DEFAULT_THRESHOLD_NS;

    if (argc != 1 && argc != 3 && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 3) {
        rounds = atoi(argv[1]);
        hit_threshold_ns = strtoull(argv[2], NULL, 0);
    }
    if (argc == 4) {
        if (strcmp(argv[3], "load") == 0) {
            use_sw_prefetch = 0;
        } else if (strcmp(argv[3], "sw") == 0) {
            use_sw_prefetch = 1;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (rounds <= 0 || hit_threshold_ns == 0) {
        print_usage(argv[0]);
        return 1;
    }

    mapping_size = (EXTRA_REGION_PAGES + 2) * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }

    memset(mapping, 0xff, mapping_size);
    training_region = mapping;
    extra_regions = mapping + PAGE_SIZE;
    test_region = mapping + (EXTRA_REGION_PAGES + 1) * PAGE_SIZE;

    touch_region(mapping, mapping_size);
    flush_region(mapping, mapping_size);

    printf("# SMS different-PC different-memory test\n");
    printf("# access mode: %s\n",
           use_sw_prefetch ? "software prefetch (prfm pldl1keep)" : "load (ldrb)");
    printf("# training lines in training_region via PC A: 4, 1, 6, 7, 3, 11, 15\n");
    printf("# trigger line in test_region via PC B: 4\n");
    printf("# expected prefetch lines in test_region: 1, 3, 6, 7, 11, 15\n");

    run_test("without_extra_region_accesses", 0, rounds, hit_threshold_ns);
    run_test("with_16_extra_region_accesses", 1, rounds, hit_threshold_ns);

    munmap(mapping, mapping_size);
    return 0;
}
