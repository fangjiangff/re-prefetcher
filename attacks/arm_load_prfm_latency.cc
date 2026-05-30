// arm_load_prfm_12_latency.cpp
//
// Build:
//   g++ -O2 -std=c++17 -march=armv8-a arm_load_prfm_12_latency.cpp -o bench
//
// If PMCCNTR_EL0 is enabled for userspace:
//   g++ -O2 -std=c++17 -march=armv8-a -DUSE_PMCCNTR arm_load_prfm_12_latency.cpp -o bench
//
// Run:
//   ./bench --cpu 0 --miss-mb 512 --load-iters 1000000 --prfm-iters 2000000
//
// Notes:
//   1. load hit / load miss use dependent pointer chasing.
//   2. PRFM hit / miss measure PRFM issue cost, not memory-fill completion latency.
//   3. Do NOT use DSB after PRFM to claim the prefetch has completed.

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#ifdef __linux__
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static volatile uintptr_t g_sink = 0;

struct alignas(64) Node {
    Node *next;
    uint8_t pad[64 - sizeof(Node *)];
};

static_assert(sizeof(Node) == 64, "Node must be exactly one cache line");

static inline void full_barrier() {
    asm volatile("dsb sy\n\tisb" ::: "memory");
}

static inline void load_done_barrier() {
    asm volatile("dsb ld\n\tisb" ::: "memory");
}

static inline void instr_barrier_only() {
    // Important:
    // ISB only bounds instruction timing.
    // It does NOT guarantee PRFM completion.
    asm volatile("isb" ::: "memory");
}

static inline uint64_t read_timer() {
    uint64_t v;
#ifdef USE_PMCCNTR
    asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
#else
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
#endif
    return v;
}

#ifndef USE_PMCCNTR
static inline uint64_t read_cntfrq() {
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
#endif

static inline uint64_t timer_start() {
    full_barrier();
    return read_timer();
}

static inline uint64_t timer_stop_after_load() {
    load_done_barrier();
    return read_timer();
}

static inline uint64_t timer_stop_after_prfm() {
    instr_barrier_only();
    return read_timer();
}

static inline uint64_t xorshift64(uint64_t &x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

static size_t floor_pow2(size_t x) {
    if (x == 0) return 0;
    size_t p = 1;
    while (p <= x / 2) p <<= 1;
    return p;
}

static void pin_cpu(int cpu) {
#ifdef __linux__
    if (cpu < 0) return;

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::perror("sched_setaffinity");
    }
#else
    (void)cpu;
#endif
}

static uint8_t *alloc_aligned_bytes(size_t bytes, size_t align = 4096) {
    size_t rounded = ((bytes + align - 1) / align) * align;

    void *p = nullptr;
    if (posix_memalign(&p, align, rounded) != 0 || !p) {
        std::fprintf(stderr, "posix_memalign failed, bytes=%zu\n", bytes);
        std::exit(1);
    }

#ifdef __linux__
    madvise(p, rounded, MADV_HUGEPAGE);
#endif

    return reinterpret_cast<uint8_t *>(p);
}

static void touch_buffer(uint8_t *buf, size_t bytes) {
    for (size_t i = 0; i < bytes; i += 64) {
        buf[i] = static_cast<uint8_t>(i);
    }
    full_barrier();
}

static void thrash_cache(uint8_t *buf, size_t bytes) {
    uintptr_t sum = 0;
    for (size_t i = 0; i < bytes; i += 64) {
        sum += buf[i];
    }
    g_sink ^= sum;
    full_barrier();
}

struct RandomList {
    Node *base;
    Node *start;
    size_t nodes;
};

static RandomList make_random_pointer_cycle(size_t bytes, uint64_t seed) {
    size_t n = bytes / sizeof(Node);
    if (n < 1024) {
        std::fprintf(stderr, "miss buffer too small\n");
        std::exit(1);
    }

    Node *nodes = reinterpret_cast<Node *>(alloc_aligned_bytes(n * sizeof(Node), 4096));

    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);

    std::mt19937_64 rng(seed);
    std::shuffle(idx.begin(), idx.end(), rng);

    for (size_t i = 0; i < n; i++) {
        Node *cur = &nodes[idx[i]];
        Node *nxt = &nodes[idx[(i + 1) % n]];
        cur->next = nxt;
    }

    full_barrier();

    return RandomList{nodes, &nodes[idx[0]], n};
}

static Node *make_hot_self_node() {
    Node *n = reinterpret_cast<Node *>(alloc_aligned_bytes(sizeof(Node), 4096));
    n->next = n;
    full_barrier();

    for (int i = 0; i < 1000; i++) {
        asm volatile("ldr %x0, [%x0]" : "+r"(n) :: "memory");
    }

    return n;
}

static uint64_t bench_dependent_empty(Node *start, size_t iters) {
    Node *p = start;

    uint64_t t0 = timer_start();

    for (size_t i = 0; i < iters; i++) {
        asm volatile("add %x0, %x0, #0" : "+r"(p) :: "memory");
    }

    uint64_t t1 = timer_stop_after_load();

    g_sink ^= reinterpret_cast<uintptr_t>(p);
    return t1 - t0;
}

static uint64_t bench_dependent_load_chain(Node *start, size_t iters) {
    Node *p = start;

    uint64_t t0 = timer_start();

    for (size_t i = 0; i < iters; i++) {
        asm volatile("ldr %x0, [%x0]" : "+r"(p) :: "memory");
    }

    uint64_t t1 = timer_stop_after_load();

    g_sink ^= reinterpret_cast<uintptr_t>(p);
    return t1 - t0;
}

static uint64_t bench_prfm_empty(uint8_t *base, size_t lines_pow2, size_t iters) {
    uint64_t state = 0x123456789abcdefULL;
    const size_t mask = lines_pow2 - 1;

    uint64_t t0 = timer_start();

    for (size_t i = 0; i < iters; i++) {
        size_t idx = xorshift64(state) & mask;
        uint8_t *p = base + idx * 64;

        asm volatile("" :: "r"(p) : "memory");
    }

    uint64_t t1 = timer_stop_after_prfm();

    g_sink ^= state;
    return t1 - t0;
}

#define DEFINE_PRFM_BENCH(FUNC_NAME, PRFM_HINT)                         \
static uint64_t FUNC_NAME(uint8_t *base, size_t lines_pow2, size_t iters) { \
    uint64_t state = 0x123456789abcdefULL;                              \
    const size_t mask = lines_pow2 - 1;                                  \
                                                                        \
    uint64_t t0 = timer_start();                                         \
                                                                        \
    for (size_t i = 0; i < iters; i++) {                                 \
        size_t idx = xorshift64(state) & mask;                           \
        uint8_t *p = base + idx * 64;                                    \
        asm volatile("prfm " PRFM_HINT ", [%0]" :: "r"(p) : "memory"); \
    }                                                                   \
                                                                        \
    uint64_t t1 = timer_stop_after_prfm();                               \
                                                                        \
    g_sink ^= state;                                                     \
    return t1 - t0;                                                      \
}

DEFINE_PRFM_BENCH(bench_pldl1keep, "pldl1keep")
DEFINE_PRFM_BENCH(bench_pldl1strm, "pldl1strm")
DEFINE_PRFM_BENCH(bench_pldl2keep, "pldl2keep")
DEFINE_PRFM_BENCH(bench_pldl2strm, "pldl2strm")
DEFINE_PRFM_BENCH(bench_pldl3keep, "pldl3keep")
DEFINE_PRFM_BENCH(bench_pldl3strm, "pldl3strm")

DEFINE_PRFM_BENCH(bench_pstl1keep, "pstl1keep")
DEFINE_PRFM_BENCH(bench_pstl1strm, "pstl1strm")
DEFINE_PRFM_BENCH(bench_pstl2keep, "pstl2keep")
DEFINE_PRFM_BENCH(bench_pstl2strm, "pstl2strm")
DEFINE_PRFM_BENCH(bench_pstl3keep, "pstl3keep")
DEFINE_PRFM_BENCH(bench_pstl3strm, "pstl3strm")

using PrfmBenchFn = uint64_t (*)(uint8_t *, size_t, size_t);

struct PrfmCase {
    const char *name;
    PrfmBenchFn fn;
};

static const PrfmCase prfm_cases[] = {
    {"PLDL1KEEP", bench_pldl1keep},
    {"PLDL1STRM", bench_pldl1strm},
    {"PLDL2KEEP", bench_pldl2keep},
    {"PLDL2STRM", bench_pldl2strm},
    {"PLDL3KEEP", bench_pldl3keep},
    {"PLDL3STRM", bench_pldl3strm},

    {"PSTL1KEEP", bench_pstl1keep},
    {"PSTL1STRM", bench_pstl1strm},
    {"PSTL2KEEP", bench_pstl2keep},
    {"PSTL2STRM", bench_pstl2strm},
    {"PSTL3KEEP", bench_pstl3keep},
    {"PSTL3STRM", bench_pstl3strm},
};

static double adjusted_per_op(uint64_t total, uint64_t overhead, size_t iters) {
    if (total <= overhead) return 0.0;
    return static_cast<double>(total - overhead) / static_cast<double>(iters);
}

static void print_ticks_or_ns(double ticks_per_op) {
#ifdef USE_PMCCNTR
    std::printf("%10.3f cycles/op", ticks_per_op);
#else
    static const double freq = static_cast<double>(read_cntfrq());
    double ns = ticks_per_op * 1e9 / freq;
    std::printf("%10.3f ticks/op, %10.3f ns/op", ticks_per_op, ns);
#endif
}

static void print_load_metric(const char *name, double ticks_per_op) {
    std::printf("%-32s : ", name);
    print_ticks_or_ns(ticks_per_op);
    std::printf("\n");
}

int main(int argc, char **argv) {
    int cpu = -1;
    size_t miss_mb = 256;
    size_t load_iters = 1000000;
    size_t prfm_iters = 2000000;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            cpu = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--miss-mb") == 0 && i + 1 < argc) {
            miss_mb = std::strtoull(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--load-iters") == 0 && i + 1 < argc) {
            load_iters = std::strtoull(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--prfm-iters") == 0 && i + 1 < argc) {
            prfm_iters = std::strtoull(argv[++i], nullptr, 0);
        } else {
            std::fprintf(stderr,
                "Usage: %s [--cpu N] [--miss-mb MB] [--load-iters N] [--prfm-iters N]\n",
                argv[0]);
            return 1;
        }
    }

    pin_cpu(cpu);

#ifdef USE_PMCCNTR
    std::printf("Timer         : PMCCNTR_EL0, unit = CPU cycles\n");
#else
    std::printf("Timer         : CNTVCT_EL0, frequency = %" PRIu64 " Hz\n", read_cntfrq());
    std::printf("Note          : CNTVCT may be too coarse for tiny PRFM issue costs.\n");
#endif

    std::printf("CPU pin       : %d\n", cpu);
    std::printf("miss buffer   : %zu MB\n", miss_mb);
    std::printf("load iters    : %zu\n", load_iters);
    std::printf("prfm iters    : %zu\n\n", prfm_iters);

    const size_t miss_bytes = miss_mb * 1024ULL * 1024ULL;

    uint8_t *evict_buf = alloc_aligned_bytes(miss_bytes, 4096);
    touch_buffer(evict_buf, miss_bytes);

    // ============================================================
    // 1. load hit
    // ============================================================
    Node *hot = make_hot_self_node();

    uint64_t load_empty_hot = bench_dependent_empty(hot, load_iters);
    uint64_t load_hit_total = bench_dependent_load_chain(hot, load_iters);

    double load_hit = adjusted_per_op(load_hit_total, load_empty_hot, load_iters);

    // ============================================================
    // 2. load miss
    // ============================================================
    RandomList miss_list = make_random_pointer_cycle(miss_bytes, 0xdeadbeef12345678ULL);

    thrash_cache(evict_buf, miss_bytes);
    uint64_t load_empty_miss = bench_dependent_empty(miss_list.start, load_iters);

    thrash_cache(evict_buf, miss_bytes);
    uint64_t load_miss_total = bench_dependent_load_chain(miss_list.start, load_iters);

    double load_miss = adjusted_per_op(load_miss_total, load_empty_miss, load_iters);

    std::printf("Load results after overhead subtraction:\n");
    print_load_metric("load hit", load_hit);
    print_load_metric("load miss", load_miss);

    // ============================================================
    // 3. PRFM hit set
    // ============================================================
    const size_t hot_lines = 64; // 4 KB hot set
    uint8_t *pf_hot = alloc_aligned_bytes(hot_lines * 64, 4096);
    touch_buffer(pf_hot, hot_lines * 64);

    uintptr_t warm_sum = 0;
    for (size_t i = 0; i < hot_lines * 64; i += 64) {
        warm_sum += pf_hot[i];
    }
    g_sink ^= warm_sum;
    full_barrier();

    uint64_t prfm_empty_hit = bench_prfm_empty(pf_hot, hot_lines, prfm_iters);

    // ============================================================
    // 4. PRFM miss set
    // ============================================================
    uint8_t *pf_cold = alloc_aligned_bytes(miss_bytes, 4096);
    touch_buffer(pf_cold, miss_bytes);

    size_t cold_lines = floor_pow2(miss_bytes / 64);
    if (cold_lines < 1024) {
        std::fprintf(stderr, "cold_lines too small\n");
        return 1;
    }

    thrash_cache(evict_buf, miss_bytes);
    uint64_t prfm_empty_miss = bench_prfm_empty(pf_cold, cold_lines, prfm_iters);

    // ============================================================
    // 5. Run all 12 PRFM hints
    // ============================================================
    std::printf("\nPRFM results after address-generation overhead subtraction:\n");
    std::printf("%-12s | %-28s | %-28s\n", "hint", "hit target", "miss target");
    std::printf("-------------+------------------------------+------------------------------\n");

    for (const auto &c : prfm_cases) {
        // Re-warm the small hit set before every case.
        warm_sum = 0;
        for (size_t i = 0; i < hot_lines * 64; i += 64) {
            warm_sum += pf_hot[i];
        }
        g_sink ^= warm_sum;
        full_barrier();

        uint64_t hit_total = c.fn(pf_hot, hot_lines, prfm_iters);
        double hit_cost = adjusted_per_op(hit_total, prfm_empty_hit, prfm_iters);

        // Re-thrash before every miss case.
        // This improves coldness, but PRFM may still be dropped, merged,
        // throttled, or converted into different microarchitectural requests.
        thrash_cache(evict_buf, miss_bytes);

        uint64_t miss_total = c.fn(pf_cold, cold_lines, prfm_iters);
        double miss_cost = adjusted_per_op(miss_total, prfm_empty_miss, prfm_iters);

        std::printf("%-12s | ", c.name);
        print_ticks_or_ns(hit_cost);
        std::printf(" | ");
        print_ticks_or_ns(miss_cost);
        std::printf("\n");
    }

    std::printf("\nRaw overheads:\n");
    std::printf("load hit overhead     : %" PRIu64 "\n", load_empty_hot);
    std::printf("load miss overhead    : %" PRIu64 "\n", load_empty_miss);
    std::printf("prfm hit overhead     : %" PRIu64 "\n", prfm_empty_hit);
    std::printf("prfm miss overhead    : %" PRIu64 "\n", prfm_empty_miss);

    std::printf("\ng_sink = 0x%lx\n", static_cast<unsigned long>(g_sink));

    return 0;
}