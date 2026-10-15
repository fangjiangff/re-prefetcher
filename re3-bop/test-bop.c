#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __aarch64__
#error "This test requires AArch64 DC CIVAC, CPP RCTX, and CNTVCT_EL0."
#endif

#include "../until.h"

#ifndef ENABLE_PMU
#define ENABLE_PMU 1
#endif
#ifndef ENABLE_CACHE_PROBE
#define ENABLE_CACHE_PROBE 1
#endif
#ifndef PMU_CORE_X925
#define PMU_CORE_X925 1
#endif
#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 2000
#endif
#ifndef TRAIN_STORE
#define TRAIN_STORE 0
#endif

#if TRAIN_STORE
#define TRAIN_ACCESS_ASM(reg) "mov w17, #1\n\tstrb w17, [%0]"
#define TRAIN_ACCESS_CLOBBERS(reg) #reg, "x17", "memory"
#define TRIGGER_ACCESS_CLOBBERS "x17", "memory"
#else
#define TRAIN_ACCESS_ASM(reg) "ldr " #reg ", [%0]"
#define TRAIN_ACCESS_CLOBBERS(reg) #reg, "memory"
#define TRIGGER_ACCESS_CLOBBERS "x17", "memory"
#endif

#define TRAIN_NOISE_ACCESSES 0

#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"

#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)
#define DEFAULT_BOP_OFFSET_LINES 16
#define TRAIN_PC_COUNT 16
#define POOL_PAGES 8192
#define DEFAULT_ROUNDS 100
#define DEFAULT_THRESHOLD_NS 80
#define DEFAULT_TRAIN_PAIRS 1024
#define DEFAULT_TRIGGER_BATCH 1
#define PAGE_SEED 0x243f6a8885a308d3ULL

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

#define POST_TRAIN_PAGES 256

static uint8_t *post_train_mapping;
static size_t post_train_mapping_size;

enum train_mode {
    TRAIN_NONE,
    TRAIN_FIXED_OFFSET,
    TRAIN_RANDOM_DELTA,
};

enum pre_trigger_action {
    PRE_TRIGGER_NONE,
    PRE_TRIGGER_SYSCALL,
    PRE_TRIGGER_YIELD,
    PRE_TRIGGER_NANOSLEEP,
    PRE_TRIGGER_CPP_RCTX,
    PRE_TRIGGER_PROCESS,
};

struct case_stats {
    uint64_t rounds;
    uint64_t probes;
    uint64_t hits;
    uint64_t latency_ns;
    uint64_t l1_events;
    uint64_t l2_events;
};

typedef void (*access_function)(void *);

static uint8_t *mapping;
static size_t mapping_size;
static size_t page_order[POOL_PAGES];
static size_t slot_order[POOL_PAGES];
static uint64_t timer_freq_hz;
static pid_t process_action_pid = -1;
static int process_action_request_fd = -1;
static int process_action_reply_fd = -1;

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

#define DEFINE_ACCESS(number, reg)                                      \
    __attribute__((noinline)) static void train_pc##number(void *addr)   \
    {                                                                    \
        asm volatile(TRAIN_ACCESS_ASM(reg)                             \
                     :: "r"(addr) : TRAIN_ACCESS_CLOBBERS(reg));       \
    }

DEFINE_ACCESS(0, x1)
DEFINE_ACCESS(1, x2)
DEFINE_ACCESS(2, x3)
DEFINE_ACCESS(3, x4)
DEFINE_ACCESS(4, x5)
DEFINE_ACCESS(5, x6)
DEFINE_ACCESS(6, x7)
DEFINE_ACCESS(7, x8)
DEFINE_ACCESS(8, x9)
DEFINE_ACCESS(9, x10)
DEFINE_ACCESS(10, x11)
DEFINE_ACCESS(11, x12)
DEFINE_ACCESS(12, x13)
DEFINE_ACCESS(13, x14)
DEFINE_ACCESS(14, x15)
DEFINE_ACCESS(15, x16)

static access_function const train_pcs[TRAIN_PC_COUNT] = {
    train_pc0, train_pc1, train_pc2, train_pc3,
    train_pc4, train_pc5, train_pc6, train_pc7,
    train_pc8, train_pc9, train_pc10, train_pc11,
    train_pc12, train_pc13, train_pc14, train_pc15,
};

/* No training access uses this PC. */
__attribute__((noinline)) static void bop_trigger_access(void *addr)
{
    asm volatile(TRAIN_ACCESS_ASM(x17)
                 :: "r"(addr) : TRIGGER_ACCESS_CLOBBERS);
}

static inline __attribute__((always_inline)) void wait_for_prefetch(void)
{
    asm volatile(".rept " STRINGIFY(PREFETCH_WAIT_NOPS) "\n\t"
                 "nop\n\t"
                 ".endr\n\t"
                 ::: "memory");
}

static uint8_t *page_addr(size_t page, size_t line)
{
    return mapping + page * PAGE_SIZE + line * LINE_SIZE;
}

static void flush_pool_mapping(void)
{
    for (size_t offset = 0; offset < mapping_size; offset += LINE_SIZE)
        flush(mapping + offset);
    mfence();
}

static size_t offset_span_pages(size_t offset_lines)
{
    return offset_lines / LINES_PER_PAGE + 1u;
}

static size_t offset_base_line_count(size_t offset_lines)
{
    size_t remainder = offset_lines % LINES_PER_PAGE;

    return remainder == 0 ? LINES_PER_PAGE : LINES_PER_PAGE - remainder;
}

static size_t round_slot_page(size_t round, size_t position,
                              size_t span_pages)
{
    size_t slot_count = POOL_PAGES / span_pages;
    size_t slot = (round * 977u + position) % slot_count;

    return slot_order[slot] * span_pages;
}

static void init_slot_order(size_t slot_count, size_t offset_lines)
{
    uint64_t rng = PAGE_SEED ^ 0x9e3779b97f4a7c15ULL ^ offset_lines;

    for (size_t i = 0; i < slot_count; i++)
        slot_order[i] = i;
    for (size_t i = slot_count - 1; i > 0; i--) {
        size_t j = (size_t)(splitmix64_next(&rng) % (i + 1));
        size_t temporary = slot_order[i];

        slot_order[i] = slot_order[j];
        slot_order[j] = temporary;
    }
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

static void prefault_mapping(void)
{
    uint64_t rng = PAGE_SEED ^ 0xa4093822299f31d0ULL;

    /* Random page order avoids leaving a sequential page stream behind. */
    for (size_t i = 0; i < POOL_PAGES; i++) {
        size_t page = page_order[i];
        size_t line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);

        *page_addr(page, line) = (uint8_t)i;
    }
    mfence();
}

static uint8_t *post_train_addr(size_t page, size_t line)
{
    return post_train_mapping
         + page * PAGE_SIZE
         + line * LINE_SIZE;
}

static void post_train_accesses(size_t round, size_t count)
{
    uint64_t rng =
        PAGE_SEED ^
        0x8f3f73b5cf1c9adeULL ^
        ((uint64_t)round * 0x9e3779b97f4a7c15ULL);

    for (size_t i = 0; i < count; i++) {
        size_t page = (size_t)(splitmix64_next(&rng) % POST_TRAIN_PAGES);

        size_t line = (size_t)(splitmix64_next(&rng) % LINES_PER_PAGE);

        uint8_t *addr = post_train_addr(page, line);

        train_pcs[(i * 5u + 1u) % TRAIN_PC_COUNT](addr);
    }

    // mfence();
}

static void train_bop(enum train_mode mode, size_t round, size_t train_pairs,
                      size_t offset_lines, size_t span_pages)
{
    uint64_t rng = PAGE_SEED ^ ((uint64_t)round * 0xd1b54a32d192ed03ULL)
                   ^ ((uint64_t)offset_lines << 32);
    uint64_t mate_rng = rng ^ 0x6a09e667f3bcc909ULL;
    size_t base_line_count = offset_base_line_count(offset_lines);

    if (mode == TRAIN_NONE)
        return;

    for (size_t i = 0; i < train_pairs; i++) {
        size_t page = round_slot_page(round, i, span_pages);
        size_t base_line = (size_t)(splitmix64_next(&rng) % base_line_count);
        size_t mate_line;
        uint8_t *base = page_addr(page, base_line);
        uint8_t *mate;

        if (mode == TRAIN_RANDOM_DELTA) {
            size_t fixed_mate_line = base_line + offset_lines;
            size_t random_base_line;

            if (base_line_count == 1) {
                /* Keep the same target-page class at the degenerate edge. */
                mate_line = offset_lines == 0 ? 1 : offset_lines - 1;
            } else {
                do {
                    random_base_line =(size_t)(splitmix64_next(&mate_rng) % base_line_count);
                    mate_line = random_base_line + offset_lines;
                } while (mate_line == base_line ||
                         mate_line == fixed_mate_line);
            }
        } else {
            mate_line = base_line + offset_lines;
        }
        mate = page_addr(page, mate_line);

        train_pcs[(i * 5u) % TRAIN_PC_COUNT](base);
        nops();
        /* Insert unrelated memory accesses between Xi and Xi+d. */
        for (size_t j = 0; j < TRAIN_NOISE_ACCESSES; j++) {
            size_t noise_page = page_order[(i * 131u + j * 977u + 123u) % POOL_PAGES];

            size_t noise_line = (size_t)(splitmix64_next(&mate_rng) % LINES_PER_PAGE);

            uint8_t *noise = page_addr(noise_page, noise_line);
            /*
            * Use a rotating PC so that the noise accesses themselves do not
            * form an obvious per-PC stride stream.
            */
            train_pcs[(i * 11u + j * 3u + 7u) % TRAIN_PC_COUNT](noise);
        }


        train_pcs[(i * 7u + 3u) % TRAIN_PC_COUNT](mate);
        nops();
    }
}

static void prepare_trigger_batch(size_t round, size_t trigger_batch,
                                  size_t probe_delta, size_t layout_offset,
                                  size_t span_pages,
                                  void **triggers, void **targets)
{
    uint64_t rng = PAGE_SEED ^ 0x13198a2e03707344ULL
                   ^ ((uint64_t)round * 0x94d049bb133111ebULL);
    size_t base_line_count = offset_base_line_count(layout_offset);
    size_t slot_count = POOL_PAGES / span_pages;
    size_t first_slot = slot_count - trigger_batch;

    for (size_t i = 0; i < trigger_batch; i++) {
        size_t page = round_slot_page(round, first_slot + i, span_pages);
        size_t line = (size_t)(splitmix64_next(&rng) % base_line_count);

        triggers[i] = page_addr(page, line);
        targets[i] = page_addr(page, line + probe_delta);
    }
}

static ssize_t read_full(int fd, void *buffer, size_t length)
{
    uint8_t *cursor = buffer;
    size_t remaining = length;

    while (remaining != 0) {
        ssize_t count = read(fd, cursor, remaining);

        if (count == 0)
            return (ssize_t)(length - remaining);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += count;
        remaining -= (size_t)count;
    }
    return (ssize_t)length;
}

static int write_full(int fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = buffer;
    size_t remaining = length;

    while (remaining != 0) {
        ssize_t count = write(fd, cursor, remaining);

        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += count;
        remaining -= (size_t)count;
    }
    return 0;
}

static void process_action_child_loop(int request_fd, int reply_fd)
{
    char token = 'R';

    if (write_full(reply_fd, &token, sizeof(token)) != 0)
        _exit(1);

    for (;;) {
        if (read_full(request_fd, &token, sizeof(token)) != sizeof(token))
            _exit(0);
        if (token == 'Q')
            _exit(0);

        (void)syscall(SYS_getpid);
        nops();

        if (write_full(reply_fd, &token, sizeof(token)) != 0)
            _exit(1);
    }
}

static int setup_process_action(enum pre_trigger_action action)
{
    int request_pipe[2] = {-1, -1};
    int reply_pipe[2] = {-1, -1};
    char ready;

    if (action != PRE_TRIGGER_PROCESS)
        return 0;
    if (pipe(request_pipe) != 0 || pipe(reply_pipe) != 0) {
        fprintf(stderr, "pipe for process action failed: %s\n",
                strerror(errno));
        goto fail;
    }

    process_action_pid = fork();
    if (process_action_pid < 0) {
        fprintf(stderr, "fork for process action failed: %s\n",
                strerror(errno));
        goto fail;
    }
    if (process_action_pid == 0) {
        close(request_pipe[1]);
        close(reply_pipe[0]);
        process_action_child_loop(request_pipe[0], reply_pipe[1]);
    }

    close(request_pipe[0]);
    close(reply_pipe[1]);
    process_action_request_fd = request_pipe[1];
    process_action_reply_fd = reply_pipe[0];
    request_pipe[1] = -1;
    reply_pipe[0] = -1;

    if (read_full(process_action_reply_fd, &ready, sizeof(ready)) !=
        sizeof(ready) || ready != 'R') {
        fprintf(stderr, "process action child did not become ready\n");
        goto fail;
    }
    return 0;

fail:
    if (process_action_request_fd >= 0) {
        close(process_action_request_fd);
        process_action_request_fd = -1;
    }
    if (process_action_reply_fd >= 0) {
        close(process_action_reply_fd);
        process_action_reply_fd = -1;
    }
    if (request_pipe[0] >= 0)
        close(request_pipe[0]);
    if (request_pipe[1] >= 0)
        close(request_pipe[1]);
    if (reply_pipe[0] >= 0)
        close(reply_pipe[0]);
    if (reply_pipe[1] >= 0)
        close(reply_pipe[1]);
    if (process_action_pid > 0) {
        (void)waitpid(process_action_pid, NULL, 0);
        process_action_pid = -1;
    }
    return -1;
}

static void cleanup_process_action(void)
{
    char token = 'Q';

    if (process_action_request_fd >= 0) {
        (void)write_full(process_action_request_fd, &token, sizeof(token));
        close(process_action_request_fd);
        process_action_request_fd = -1;
    }
    if (process_action_reply_fd >= 0) {
        close(process_action_reply_fd);
        process_action_reply_fd = -1;
    }
    if (process_action_pid > 0) {
        (void)waitpid(process_action_pid, NULL, 0);
        process_action_pid = -1;
    }
}

static void run_process_action(void)
{
    char token = 'P';

    if (process_action_request_fd < 0 || process_action_reply_fd < 0) {
        fprintf(stderr, "process action was not initialized\n");
        exit(1);
    }
    if (write_full(process_action_request_fd, &token, sizeof(token)) != 0 ||
        read_full(process_action_reply_fd, &token, sizeof(token)) !=
        sizeof(token)) {
        fprintf(stderr, "process action handshake failed: %s\n",
                strerror(errno));
        exit(1);
    }
}

static const char *pre_trigger_action_name(enum pre_trigger_action action)
{
    switch (action) {
    case PRE_TRIGGER_NONE:
        return "none";
    case PRE_TRIGGER_SYSCALL:
        return "syscall";
    case PRE_TRIGGER_YIELD:
        return "yield";
    case PRE_TRIGGER_NANOSLEEP:
        return "nanosleep";
    case PRE_TRIGGER_CPP_RCTX:
        return "cpp_rctx";
    case PRE_TRIGGER_PROCESS:
        return "process";
    }
    return "unknown";
}

static void run_pre_trigger_action(enum pre_trigger_action action)
{
    volatile long syscall_result;
    struct timespec request = {
        .tv_sec = 0,
        .tv_nsec = 1000,
    };

    switch (action) {
    case PRE_TRIGGER_NONE:
        break;
    case PRE_TRIGGER_SYSCALL:
        syscall_result = syscall(SYS_getpid);
        (void)syscall_result;
        break;
    case PRE_TRIGGER_YIELD:
        sched_yield();
        break;
    case PRE_TRIGGER_NANOSLEEP:
        nanosleep(&request, NULL);
        break;
    case PRE_TRIGGER_CPP_RCTX:
        cpp_rctx();
        break;
    case PRE_TRIGGER_PROCESS:
        run_process_action();
        break;
    }
    mfence();
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
    if (ioctl(pmu_fd, PREFETCH_PMU_IOC_REARM) != 0) {
        fprintf(stderr, "PMU rearm ioctl failed: %s\n", strerror(errno));
        close(pmu_fd);
        pmu_fd = -1;
        return -1;
    }
    return 0;
}
#endif

#if ENABLE_CACHE_PROBE
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
#endif

static void run_case(const char *name, enum train_mode mode,
                     size_t train_offset, size_t probe_delta,
                     size_t layout_offset, size_t span_pages, size_t rounds,
                     uint64_t threshold_ns, size_t train_pairs,
                     size_t trigger_batch,
                     enum pre_trigger_action pre_trigger_action)
{
    struct case_stats stats = {0};
    size_t batch_slots;
    void **triggers;
    void **targets;

    if (rounds > SIZE_MAX / trigger_batch) {
        fprintf(stderr, "trigger batch size overflow\n");
        exit(1);
    }
    batch_slots = rounds * trigger_batch;
    triggers = calloc(batch_slots, sizeof(*triggers));
    targets = calloc(batch_slots, sizeof(*targets));

#if !ENABLE_CACHE_PROBE
    (void)threshold_ns;
#endif

    if (triggers == NULL || targets == NULL) {
        fprintf(stderr, "calloc trigger arrays failed\n");
        exit(1);
    }

    for (size_t round = 0; round < rounds; round++)
        prepare_trigger_batch(0, trigger_batch, probe_delta,
                              layout_offset, span_pages,
                              triggers + round * trigger_batch,
                              targets + round * trigger_batch);

    for (size_t round = 0; round < rounds; round++) {
        void **round_triggers = triggers + round * trigger_batch;
#if ENABLE_CACHE_PROBE
        void **round_targets = targets + round * trigger_batch;
#endif
#if ENABLE_PMU
        struct prefetch_pmu_snapshot before = {0};
        struct prefetch_pmu_snapshot after = {0};
#endif

        flush_pool_mapping();
        cpp_rctx();
        train_bop(mode, 0, train_pairs, train_offset, span_pages);
#if ENABLE_PMU
        if (pmu_ready)
            before = prefetch_pmu_read();
#endif

        for (size_t i = 0; i < trigger_batch; i++)
            bop_trigger_access(round_triggers[i]);
        wait_for_prefetch();
        
#if ENABLE_PMU
        if (pmu_ready) {
            after = prefetch_pmu_read();
            stats.l1_events += prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
            stats.l2_events += prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
        }
#endif

#if ENABLE_CACHE_PROBE
        /* A coprime permutation separates trigger order from probe order. */
        for (size_t k = 0; k < trigger_batch; k++) {
            size_t i = (k * 37u + 11u) % trigger_batch;
            uint64_t latency = timed_reload_ns(round_targets[i]);

            stats.latency_ns += latency;
            stats.probes++;
            if (latency <= threshold_ns)
                stats.hits++;
        }
#endif
        stats.rounds++;
    }

    printf("\n[%s]\n", name);
    if (mode == TRAIN_RANDOM_DELTA)
        printf("# train_mode=%d train_offset_lines=- "
               "probe_delta_lines=%zu rounds=%llu\n",
               mode, probe_delta, (unsigned long long)stats.rounds);
    else
        printf("# train_mode=%d train_offset_lines=%zu "
               "probe_delta_lines=%zu rounds=%llu\n",
               mode, train_offset, probe_delta,
               (unsigned long long)stats.rounds);
    printf("# pre_trigger_action=%s\n",
           pre_trigger_action_name(pre_trigger_action));
#if ENABLE_PMU
    if (pmu_ready) {
        double trigger_denominator =
            (double)stats.rounds * (double)trigger_batch;
        printf("# PMU %-24s total=%llu per_trigger=%.6f\n",
               "l1d_cache_hwprf",
               (unsigned long long)stats.l1_events,
               (double)stats.l1_events / trigger_denominator);
        printf("# PMU %-24s total=%llu per_trigger=%.6f\n",
               "l2d_cache_hwprf",
               (unsigned long long)stats.l2_events,
               (double)stats.l2_events / trigger_denominator);
    }
#endif
#if ENABLE_CACHE_PROBE
    printf("# CACHE probes=%llu hits=%llu hits_per_1000=%llu "
           "avg_latency_ns=%.2f threshold_ns=%llu\n",
           (unsigned long long)stats.probes,
           (unsigned long long)stats.hits,
           (unsigned long long)(stats.probes == 0 ? 0 :
               stats.hits * 1000u / stats.probes),
           stats.probes == 0 ? 0.0 :
               (double)stats.latency_ns / (double)stats.probes,
           (unsigned long long)threshold_ns);
#endif

    free(targets);
    free(triggers);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [rounds threshold_ns train_pairs trigger_batch "
            "[offset_lines [pre_trigger_action]]]\n",
            program);
    fprintf(stderr,
            "pre_trigger_action: 0=none, 1=syscall, 2=yield, "
            "3=nanosleep, 4=cpp_rctx, 5=process\n");
}

int main(int argc, char **argv)
{
    size_t rounds = DEFAULT_ROUNDS;
    uint64_t threshold_ns = DEFAULT_THRESHOLD_NS;
    size_t train_pairs = DEFAULT_TRAIN_PAIRS;
    size_t trigger_batch = DEFAULT_TRIGGER_BATCH;
    size_t offset_lines = DEFAULT_BOP_OFFSET_LINES;
    size_t wrong_probe_offset;
    size_t layout_offset;
    enum pre_trigger_action pre_trigger_action = PRE_TRIGGER_NONE;
    size_t span_pages;
    size_t slots_needed;
    size_t pages_needed;
    int result = 0;

    if (argc != 1 && argc != 5 && argc != 6 && argc != 7) {
        usage(argv[0]);
        return 1;
    }
    if (argc >= 5) {
        rounds = strtoull(argv[1], NULL, 0);
        threshold_ns = strtoull(argv[2], NULL, 0);
        train_pairs = strtoull(argv[3], NULL, 0);
        trigger_batch = strtoull(argv[4], NULL, 0);
    }
    if (argc == 6) {
        offset_lines = strtoull(argv[5], NULL, 0);
    }
    if (argc == 7) {
        offset_lines = strtoull(argv[5], NULL, 0);
        pre_trigger_action =
            (enum pre_trigger_action)strtoull(argv[6], NULL, 0);
    }
    wrong_probe_offset = offset_lines == 0 ? 1 :
                         offset_lines == 1 ? 2 : offset_lines - 1;
    layout_offset = offset_lines > wrong_probe_offset ?
                    offset_lines : wrong_probe_offset;
    span_pages = offset_span_pages(layout_offset);
    slots_needed = train_pairs + trigger_batch;
    pages_needed = slots_needed * span_pages;
    if (rounds == 0 || threshold_ns == 0 ||
        trigger_batch == 0 || pages_needed > POOL_PAGES ||
        pre_trigger_action > PRE_TRIGGER_PROCESS) {
        usage(argv[0]);
        fprintf(stderr,
                "pages_needed=%zu, limit=%d; trigger_batch must be positive; "
                "offset_lines must be non-negative and fit in the mapped pool; "
                "pre_trigger_action must be 0..5\n",
                pages_needed, POOL_PAGES);
        return 1;
    }

    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    init_page_order();
    init_slot_order(POOL_PAGES / span_pages, offset_lines);
    if (setup_process_action(pre_trigger_action) != 0)
        return 1;

    mapping_size = (size_t)POOL_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        cleanup_process_action();
        return 1;
    }
    prefault_mapping();

    post_train_mapping_size = (size_t)POST_TRAIN_PAGES * PAGE_SIZE;

    post_train_mapping = mmap(NULL,
                            post_train_mapping_size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                            -1, 0);

    if (post_train_mapping == MAP_FAILED) {
        fprintf(stderr, "post-train mmap failed: %s\n", strerror(errno));
        return 1;
    }
    for (size_t i = 0; i < POST_TRAIN_PAGES; i++) {
        post_train_mapping[i * PAGE_SIZE] = (uint8_t)i;
    }

    mfence();


    cpp_rctx();

    printf("# BOP existence and offset-relocation test\n");
    printf("# offset_lines=%zu offset_bytes=%zu\n",
           offset_lines, offset_lines * LINE_SIZE);
    printf("# rounds=%zu train_pairs=%zu trigger_batch=%zu pages=%d "
           "threshold_ns=%llu wait_nops=%d train_pcs=%d TRAIN_NOISE_ACCESSES=%d\n",
           rounds, train_pairs, trigger_batch, POOL_PAGES,
           (unsigned long long)threshold_ns, PREFETCH_WAIT_NOPS,
           TRAIN_PC_COUNT, TRAIN_NOISE_ACCESSES);
    printf("# pre_trigger_action=%s\n",
           pre_trigger_action_name(pre_trigger_action));
    printf("# strict_training=0 training_flush=mapping_each_round "
           "training_noise_loads=0 trigger_slot_policy=fixed_tail\n");
    printf("# compatibility_diagnostic=round_flush_before_training\n");
    printf("# trigger_address_policy=precomputed_before_rounds\n");
    printf("# sequence_round_policy=fixed_round_0\n");
    printf("# training_access_type=%s\n", TRAIN_STORE ? "store" : "load");
    printf("# random_control_match=%s\n",
           offset_base_line_count(offset_lines) == 1 ?
           "partial_singleton_line_range" :
           "matched_mate_page_class_and_line_range");
    printf("# probe_interpretation=%s\n",
           offset_lines == 0 ? "degenerate_same_address" : "measurable");
    printf("# layout_span_pages=%zu training_slot_first=0 "
           "training_slot_count=%zu trigger_slot_first=%zu "
           "trigger_slot_count=%zu\n",
           span_pages, train_pairs,
           POOL_PAGES / span_pages - trigger_batch, trigger_batch);
    printf("# train pairs and trigger pages are disjoint within each round\n");
    printf("# all triggers complete before any target is probed\n");
#if PMU_CORE_X925
    printf("# PMU core=Cortex-X925 events=0x8154,0x8155\n");
#else
    printf("# PMU core=Cortex-A725 events=0x8154,0x8285\n");
#endif

#if ENABLE_PMU
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        fprintf(stderr, "# PMU unavailable; continuing cache-only\n");
        result = 2;
    }
#endif

    run_case("untrained", TRAIN_NONE, offset_lines, offset_lines,
             layout_offset, span_pages,
             rounds, threshold_ns, train_pairs, trigger_batch,
             pre_trigger_action);
    run_case("random_delta_control", TRAIN_RANDOM_DELTA,
             offset_lines, offset_lines, layout_offset, span_pages,
             rounds, threshold_ns, train_pairs, trigger_batch,
             pre_trigger_action);
    run_case("fixed_offset_expected", TRAIN_FIXED_OFFSET,
             offset_lines, offset_lines, layout_offset, span_pages,
             rounds, threshold_ns, train_pairs, trigger_batch,
             pre_trigger_action);

#if ENABLE_PMU
    if (pmu_fd >= 0)
        close(pmu_fd);
#endif
    cleanup_process_action();
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
