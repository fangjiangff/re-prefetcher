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
#define TRAINING_REGION_PAGES 32
#define DEFAULT_ROUNDS 40000
#define DEFAULT_THRESHOLD_NS 150
#define DEFAULT_ENTRIES 8
#define MAX_EXTRA_ENTRY_FUNCS 32

static const size_t training_lines[] = {4, 1, 6, 7, 10};
static const size_t extra_lines[] = {5, 1, 3, 9};
static const size_t trigger_line = 4;

static uint8_t delay_array[100 * CACHE_LINE_SIZE] = {0};

static uint8_t *mapping;
static uint8_t *training_region;
static uint8_t *test_region;
static size_t mapping_size;
static int use_sw_prefetch;

typedef void (*access_func_t)(void *);

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
 * Target PC: split target training and final trigger both use this PC.
 */
__attribute__((noinline)) static void target_load(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

__attribute__((noinline)) static void target_sw_prefetch(void *addr) {
    asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory");
}

#define DEFINE_EXTRA_FUNCS(N) \
    __attribute__((noinline)) static void extra_load_##N(void *addr) { \
        asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0"); \
    } \
    __attribute__((noinline)) static void extra_sw_##N(void *addr) { \
        asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory"); \
    }

DEFINE_EXTRA_FUNCS(0)
DEFINE_EXTRA_FUNCS(1)
DEFINE_EXTRA_FUNCS(2)
DEFINE_EXTRA_FUNCS(3)
DEFINE_EXTRA_FUNCS(4)
DEFINE_EXTRA_FUNCS(5)
DEFINE_EXTRA_FUNCS(6)
DEFINE_EXTRA_FUNCS(7)
DEFINE_EXTRA_FUNCS(8)
DEFINE_EXTRA_FUNCS(9)
DEFINE_EXTRA_FUNCS(10)
DEFINE_EXTRA_FUNCS(11)
DEFINE_EXTRA_FUNCS(12)
DEFINE_EXTRA_FUNCS(13)
DEFINE_EXTRA_FUNCS(14)
DEFINE_EXTRA_FUNCS(15)
DEFINE_EXTRA_FUNCS(16)
DEFINE_EXTRA_FUNCS(17)
DEFINE_EXTRA_FUNCS(18)
DEFINE_EXTRA_FUNCS(19)
DEFINE_EXTRA_FUNCS(20)
DEFINE_EXTRA_FUNCS(21)
DEFINE_EXTRA_FUNCS(22)
DEFINE_EXTRA_FUNCS(23)
DEFINE_EXTRA_FUNCS(24)
DEFINE_EXTRA_FUNCS(25)
DEFINE_EXTRA_FUNCS(26)
DEFINE_EXTRA_FUNCS(27)
DEFINE_EXTRA_FUNCS(28)
DEFINE_EXTRA_FUNCS(29)
DEFINE_EXTRA_FUNCS(30)
DEFINE_EXTRA_FUNCS(31)

static access_func_t extra_load_funcs[MAX_EXTRA_ENTRY_FUNCS] = {
    extra_load_0, extra_load_1, extra_load_2, extra_load_3,
    extra_load_4, extra_load_5, extra_load_6, extra_load_7,
    extra_load_8, extra_load_9, extra_load_10, extra_load_11,
    extra_load_12, extra_load_13, extra_load_14, extra_load_15,
    extra_load_16, extra_load_17, extra_load_18, extra_load_19,
    extra_load_20, extra_load_21, extra_load_22, extra_load_23,
    extra_load_24, extra_load_25, extra_load_26, extra_load_27,
    extra_load_28, extra_load_29, extra_load_30, extra_load_31,
};

static access_func_t extra_sw_funcs[MAX_EXTRA_ENTRY_FUNCS] = {
    extra_sw_0, extra_sw_1, extra_sw_2, extra_sw_3,
    extra_sw_4, extra_sw_5, extra_sw_6, extra_sw_7,
    extra_sw_8, extra_sw_9, extra_sw_10, extra_sw_11,
    extra_sw_12, extra_sw_13, extra_sw_14, extra_sw_15,
    extra_sw_16, extra_sw_17, extra_sw_18, extra_sw_19,
    extra_sw_20, extra_sw_21, extra_sw_22, extra_sw_23,
    extra_sw_24, extra_sw_25, extra_sw_26, extra_sw_27,
    extra_sw_28, extra_sw_29, extra_sw_30, extra_sw_31,
};

static inline void target_access(void *addr) {
    if (use_sw_prefetch) {
        target_sw_prefetch(addr);
    } else {
        target_load(addr);
    }
}

static inline void extra_entry_access(size_t entry, void *addr) {
    if (use_sw_prefetch) {
        extra_sw_funcs[entry](addr);
    } else {
        extra_load_funcs[entry](addr);
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

static void run_test(size_t entries, int rounds, uint64_t hit_threshold_ns) {
    const size_t lines_per_page = PAGE_SIZE / CACHE_LINE_SIZE;
    int hits[PAGE_SIZE / CACHE_LINE_SIZE];

    memset(hits, 0, sizeof(hits));

    for (int round = 0; round < rounds; round++) {
        flush_region(training_region, TRAINING_REGION_PAGES * PAGE_SIZE);
        flush_region(test_region, PAGE_SIZE);

        for (size_t i = 0; i + 1 < sizeof(training_lines) / sizeof(training_lines[0]); i++) {
            target_access(training_region + training_lines[i] * CACHE_LINE_SIZE);
        }
        mfence();

        for (size_t entry = 0; entry < entries; entry++) {
            for (size_t i = 0; i < sizeof(extra_lines) / sizeof(extra_lines[0]); i++) {
                extra_entry_access(entry, training_region + entry * PAGE_SIZE + extra_lines[i] * CACHE_LINE_SIZE);
            }
            mfence();
        }

        target_access(training_region + training_lines[(sizeof(training_lines) / sizeof(training_lines[0])) - 1] * CACHE_LINE_SIZE);
        mfence();

        target_access(test_region + trigger_line * CACHE_LINE_SIZE);
        mfence();

        size_t probe_line = (size_t)round % lines_per_page;
        delay_before_probe();
        uint64_t t = reload_time_ns(test_region + probe_line * CACHE_LINE_SIZE);
        if (t <= hit_threshold_ns) {
            hits[probe_line]++;
        }
    }

    printf("# SMS training-entries test\n");
    printf("# access mode: %s\n",
           use_sw_prefetch ? "software prefetch (prfm pldl1keep)" : "load (ldrb)");
    printf("# entries=%zu rounds=%d threshold_ns=%lu\n", entries, rounds, (unsigned long)hit_threshold_ns);
    printf("# training lines in target entry: 4, 1, 6, 7, 10; trigger line in test_region: 4\n");
    printf("# extra-entry lines: 5, 1, 3, 9\n");
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
    fprintf(stderr, "usage: %s [rounds threshold_ns entries [load|sw]]\n", prog);
    fprintf(stderr, "default: rounds=%d threshold_ns=%d entries=%d access=load\n",
            DEFAULT_ROUNDS, DEFAULT_THRESHOLD_NS, DEFAULT_ENTRIES);
}

int main(int argc, char **argv) {
    int rounds = DEFAULT_ROUNDS;
    uint64_t hit_threshold_ns = DEFAULT_THRESHOLD_NS;
    size_t entries = DEFAULT_ENTRIES;

    if (argc != 1 && argc != 4 && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 4) {
        rounds = atoi(argv[1]);
        hit_threshold_ns = strtoull(argv[2], NULL, 0);
        entries = strtoull(argv[3], NULL, 0);
    }
    if (argc == 5) {
        if (strcmp(argv[4], "load") == 0) {
            use_sw_prefetch = 0;
        } else if (strcmp(argv[4], "sw") == 0) {
            use_sw_prefetch = 1;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (rounds <= 0 || hit_threshold_ns == 0 || entries >= MAX_EXTRA_ENTRY_FUNCS ||
        entries >= TRAINING_REGION_PAGES) {
        print_usage(argv[0]);
        return 1;
    }

    mapping_size = (TRAINING_REGION_PAGES + 1) * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }

    memset(mapping, 0xff, mapping_size);
    training_region = mapping;
    test_region = mapping + TRAINING_REGION_PAGES * PAGE_SIZE;

    touch_region(mapping, mapping_size);
    flush_region(mapping, mapping_size);

    run_test(entries, rounds, hit_threshold_ns);

    munmap(mapping, mapping_size);
    return 0;
}
