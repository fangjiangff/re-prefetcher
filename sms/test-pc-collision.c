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
#define GAP_REGION_PAGES 16
#define DEFAULT_ROUNDS 40000
#define DEFAULT_THRESHOLD_NS 150
#define DEFAULT_COLLIDING_BITS 12

static const size_t training_lines[] = {4, 1, 6, 7};
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

#define DEFINE_COLLISION_FUNCS(BITS) \
    __attribute__((aligned(1 << BITS), noinline)) static void load_##BITS##_train(void *addr) { \
        asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0"); \
    } \
    __attribute__((aligned(1 << BITS), noinline)) static void load_##BITS##_trigger(void *addr) { \
        asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0"); \
    } \
    __attribute__((aligned(1 << BITS), noinline)) static void sw_##BITS##_train(void *addr) { \
        asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory"); \
    } \
    __attribute__((aligned(1 << BITS), noinline)) static void sw_##BITS##_trigger(void *addr) { \
        asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory"); \
    }

DEFINE_COLLISION_FUNCS(5)
DEFINE_COLLISION_FUNCS(6)
DEFINE_COLLISION_FUNCS(7)
DEFINE_COLLISION_FUNCS(8)
DEFINE_COLLISION_FUNCS(9)
DEFINE_COLLISION_FUNCS(10)
DEFINE_COLLISION_FUNCS(11)
DEFINE_COLLISION_FUNCS(12)
DEFINE_COLLISION_FUNCS(13)
DEFINE_COLLISION_FUNCS(14)
DEFINE_COLLISION_FUNCS(15)
DEFINE_COLLISION_FUNCS(16)
DEFINE_COLLISION_FUNCS(17)
DEFINE_COLLISION_FUNCS(18)
DEFINE_COLLISION_FUNCS(19)
DEFINE_COLLISION_FUNCS(20)
DEFINE_COLLISION_FUNCS(21)
DEFINE_COLLISION_FUNCS(22)
DEFINE_COLLISION_FUNCS(23)
DEFINE_COLLISION_FUNCS(24)

static int select_access_pair(size_t bits, access_func_t *train, access_func_t *trigger) {
#define SELECT_CASE(BITS) \
    case BITS: \
        if (use_sw_prefetch) { \
            *train = sw_##BITS##_train; \
            *trigger = sw_##BITS##_trigger; \
        } else { \
            *train = load_##BITS##_train; \
            *trigger = load_##BITS##_trigger; \
        } \
        return 0

    switch (bits) {
        SELECT_CASE(5);
        SELECT_CASE(6);
        SELECT_CASE(7);
        SELECT_CASE(8);
        SELECT_CASE(9);
        SELECT_CASE(10);
        SELECT_CASE(11);
        SELECT_CASE(12);
        SELECT_CASE(13);
        SELECT_CASE(14);
        SELECT_CASE(15);
        SELECT_CASE(16);
        SELECT_CASE(17);
        SELECT_CASE(18);
        SELECT_CASE(19);
        SELECT_CASE(20);
        SELECT_CASE(21);
        SELECT_CASE(22);
        SELECT_CASE(23);
        SELECT_CASE(24);
        default:
            return -1;
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

static void print_pc_info(size_t bits, access_func_t train, access_func_t trigger) {
    uintptr_t mask = (1ULL << bits) - 1;
    printf("# train_pc=0x%lx trigger_pc=0x%lx colliding_bits=%zu pc_lsb_equal=%d\n",
           (unsigned long)(uintptr_t)train,
           (unsigned long)(uintptr_t)trigger,
           bits,
           (((uintptr_t)train & mask) == ((uintptr_t)trigger & mask)) ? 1 : 0);
}

static void run_test(size_t colliding_bits, int rounds, uint64_t hit_threshold_ns) {
    const size_t lines_per_page = PAGE_SIZE / CACHE_LINE_SIZE;
    int hits[PAGE_SIZE / CACHE_LINE_SIZE];
    access_func_t train_access = NULL;
    access_func_t trigger_access = NULL;

    if (select_access_pair(colliding_bits, &train_access, &trigger_access) != 0) {
        fprintf(stderr, "colliding_bits must be in [5, 24]\n");
        exit(1);
    }

    memset(hits, 0, sizeof(hits));

    for (int round = 0; round < rounds; round++) {
        flush_region(training_region, PAGE_SIZE);
        flush_region(test_region, PAGE_SIZE);

        for (size_t i = 0; i < sizeof(training_lines) / sizeof(training_lines[0]); i++) {
            train_access(training_region + training_lines[i] * CACHE_LINE_SIZE);
        }
        mfence();

        trigger_access(test_region + trigger_line * CACHE_LINE_SIZE);
        mfence();

        size_t probe_line = (size_t)round % lines_per_page;
        delay_before_probe();
        uint64_t t = reload_time_ns(test_region + probe_line * CACHE_LINE_SIZE);
        if (t <= hit_threshold_ns) {
            hits[probe_line]++;
        }
    }

    printf("# SMS PC-collision test\n");
    printf("# access mode: %s\n",
           use_sw_prefetch ? "software prefetch (prfm pldl1keep)" : "load (ldrb)");
    printf("# rounds=%d threshold_ns=%lu\n", rounds, (unsigned long)hit_threshold_ns);
    print_pc_info(colliding_bits, train_access, trigger_access);
    printf("# training lines in training_region: 4, 1, 6, 7; trigger line in test_region: 4\n");
    printf("# expected prefetch lines in test_region: 1, 6, 7\n");
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
    fprintf(stderr, "usage: %s [rounds threshold_ns colliding_bits [load|sw]]\n", prog);
    fprintf(stderr, "default: rounds=%d threshold_ns=%d colliding_bits=%d access=load\n",
            DEFAULT_ROUNDS, DEFAULT_THRESHOLD_NS, DEFAULT_COLLIDING_BITS);
}

int main(int argc, char **argv) {
    int rounds = DEFAULT_ROUNDS;
    uint64_t hit_threshold_ns = DEFAULT_THRESHOLD_NS;
    size_t colliding_bits = DEFAULT_COLLIDING_BITS;

    if (argc != 1 && argc != 4 && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 4) {
        rounds = atoi(argv[1]);
        hit_threshold_ns = strtoull(argv[2], NULL, 0);
        colliding_bits = strtoull(argv[3], NULL, 0);
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
    if (rounds <= 0 || hit_threshold_ns == 0 || colliding_bits < 5 || colliding_bits > 24) {
        print_usage(argv[0]);
        return 1;
    }

    mapping_size = (GAP_REGION_PAGES + 1) * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }

    memset(mapping, 0xff, mapping_size);
    training_region = mapping;
    test_region = mapping + GAP_REGION_PAGES * PAGE_SIZE;

    touch_region(mapping, mapping_size);
    flush_region(mapping, mapping_size);

    run_test(colliding_bits, rounds, hit_threshold_ns);

    munmap(mapping, mapping_size);
    return 0;
}
