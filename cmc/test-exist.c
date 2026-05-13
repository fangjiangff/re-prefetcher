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
#define MAPPING_PAGES 256
#define DEFAULT_ROUNDS 40000
#define DEFAULT_THRESHOLD_NS 150
#define DEFAULT_TRAINING_REPLAYS 128

struct node {
    size_t page;
    size_t line;
};

/*
 * One trigger followed by an irregular temporal miss chain.
 *
 * The nodes intentionally live in different pages and use non-monotonic page
 * deltas. That keeps this test away from same-region/SMS-style prefetching and
 * away from regular-stride prefetching.
 */
static const struct node chain_nodes[] = {
    {  7, 11}, /* trigger */
    { 43,  3},
    { 91, 37},
    { 18, 52},
    {133,  9},
    { 56, 27},
    {199, 60},
    { 25, 14},
    {166, 45},
    { 72,  6},
    {213, 33},
    {109, 58},
    {240, 21},
    {  5, 49},
    {151,  2},
    {117, 40},
    {224, 17},
};

static const struct node control_nodes[] = {
    { 31, 23},
    { 64, 51},
    { 98,  8},
    {125, 35},
    {182, 12},
    {232, 54},
    { 14, 30},
    {207,  4},
};

static uint8_t delay_array[100 * CACHE_LINE_SIZE] = {0};

static uint8_t *mapping;
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
 * Keep all chain accesses at one stable load PC. The access addresses are
 * deliberately irregular, so a PC-localized stride prefetcher should not lock
 * on to this stream.
 */
__attribute__((noinline)) static void chain_load(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

__attribute__((noinline)) static void chain_sw_prefetch(void *addr) {
    asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory");
}

__attribute__((noinline)) static void *chain_step(void *addr) {
    void *next;
    asm volatile("ldr %0, [%1]\n\t" : "=r"(next) : "r"(addr) : "memory");
    return next;
}

static inline void chain_access(void *addr) {
    if (use_sw_prefetch) {
        chain_sw_prefetch(addr);
    } else {
        chain_load(addr);
    }
}

static uint8_t *addr_for_node(struct node n) {
    return mapping + n.page * PAGE_SIZE + n.line * CACHE_LINE_SIZE;
}

static void init_pointer_chain(void) {
    const size_t chain_count = sizeof(chain_nodes) / sizeof(chain_nodes[0]);

    for (size_t i = 0; i < chain_count; i++) {
        uint8_t *next = addr_for_node(chain_nodes[(i + 1) % chain_count]);
        memcpy(addr_for_node(chain_nodes[i]), &next, sizeof(next));
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

static void flush_nodes(const struct node *nodes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        flush(addr_for_node(nodes[i]));
    }
    mfence();
}

static void touch_region(uint8_t *base, size_t size) {
    for (size_t offset = 0; offset < size; offset += CACHE_LINE_SIZE) {
        maccess(base + offset);
    }
    mfence();
}

static void validate_nodes(void) {
    const size_t chain_count = sizeof(chain_nodes) / sizeof(chain_nodes[0]);
    const size_t control_count = sizeof(control_nodes) / sizeof(control_nodes[0]);
    long prev_delta = 0;

    for (size_t i = 0; i < chain_count; i++) {
        if (chain_nodes[i].page >= MAPPING_PAGES ||
            chain_nodes[i].line >= PAGE_SIZE / CACHE_LINE_SIZE) {
            fprintf(stderr, "invalid chain node %zu\n", i);
            exit(1);
        }
        for (size_t j = i + 1; j < chain_count; j++) {
            if (chain_nodes[i].page == chain_nodes[j].page) {
                fprintf(stderr, "chain nodes %zu and %zu share one page\n", i, j);
                exit(1);
            }
        }
        if (i > 0) {
            long delta = (long)chain_nodes[i].page - (long)chain_nodes[i - 1].page;
            if (delta == 0 || delta == prev_delta || delta == 1 || delta == -1) {
                fprintf(stderr, "chain page deltas are too regular near node %zu\n", i);
                exit(1);
            }
            prev_delta = delta;
        }
    }

    for (size_t i = 0; i < control_count; i++) {
        if (control_nodes[i].page >= MAPPING_PAGES ||
            control_nodes[i].line >= PAGE_SIZE / CACHE_LINE_SIZE) {
            fprintf(stderr, "invalid control node %zu\n", i);
            exit(1);
        }
        for (size_t j = 0; j < chain_count; j++) {
            if (control_nodes[i].page == chain_nodes[j].page) {
                fprintf(stderr, "control node %zu shares page with chain node %zu\n", i, j);
                exit(1);
            }
        }
    }
}

static void train_cmc_sequence(int training_replays) {
    const size_t chain_count = sizeof(chain_nodes) / sizeof(chain_nodes[0]);

    for (int replay = 0; replay < training_replays; replay++) {
        flush_nodes(chain_nodes, chain_count);
        void *addr = addr_for_node(chain_nodes[0]);
        for (size_t i = 0; i < chain_count; i++) {
            if (use_sw_prefetch) {
                chain_sw_prefetch(addr);
            }
            addr = chain_step(addr);
        }
        mfence();
    }
}

static void run_test(int rounds, uint64_t hit_threshold_ns,
                     int training_replays) {
    const size_t chain_count = sizeof(chain_nodes) / sizeof(chain_nodes[0]);
    const size_t target_count = chain_count - 1;
    const size_t control_count = sizeof(control_nodes) / sizeof(control_nodes[0]);
    int depth_hits[sizeof(chain_nodes) / sizeof(chain_nodes[0]) - 1];
    int control_hits[sizeof(control_nodes) / sizeof(control_nodes[0])];

    memset(depth_hits, 0, sizeof(depth_hits));
    memset(control_hits, 0, sizeof(control_hits));

    for (int round = 0; round < rounds; round++) {
        train_cmc_sequence(training_replays);

        flush_nodes(chain_nodes, chain_count);
        flush_nodes(control_nodes, control_count);

        size_t depth = 1 + ((size_t)round % target_count);
        void *addr = addr_for_node(chain_nodes[0]);
        for (size_t step = 0; step < depth; step++) {
            if (use_sw_prefetch) {
                chain_sw_prefetch(addr);
            }
            addr = chain_step(addr);
        }
        mfence();

        delay_before_probe();
        uint64_t t = reload_time_ns(addr_for_node(chain_nodes[depth]));
        if (t <= hit_threshold_ns) {
            depth_hits[depth - 1]++;
        }

        size_t control = (size_t)round % control_count;
        delay_before_probe();
        t = reload_time_ns(addr_for_node(control_nodes[control]));
        if (t <= hit_threshold_ns) {
            control_hits[control]++;
        }
    }

    printf("# CMC existence test\n");
    printf("# access mode: %s\n",
           use_sw_prefetch ? "software prefetch plus pointer load" : "pointer load (ldr)");
    printf("# rounds=%d threshold_ns=%lu training_replays=%d\n",
           rounds, (unsigned long)hit_threshold_ns, training_replays);
    printf("# trigger: node=0 page=%zu line=%zu\n",
           chain_nodes[0].page, chain_nodes[0].line);
    printf("# chain nodes use one line per page with irregular page deltas;\n");
    printf("# each row executes depth dependent loads, then probes the next chain node.\n");
    printf("# sustained next-node hits above controls suggest CMC/history prefetching.\n");

    int probes_per_depth = rounds / (int)target_count;
    int probes_per_control_node = rounds / (int)control_count;
    if (probes_per_depth <= 0) {
        probes_per_depth = 1;
    }
    if (probes_per_control_node <= 0) {
        probes_per_control_node = 1;
    }

    printf("\n[depth_next]\n");
    printf("# depth\tpage\tline\trole\thits\tper_1000\n");
    for (size_t depth = 1; depth <= target_count; depth++) {
        printf("%2zu\t%3zu\t%2zu\tnext_after_depth\t%5d\t%4d\n",
               depth,
               chain_nodes[depth].page,
               chain_nodes[depth].line,
               depth_hits[depth - 1],
               depth_hits[depth - 1] * 1000 / probes_per_depth);
    }

    printf("\n[controls]\n");
    printf("# idx\tpage\tline\trole\thits\tper_1000\n");
    for (size_t i = 0; i < control_count; i++) {
        printf("%2zu\t%3zu\t%2zu\tcontrol\t\t%5d\t%4d\n",
               i,
               control_nodes[i].page,
               control_nodes[i].line,
               control_hits[i],
               control_hits[i] * 1000 / probes_per_control_node);
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr, "usage: %s [rounds threshold_ns training_replays [load|sw]]\n", prog);
    fprintf(stderr, "default: rounds=%d threshold_ns=%d training_replays=%d access=load\n",
            DEFAULT_ROUNDS, DEFAULT_THRESHOLD_NS, DEFAULT_TRAINING_REPLAYS);
}

int main(int argc, char **argv) {
    int rounds = DEFAULT_ROUNDS;
    uint64_t hit_threshold_ns = DEFAULT_THRESHOLD_NS;
    int training_replays = DEFAULT_TRAINING_REPLAYS;

    if (argc != 1 && argc != 4 && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 4) {
        rounds = atoi(argv[1]);
        hit_threshold_ns = strtoull(argv[2], NULL, 0);
        training_replays = atoi(argv[3]);
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
    if (rounds <= 0 || hit_threshold_ns == 0 || training_replays <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    validate_nodes();

    mapping_size = MAPPING_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }

    memset(mapping, 0xff, mapping_size);
    init_pointer_chain();
    touch_region(mapping, mapping_size);
    flush_nodes(chain_nodes, sizeof(chain_nodes) / sizeof(chain_nodes[0]));
    flush_nodes(control_nodes, sizeof(control_nodes) / sizeof(control_nodes[0]));

    run_test(rounds, hit_threshold_ns, training_replays);

    munmap(mapping, mapping_size);
    return 0;
}
