#define _GNU_SOURCE

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../until.h"

#define Items 10240

#ifndef STRIDE_BYTES
#define STRIDE_BYTES 64
#endif

#ifndef TRAIN_STEP
#define TRAIN_STEP 10
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

/*
 * Which accesses fault:
 *   0: no fault (baseline)
 *   1: only the last, trigger access faults
 *   2: every access in the stride sequence faults
 */
#ifndef FAULT_SCOPE
#define FAULT_SCOPE 1
#endif

/*
 * 0 (default): the handler siglongjmp()s over the faulting store.  The store
 *              never retires, so this is the useful mode for asking whether
 *              the faulting request itself can train/trigger the prefetcher.
 * 1:           the handler makes the page writable and returns.  Linux retries
 *              the store, which then retires normally.  This is a positive
 *              control, not proof that the faulting attempt trained anything.
 */
#ifndef FAULT_RECOVERY_RETRY
#define FAULT_RECOVERY_RETRY 0
#endif

#ifndef SINGLE_PROBE
#define SINGLE_PROBE 0
#endif

#ifndef SINGLE_PROBE_POSITION
#define SINGLE_PROBE_POSITION (TRAIN_STEP * (STRIDE_BYTES / LINE_SIZE))
#endif

#if FAULT_SCOPE < 0 || FAULT_SCOPE > 2
#error "FAULT_SCOPE must be 0, 1, or 2"
#endif

#if SINGLE_PROBE &&                                                     \
    (SINGLE_PROBE_POSITION < 0 || SINGLE_PROBE_POSITION >= PROBE_POSITIONS)
#error "SINGLE_PROBE_POSITION must be inside PROBE_POSITIONS"
#endif

#define ARRAY2_SIZE (Items * LINE_SIZE * sizeof(uint8_t))

static uint8_t *array2;
static size_t page_size;

static long long latency_sum[PROBE_POSITIONS];
static int probe_count[PROBE_POSITIONS];

static sigjmp_buf fault_env;
static volatile sig_atomic_t fault_armed;
static volatile sig_atomic_t fault_count;
static uint8_t *volatile fault_page;

static uint8_t *page_start(const void *addr)
{
    uintptr_t value = (uintptr_t)addr;

    return (uint8_t *)(value - value % page_size);
}

static void segv_handler(int signo, siginfo_t *info, void *ucontext)
{
    uint8_t *page = fault_page;
    uintptr_t bad = (uintptr_t)info->si_addr;

    (void)ucontext;
    if (signo != SIGSEGV || !fault_armed || page == NULL ||
        bad < (uintptr_t)page || bad >= (uintptr_t)page + page_size) {
        _exit(128 + SIGSEGV);
    }

    fault_count++;
    fault_armed = 0;

#if FAULT_RECOVERY_RETRY
    /* On Linux the libc mprotect wrapper is a direct system-call wrapper. */
    if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0)
        _exit(127);
    return; /* Return to the faulting PC; the store is retried. */
#else
    siglongjmp(fault_env, 1); /* The faulting store never retires. */
#endif
}

static int install_segv_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = segv_handler;
    action.sa_flags = SA_SIGINFO;
    return sigaction(SIGSEGV, &action, NULL);
}

/* Keep all stride stores at one static PC, as in test0-exist.c. */
static inline __attribute__((always_inline)) void stride_store(void *addr)
{
    mStore_noinline(addr);
}

static int faulting_stride_store(void *addr)
{
    uint8_t *page = page_start(addr);

#if !FAULT_RECOVERY_RETRY
    if (sigsetjmp(fault_env, 1) != 0) {
        /* We arrive here from the SIGSEGV handler. */
        if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0)
            return -1;
        fault_page = NULL;
        return 0;
    }
#endif

    /* PROT_READ makes the page resident and readable, but a store will fault. */
    if (mprotect(page, page_size, PROT_READ) != 0)
        return -1;

    fault_page = page;
    fault_armed = 1;
    stride_store(addr);

#if FAULT_RECOVERY_RETRY
    /* The handler already restored RW before the kernel retried the store. */
    fault_page = NULL;
    fault_armed = 0;
#else
    /* Reaching this point means the supposedly faulting store did not fault. */
    fault_armed = 0;
    fault_page = NULL;
    errno = EFAULT;
    return -1;
#endif
    return 0;
}

static int should_fault(int step)
{
#if FAULT_SCOPE == 0
    (void)step;
    return 0;
#elif FAULT_SCOPE == 1
    return step == TRAIN_STEP - 1;
#else
    (void)step;
    return 1;
#endif
}

int main(void)
{
    uint64_t rounds = ROUNDS;
    int stride = STRIDE_BYTES;
    int train_step = TRAIN_STEP;

    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size == 0 || page_size == (size_t)-1) {
        perror("sysconf(_SC_PAGESIZE)");
        return 1;
    }
    if (train_step < 1 || stride < 1 ||
        (uint64_t)train_step * (uint64_t)stride >= ARRAY2_SIZE) {
        fprintf(stderr, "invalid training/probe range\n");
        return 1;
    }
    if ((uint64_t)(PROBE_POSITIONS - 1) * LINE_SIZE >= ARRAY2_SIZE) {
        fprintf(stderr, "probe range exceeds array2 size\n");
        return 1;
    }
    if (install_segv_handler() != 0) {
        perror("sigaction");
        return 1;
    }

    array2 = mmap(NULL, ARRAY2_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (array2 == MAP_FAILED) {
        perror("mmap array2");
        return 1;
    }
    memset(array2, -1, ARRAY2_SIZE);
    if (mlock(array2, ARRAY2_SIZE) != 0)
        perror("mlock array2");

    printf("# store-stride page-fault test\n");
    printf("# fault_scope=%d recovery=%s stride=%d train_step=%d rounds=%llu\n",
           FAULT_SCOPE, FAULT_RECOVERY_RETRY ? "retry" : "skip", stride,
           train_step, (unsigned long long)rounds);
    printf("# timer=%s unit=%s page_size=%zu\n", TIMESTAMP_SOURCE_NAME,
           TIMESTAMP_UNIT_NAME, page_size);

    for (uint64_t round = 0; round < rounds; round++) {
        for (uint64_t offset = 0; offset < ARRAY2_SIZE; offset += LINE_SIZE)
            flush(array2 + offset);
        cpp_rctx();

        for (int step = 0; step < train_step; step++) {
            uint8_t *addr = array2 + (uint64_t)step * stride;

            if (should_fault(step)) {
                if (faulting_stride_store(addr) != 0) {
                    perror("faulting_stride_store");
                    return 1;
                }
            } else {
                stride_store(addr);
            }
        }

#if SINGLE_PROBE
        int probe_pos = SINGLE_PROBE_POSITION;
#else
        int probe_pos = (int)((round * 13) % PROBE_POSITIONS);
#endif
        volatile uint8_t *probe_addr = array2 + (uint64_t)probe_pos * LINE_SIZE;
        uint64_t start = timestamp();
        mStore_inline((void *)probe_addr);
        uint64_t elapsed = timestamp() - start;

        latency_sum[probe_pos] += (long long)elapsed;
        probe_count[probe_pos]++;
    }

    sig_atomic_t expected_faults = 0;
#if FAULT_SCOPE == 1
    expected_faults = (sig_atomic_t)rounds;
#elif FAULT_SCOPE == 2
    expected_faults = (sig_atomic_t)(rounds * (uint64_t)train_step);
#endif
    printf("# faults=%d expected=%d\n", (int)fault_count,
           (int)expected_faults);
    if (fault_count != expected_faults) {
        fprintf(stderr, "unexpected SIGSEGV count: got %d, expected %d\n",
                (int)fault_count, (int)expected_faults);
        return 1;
    }

#if SINGLE_PROBE
    int first_probe_pos = SINGLE_PROBE_POSITION;
    int last_probe_pos = SINGLE_PROBE_POSITION + 1;
#else
    int first_probe_pos = 0;
    int last_probe_pos = PROBE_POSITIONS;
#endif
    printf("# pos offset_bytes avg_%s probes\n", TIMESTAMP_UNIT_NAME);
    for (int pos = first_probe_pos; pos < last_probe_pos; pos++) {
        long long avg = probe_count[pos] ? latency_sum[pos] / probe_count[pos] : 0;

        printf("%3d\t%12d\t%10lld\t%5d\n", pos, pos * LINE_SIZE, avg,
               probe_count[pos]);
    }

    munlock(array2, ARRAY2_SIZE);
    munmap(array2, ARRAY2_SIZE);
    return 0;
}
