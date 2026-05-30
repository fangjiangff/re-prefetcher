#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "This test uses AArch64 DC CIVAC and CNTVCT_EL0 instructions."
#endif

#define CACHE_LINE_SIZE 64
#define PAGE_SIZE 4096
#define LINES_PER_PAGE (PAGE_SIZE / CACHE_LINE_SIZE)
#define MAPPING_PAGES 512
#define CHAIN_NODE_COUNT 128
#define CONTROL_NODE_COUNT 16
#define DEFAULT_ROUNDS 40000
#define DEFAULT_THRESHOLD_NS 120
#define DEFAULT_TRAINING_REPLAYS 1
#define NODE_SEED 0x9e3779b97f4a7c15ULL

struct node {
    size_t page;
    size_t line;
};

/*
 * The original 12-node handwritten chain is replaced by a deterministic
 * 128-node irregular chain. Each node uses one cache line in a distinct page.
 * This keeps the access stream pointer-dependent and irregular while giving a
 * temporal/correlation prefetcher enough history to learn a longer stream.
 */
static struct node chain_nodes[CHAIN_NODE_COUNT];
static struct node control_nodes[CONTROL_NODE_COUNT];

static uint8_t delay_array[100 * CACHE_LINE_SIZE] = {0};

static uint8_t *mapping;
static size_t mapping_size;
static uint64_t timer_freq_hz;
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

static inline uint64_t splitmix64_next(uint64_t *x) {
    uint64_t z;

    *x += 0x9e3779b97f4a7c15ULL;
    z = *x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static inline uint64_t read_cntvct_el0(void) {
    uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(v) :: "memory");
    return v;
}

static inline uint64_t read_cntfrq_el0(void) {
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t ticks_to_ns(uint64_t ticks) {
    __uint128_t prod = (__uint128_t)ticks * 1000000000ULL;
    prod += timer_freq_hz / 2;
    return (uint64_t)(prod / timer_freq_hz);
}

/*
 * Keep all pointer-chain accesses at one stable load PC. This is important
 * when testing whether a temporal prefetcher uses PC-localized streams.
 */
__attribute__((noinline)) static void chain_sw_prefetch(void *addr) {
    asm volatile("prfm pldl1keep, [%0]\n\t" :: "r"(addr) : "memory");
}

__attribute__((noinline)) static void *chain_step(void *addr) {
    void *next;
    asm volatile("ldr %0, [%1]\n\t" : "=r"(next) : "r"(addr) : "memory");
    return next;
}

static inline void *chain_access_step(void *addr) {
    if (use_sw_prefetch) {
        chain_sw_prefetch(addr);
    }
    return chain_step(addr);
}

static uint8_t *addr_for_node(struct node n) {
    return mapping + n.page * PAGE_SIZE + n.line * CACHE_LINE_SIZE;
}

static void init_nodes(void) {
    uint64_t rng = NODE_SEED;
    size_t pages[MAPPING_PAGES];

    for (size_t i = 0; i < MAPPING_PAGES; i++) {
        pages[i] = i;
    }

    for (size_t i = MAPPING_PAGES - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(&rng) % (i + 1));
        size_t tmp = pages[i];
        pages[i] = pages[j];
        pages[j] = tmp;
    }

    for (size_t i = 0; i < CHAIN_NODE_COUNT; i++) {
        chain_nodes[i].page = pages[i];
        chain_nodes[i].line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);
    }

    for (size_t i = 0; i < CONTROL_NODE_COUNT; i++) {
        control_nodes[i].page = pages[CHAIN_NODE_COUNT + i];
        control_nodes[i].line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);
    }

}

static void init_pointer_chain(void) {
    for (size_t i = 0; i < CHAIN_NODE_COUNT; i++) {
        uint8_t *next = addr_for_node(chain_nodes[(i + 1) % CHAIN_NODE_COUNT]);
        memcpy(addr_for_node(chain_nodes[i]), &next, sizeof(next));
    }
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
    start = read_cntvct_el0();
    maccess(addr);
    end = read_cntvct_el0();
    mfence();
    flush(addr);
    return ticks_to_ns(end - start);
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
    unsigned char used[MAPPING_PAGES];

    memset(used, 0, sizeof(used));

    for (size_t i = 0; i < CHAIN_NODE_COUNT; i++) {
        if (chain_nodes[i].page >= MAPPING_PAGES ||
            chain_nodes[i].line >= LINES_PER_PAGE) {
            fprintf(stderr, "invalid chain node %zu\n", i);
            exit(1);
        }
        if (used[chain_nodes[i].page]) {
            fprintf(stderr, "duplicate chain page at node %zu\n", i);
            exit(1);
        }
        used[chain_nodes[i].page] = 1;
    }

    for (size_t i = 0; i < CONTROL_NODE_COUNT; i++) {
        if (control_nodes[i].page >= MAPPING_PAGES ||
            control_nodes[i].line >= LINES_PER_PAGE) {
            fprintf(stderr, "invalid control node %zu\n", i);
            exit(1);
        }
        if (used[control_nodes[i].page]) {
            fprintf(stderr, "control node %zu shares page with another node\n", i);
            exit(1);
        }
        used[control_nodes[i].page] = 1;
    }

}

static void train_cmc_sequence(int training_replays) {
    for (int replay = 0; replay < training_replays; replay++) {
        flush_nodes(chain_nodes, CHAIN_NODE_COUNT);

        if (use_sw_prefetch) {
            for (size_t i = 0; i < CHAIN_NODE_COUNT; i++) {
                chain_sw_prefetch(addr_for_node(chain_nodes[i]));
            }
        } else {
            void *addr = addr_for_node(chain_nodes[0]);
            for (size_t i = 0; i < CHAIN_NODE_COUNT; i++) {
                addr = chain_step(addr);
            }
        }
        mfence();
    }
}

static void print_controls(const uint64_t *control_latency_ns,
                           const int *control_probes) {
    printf("\n[controls]\n");
    printf("# idx\tpage\tline\trole\t\tprobes\tavg_latency_ns\n");
    for (size_t i = 0; i < CONTROL_NODE_COUNT; i++) {
        uint64_t avg_latency_ns = 0;
        if (control_probes[i] > 0) {
            avg_latency_ns = control_latency_ns[i] / (uint64_t)control_probes[i];
        }

        printf("%2zu\t%3zu\t%2zu\tcontrol\t\t%6d\t%5lu\n",
               i,
               control_nodes[i].page,
               control_nodes[i].line,
               control_probes[i],
               (unsigned long)avg_latency_ns);
    }
}

static void print_header(const char *mode, int rounds, uint64_t hit_threshold_ns,
                         int training_replays) {
    printf("# CMC next-node timing test\n");
    printf("# access mode: %s\n",
           use_sw_prefetch ? "software prefetch (PRFM) training" : "pointer load only");
    printf("# test mode: %s\n", mode);
    printf("# timer: cntvct_el0, cntfrq_el0=%lu Hz\n", (unsigned long)timer_freq_hz);
    printf("# rounds=%d threshold_ns=%lu training_replays=%d\n",
           rounds, (unsigned long)hit_threshold_ns, training_replays);
    printf("# chain_nodes=%d control_nodes=%d mapping_pages=%d\n",
           CHAIN_NODE_COUNT, CONTROL_NODE_COUNT, MAPPING_PAGES);
}

static void run_node_test(int rounds, uint64_t hit_threshold_ns,
                          int training_replays) {
    const size_t transition_count = CHAIN_NODE_COUNT;
    int next_hits[CHAIN_NODE_COUNT];
    int next_probes[CHAIN_NODE_COUNT];
    uint64_t next_latency_ns[CHAIN_NODE_COUNT];
    uint64_t control_latency_ns[CONTROL_NODE_COUNT];
    int control_probes[CONTROL_NODE_COUNT];

    memset(next_hits, 0, sizeof(next_hits));
    memset(next_probes, 0, sizeof(next_probes));
    memset(next_latency_ns, 0, sizeof(next_latency_ns));
    memset(control_latency_ns, 0, sizeof(control_latency_ns));
    memset(control_probes, 0, sizeof(control_probes));

    for (int round = 0; round < rounds; round++) {
        train_cmc_sequence(training_replays);

        flush_nodes(chain_nodes, CHAIN_NODE_COUNT);
        flush_nodes(control_nodes, CONTROL_NODE_COUNT);
        mfence();

        size_t node_idx = (size_t)round % transition_count;
        size_t next_idx = (node_idx + 1) % CHAIN_NODE_COUNT;
        (void)chain_access_step(addr_for_node(chain_nodes[node_idx]));
        mfence();

        delay_before_probe();
        uint64_t t = reload_time_ns(addr_for_node(chain_nodes[next_idx]));
        next_latency_ns[node_idx] += t;
        next_probes[node_idx]++;
        if (t <= hit_threshold_ns) {
            next_hits[node_idx]++;
        }

        size_t control = (size_t)round % CONTROL_NODE_COUNT;
        delay_before_probe();
        t = reload_time_ns(addr_for_node(control_nodes[control]));
        control_latency_ns[control] += t;
        control_probes[control]++;
    }

    print_header("node", rounds, hit_threshold_ns, training_replays);
    printf("# each row accesses node[n] once, then probes node[n+1].\n");

    printf("\n[node_next]\n");
    printf("# node\tnext\tpage\tline\trole\t\tprobes\thits\tper_1000\tavg_latency_ns\n");
    for (size_t node_idx = 0; node_idx < transition_count; node_idx++) {
        size_t next_idx = (node_idx + 1) % CHAIN_NODE_COUNT;
        int probes = next_probes[node_idx];
        uint64_t avg_latency_ns = probes > 0 ?
            next_latency_ns[node_idx] / (uint64_t)probes : 0;
        int per_1000 = probes > 0 ? next_hits[node_idx] * 1000 / probes : 0;

        printf("%3zu\t%3zu\t%3zu\t%2zu\tnext_after_node\t%6d\t%5d\t%4d\t\t%5lu\n",
               node_idx,
               next_idx,
               chain_nodes[next_idx].page,
               chain_nodes[next_idx].line,
               probes,
               next_hits[node_idx],
               per_1000,
               (unsigned long)avg_latency_ns);
    }

    print_controls(control_latency_ns, control_probes);
}

static void run_depth_test(int rounds, uint64_t hit_threshold_ns,
                           int training_replays) {
    const size_t target_count = CHAIN_NODE_COUNT - 1;
    int depth_hits[CHAIN_NODE_COUNT - 1];
    int depth_probes[CHAIN_NODE_COUNT - 1];
    uint64_t depth_latency_ns[CHAIN_NODE_COUNT - 1];
    uint64_t control_latency_ns[CONTROL_NODE_COUNT];
    int control_probes[CONTROL_NODE_COUNT];

    memset(depth_hits, 0, sizeof(depth_hits));
    memset(depth_probes, 0, sizeof(depth_probes));
    memset(depth_latency_ns, 0, sizeof(depth_latency_ns));
    memset(control_latency_ns, 0, sizeof(control_latency_ns));
    memset(control_probes, 0, sizeof(control_probes));

    for (int round = 0; round < rounds; round++) {
        train_cmc_sequence(training_replays);

        flush_nodes(chain_nodes, CHAIN_NODE_COUNT);
        flush_nodes(control_nodes, CONTROL_NODE_COUNT);
        mfence();

        size_t depth = 1 + ((size_t)round % target_count);
        void *addr = addr_for_node(chain_nodes[0]);
        for (size_t step = 0; step < depth; step++) {
            addr = chain_access_step(addr);
        }
        (void)addr;
        mfence();

        delay_before_probe();
        uint64_t t = reload_time_ns(addr_for_node(chain_nodes[depth]));
        depth_latency_ns[depth - 1] += t;
        depth_probes[depth - 1]++;
        if (t <= hit_threshold_ns) {
            depth_hits[depth - 1]++;
        }

        size_t control = (size_t)round % CONTROL_NODE_COUNT;
        delay_before_probe();
        t = reload_time_ns(addr_for_node(control_nodes[control]));
        control_latency_ns[control] += t;
        control_probes[control]++;
    }

    print_header("depth", rounds, hit_threshold_ns, training_replays);
    printf("# each row executes chain accesses node0..node(depth-1), then probes node(depth).\n");

    printf("\n[depth_next]\n");
    printf("# depth\tpage\tline\trole\t\tprobes\thits\tper_1000\tavg_latency_ns\n");
    for (size_t depth = 1; depth <= target_count; depth++) {
        int probes = depth_probes[depth - 1];
        uint64_t avg_latency_ns = probes > 0 ?
            depth_latency_ns[depth - 1] / (uint64_t)probes : 0;
        int per_1000 = probes > 0 ? depth_hits[depth - 1] * 1000 / probes : 0;

        printf("%3zu\t%3zu\t%2zu\tnext_after_depth\t%6d\t%5d\t%4d\t\t%5lu\n",
               depth,
               chain_nodes[depth].page,
               chain_nodes[depth].line,
               probes,
               depth_hits[depth - 1],
               per_1000,
               (unsigned long)avg_latency_ns);
    }

    print_controls(control_latency_ns, control_probes);
}


static void run_direct_test(int rounds, uint64_t hit_threshold_ns,
                            int training_replays) {
    const size_t target_count = CHAIN_NODE_COUNT - 1;
    int depth_hits[CHAIN_NODE_COUNT - 1];
    int depth_probes[CHAIN_NODE_COUNT - 1];
    uint64_t depth_latency_ns[CHAIN_NODE_COUNT - 1];
    uint64_t control_latency_ns[CONTROL_NODE_COUNT];
    int control_probes[CONTROL_NODE_COUNT];

    memset(depth_hits, 0, sizeof(depth_hits));
    memset(depth_probes, 0, sizeof(depth_probes));
    memset(depth_latency_ns, 0, sizeof(depth_latency_ns));
    memset(control_latency_ns, 0, sizeof(control_latency_ns));
    memset(control_probes, 0, sizeof(control_probes));

    for (int round = 0; round < rounds; round++) {
        train_cmc_sequence(training_replays);

        flush_nodes(chain_nodes, CHAIN_NODE_COUNT);
        flush_nodes(control_nodes, CONTROL_NODE_COUNT);
        mfence();

        size_t depth = 1 + ((size_t)round % target_count);
        for (size_t step = 0; step < depth; step++) {
            if (use_sw_prefetch) {
                chain_sw_prefetch(addr_for_node(chain_nodes[step]));
            } else {
                maccess(addr_for_node(chain_nodes[step]));
            }
        }
        mfence();

        delay_before_probe();
        uint64_t t = reload_time_ns(addr_for_node(chain_nodes[depth]));
        depth_latency_ns[depth - 1] += t;
        depth_probes[depth - 1]++;
        if (t <= hit_threshold_ns) {
            depth_hits[depth - 1]++;
        }

        size_t control = (size_t)round % CONTROL_NODE_COUNT;
        delay_before_probe();
        t = reload_time_ns(addr_for_node(control_nodes[control]));
        control_latency_ns[control] += t;
        control_probes[control]++;
    }

    print_header("direct", rounds, hit_threshold_ns, training_replays);
    printf("# each row directly accesses chain_nodes[0..depth-1], then probes chain_nodes[depth].\n");

    printf("\n[depth_next]\n");
    printf("# depth\tpage\tline\trole\t\tprobes\thits\tper_1000\tavg_latency_ns\n");
    for (size_t depth = 1; depth <= target_count; depth++) {
        int probes = depth_probes[depth - 1];
        uint64_t avg_latency_ns = probes > 0 ?
            depth_latency_ns[depth - 1] / (uint64_t)probes : 0;
        int per_1000 = probes > 0 ? depth_hits[depth - 1] * 1000 / probes : 0;

        printf("%3zu\t%3zu\t%2zu\tnext_after_direct\t%6d\t%5d\t%4d\t\t%5lu\n",
               depth,
               chain_nodes[depth].page,
               chain_nodes[depth].line,
               probes,
               depth_hits[depth - 1],
               per_1000,
               (unsigned long)avg_latency_ns);
    }

    print_controls(control_latency_ns, control_probes);
}

static void run_window_test(int rounds, uint64_t hit_threshold_ns,
                            int training_replays, int window_k) {
    if (window_k < 0) {
        const size_t k_count = CHAIN_NODE_COUNT - 1;
        int depth_hits[CHAIN_NODE_COUNT - 1];
        int depth_probes[CHAIN_NODE_COUNT - 1];
        uint64_t depth_latency_ns[CHAIN_NODE_COUNT - 1];
        uint64_t control_latency_ns[CONTROL_NODE_COUNT];
        int control_probes[CONTROL_NODE_COUNT];

        memset(depth_hits, 0, sizeof(depth_hits));
        memset(depth_probes, 0, sizeof(depth_probes));
        memset(depth_latency_ns, 0, sizeof(depth_latency_ns));
        memset(control_latency_ns, 0, sizeof(control_latency_ns));
        memset(control_probes, 0, sizeof(control_probes));

        for (int round = 0; round < rounds; round++) {
            train_cmc_sequence(training_replays);

            flush_nodes(chain_nodes, CHAIN_NODE_COUNT);
            flush_nodes(control_nodes, CONTROL_NODE_COUNT);
            mfence();

            size_t k = (size_t)round % k_count;
            size_t sample = (size_t)round / k_count;
            size_t node_idx = sample % CHAIN_NODE_COUNT;
            size_t start_idx = (node_idx + CHAIN_NODE_COUNT - k) % CHAIN_NODE_COUNT;
            size_t next_idx = (node_idx + 1) % CHAIN_NODE_COUNT;

            void *addr = addr_for_node(chain_nodes[start_idx]);
            for (size_t step = 0; step <= k; step++) {
                addr = chain_access_step(addr);
            }
            (void)addr;
            mfence();

            delay_before_probe();
            uint64_t t = reload_time_ns(addr_for_node(chain_nodes[next_idx]));
            depth_latency_ns[k] += t;
            depth_probes[k]++;
            if (t <= hit_threshold_ns) {
                depth_hits[k]++;
            }

            size_t control = (size_t)round % CONTROL_NODE_COUNT;
            delay_before_probe();
            t = reload_time_ns(addr_for_node(control_nodes[control]));
            control_latency_ns[control] += t;
            control_probes[control]++;
        }

        print_header("window", rounds, hit_threshold_ns, training_replays);
        printf("# sweep mode: each row selects node[n], executes node[n-K]..node[n], then probes node[n+1].\n");
        printf("# K is the number of predecessor transitions before the final node[n] access.\n");

        printf("\n[depth_next]\n");
        printf("# K\tloads\trole\t\tprobes\thits\tper_1000\tavg_latency_ns\n");
        for (size_t k = 0; k < k_count; k++) {
            int probes = depth_probes[k];
            uint64_t avg_latency_ns = probes > 0 ?
                depth_latency_ns[k] / (uint64_t)probes : 0;
            int per_1000 = probes > 0 ? depth_hits[k] * 1000 / probes : 0;

            printf("%3zu\t%5zu\tnext_after_window\t%6d\t%5d\t%4d\t\t%5lu\n",
                   k,
                   k + 1,
                   probes,
                   depth_hits[k],
                   per_1000,
                   (unsigned long)avg_latency_ns);
        }

        print_controls(control_latency_ns, control_probes);
        return;
    }

    const size_t fixed_k = (size_t)window_k;
    int current_hits[CHAIN_NODE_COUNT];
    int current_probes[CHAIN_NODE_COUNT];
    uint64_t current_latency_ns[CHAIN_NODE_COUNT];
    uint64_t control_latency_ns[CONTROL_NODE_COUNT];
    int control_probes[CONTROL_NODE_COUNT];

    memset(current_hits, 0, sizeof(current_hits));
    memset(current_probes, 0, sizeof(current_probes));
    memset(current_latency_ns, 0, sizeof(current_latency_ns));
    memset(control_latency_ns, 0, sizeof(control_latency_ns));
    memset(control_probes, 0, sizeof(control_probes));

    for (int round = 0; round < rounds; round++) {
        train_cmc_sequence(training_replays);

        flush_nodes(chain_nodes, CHAIN_NODE_COUNT);
        flush_nodes(control_nodes, CONTROL_NODE_COUNT);
        mfence();

        size_t node_idx = (size_t)round % CHAIN_NODE_COUNT;
        size_t start_idx = (node_idx + CHAIN_NODE_COUNT - fixed_k) % CHAIN_NODE_COUNT;
        void *addr = addr_for_node(chain_nodes[start_idx]);
        for (size_t step = 0; step < fixed_k; step++) {
            addr = chain_access_step(addr);
        }
        (void)addr;
        mfence();

        delay_before_probe();
        uint64_t t = reload_time_ns(addr_for_node(chain_nodes[node_idx]));
        current_latency_ns[node_idx] += t;
        current_probes[node_idx]++;
        if (t <= hit_threshold_ns) {
            current_hits[node_idx]++;
        }

        size_t control = (size_t)round % CONTROL_NODE_COUNT;
        delay_before_probe();
        t = reload_time_ns(addr_for_node(control_nodes[control]));
        control_latency_ns[control] += t;
        control_probes[control]++;
    }

    print_header("window", rounds, hit_threshold_ns, training_replays);
    printf("# fixed mode: executes the previous K=%zu chain accesses, then probes current node[n].\n", fixed_k);
    printf("# each row reports whether node[n] was prefetched after node[n-K]..node[n-1].\n");

    printf("\n[depth_next]\n");
    printf("# node\tpage\tline\trole\t\tprobes\thits\tper_1000\tavg_latency_ns\n");
    for (size_t node_idx = 0; node_idx < CHAIN_NODE_COUNT; node_idx++) {
        int probes = current_probes[node_idx];
        uint64_t avg_latency_ns = probes > 0 ?
            current_latency_ns[node_idx] / (uint64_t)probes : 0;
        int per_1000 = probes > 0 ? current_hits[node_idx] * 1000 / probes : 0;

        printf("%3zu\t%3zu\t%2zu\tcurrent_after_window\t%6d\t%5d\t%4d\t\t%5lu\n",
               node_idx,
               chain_nodes[node_idx].page,
               chain_nodes[node_idx].line,
               probes,
               current_hits[node_idx],
               per_1000,
               (unsigned long)avg_latency_ns);
    }

    print_controls(control_latency_ns, control_probes);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "usage: %s [rounds threshold_ns training_replays [node|depth|direct|window [window_k]] [load|sw]]\n", prog);
    fprintf(stderr, "default: rounds=%d threshold_ns=%d training_replays=%d mode=node access=load\n",
            DEFAULT_ROUNDS, DEFAULT_THRESHOLD_NS, DEFAULT_TRAINING_REPLAYS);
}

int main(int argc, char **argv) {
    int rounds = DEFAULT_ROUNDS;
    uint64_t hit_threshold_ns = DEFAULT_THRESHOLD_NS;
    int training_replays = DEFAULT_TRAINING_REPLAYS;
    const char *mode = "node";
    int window_k = -1;
    const char *access = "load";

    if (argc != 1 && argc != 4 && argc != 5 && argc != 6 && argc != 7) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 4) {
        rounds = atoi(argv[1]);
        hit_threshold_ns = strtoull(argv[2], NULL, 0);
        training_replays = atoi(argv[3]);
    }
    if (argc >= 5) {
        mode = argv[4];
        if (strcmp(mode, "node") != 0 &&
            strcmp(mode, "depth") != 0 &&
            strcmp(mode, "direct") != 0 &&
            strcmp(mode, "window") != 0) {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (argc >= 6) {
        if (strcmp(argv[5], "load") == 0 || strcmp(argv[5], "sw") == 0) {
            access = argv[5];
        } else {
            window_k = atoi(argv[5]);
            if (argc == 7) {
                access = argv[6];
            }
        }
    }
    if (rounds <= 0 || hit_threshold_ns == 0 || training_replays <= 0 ||
        window_k >= CHAIN_NODE_COUNT) {
        print_usage(argv[0]);
        return 1;
    }
    if (window_k >= 0 && strcmp(mode, "window") != 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 7 && window_k < 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (strcmp(access, "load") == 0) {
        use_sw_prefetch = 0;
    } else if (strcmp(access, "sw") == 0) {
        use_sw_prefetch = 1;
    } else {
        print_usage(argv[0]);
        return 1;
    }

    init_nodes();
    validate_nodes();

    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }

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
    flush_nodes(chain_nodes, CHAIN_NODE_COUNT);
    flush_nodes(control_nodes, CONTROL_NODE_COUNT);
    mfence();

    if (strcmp(mode, "depth") == 0) {
        run_depth_test(rounds, hit_threshold_ns, training_replays);
    } else if (strcmp(mode, "direct") == 0) {
        run_direct_test(rounds, hit_threshold_ns, training_replays);
    } else if (strcmp(mode, "window") == 0) {
        run_window_test(rounds, hit_threshold_ns, training_replays, window_k);
    } else {
        run_node_test(rounds, hit_threshold_ns, training_replays);
    }

    munmap(mapping, mapping_size);
    return 0;
}
