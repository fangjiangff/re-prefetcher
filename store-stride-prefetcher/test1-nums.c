#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include "until.h"

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

#ifndef OTHER_PAGES_N
#define OTHER_PAGES_N 0
#endif

#ifndef NO_TRIGGER
#define NO_TRIGGER 0
#endif

#ifndef CONTEXT_SWITCH_BEFORE_TRIGGER
#define CONTEXT_SWITCH_BEFORE_TRIGGER 0
#endif

#ifndef CONTEXT_SWITCH_YIELDS
#define CONTEXT_SWITCH_YIELDS 1
#endif

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));
long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

static uint8_t *other_pages;

static inline __attribute__((always_inline)) void stride_access(void *addr) {
    mStore_noinline(addr);
}

static void context_switch_before_trigger(void) {
#if CONTEXT_SWITCH_BEFORE_TRIGGER
    for (int i = 0; i < CONTEXT_SWITCH_YIELDS; i++) {
        sched_yield();
    }
#endif
}

static void print_test_header(int stride, int train_step, uint64_t rounds) {
    printf("# %s store-stride other-page-count prefetch latency map\n",
#ifdef __x86_64__
           "x86_64"
#elif defined(__aarch64__)
           "arm64"
#else
           "unknown"
#endif
    );
    printf("# access mode: store (%s), same noinline PC for victim train and trigger\n",
#ifdef __x86_64__
           "movb store"
#else
           "strb"
#endif
    );
    printf("# stride_bytes=%d train_step=%d other_pages=%d rounds=%llu probe_positions=%d\n",
           stride, train_step, OTHER_PAGES_N, (unsigned long long)rounds,
           PROBE_POSITIONS);
    printf("# timer: %s %s\n", TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME);
    printf("# position\toffset_bytes\tavg_%s\tprobes\n", TIMESTAMP_UNIT_NAME);
}

static void flush_victim_lines(void) {
    for (uint64_t offset = 0; offset < Items * LINE_SIZE; offset += LINE_SIZE) {
        flush(&array2[offset]);
    }
}

static void flush_other_pages(int stride, int train_step) {
#if OTHER_PAGES_N > 0
    for (int page = 0; page < OTHER_PAGES_N; page++) {
        uint8_t *base = other_pages + ((uint64_t)page * PAGE_SIZE);
        for (int step = 0; step < train_step - 1; step++) {
            flush(base + 3 * LINE_SIZE + ((uint64_t)step * (uint64_t)stride));
        }
    }
#else
    (void)stride;
    (void)train_step;
#endif
}

static void train_other_pages(int stride, int train_step) {
#if OTHER_PAGES_N > 0
    for (int page = 0; page < OTHER_PAGES_N; page++) {
        uint8_t *base = other_pages + ((uint64_t)page * PAGE_SIZE);
        // for (int step = 0; step < train_step - 1; step++) {
        for (int step = 0; step < 1; step++) {
            stride_access(base + 3 * LINE_SIZE + ((uint64_t)step * (uint64_t)stride));
        }
    }
#else
    (void)stride;
    (void)train_step;
#endif
}

int main(void) {
    register uint64_t time1, time2;
    volatile uint8_t *probe_addr;

    memset(array2, -1, Items * LINE_SIZE * sizeof(uint8_t));

#if OTHER_PAGES_N > 0
    other_pages = (uint8_t *)mmap(NULL,
                                  (size_t)OTHER_PAGES_N * PAGE_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                  -1,
                                  0);
    if (other_pages == MAP_FAILED) {
        fprintf(stderr, "failed to map other_pages\n");
        return 1;
    }
    memset(other_pages, -1, (size_t)OTHER_PAGES_N * PAGE_SIZE);
#endif

    uint64_t rounds = ROUNDS;
    int stride = STRIDE_BYTES;
    int train_step = TRAIN_STEP;

    uint64_t latency2_sum = 0;

    if ((uint64_t)(train_step - 1) * (uint64_t)stride >= Items * LINE_SIZE) {
        fprintf(stderr, "training range exceeds array2 size\n");
        return 1;
    }
#if OTHER_PAGES_N > 0
    if (train_step > 1 &&
        (uint64_t)(train_step - 2) * (uint64_t)stride >= PAGE_SIZE) {
        fprintf(stderr, "other-page training range exceeds page size\n");
        return 1;
    }
#endif
    if ((uint64_t)train_step * (uint64_t)stride >= Items * LINE_SIZE) {
        fprintf(stderr, "predicted line exceeds array2 size\n");
        return 1;
    }

    print_test_header(stride, train_step, rounds);

    for (uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
        // cpp_rctx();
        flush_victim_lines();
        flush_other_pages(stride, train_step);
        mfence();

        for (int step = 0; step < train_step - 1; step++) {
            stride_access(array2 + ((uint64_t)step * (uint64_t)stride));
        }

        // train_other_pages(stride, train_step);
        // uint8_t *base = other_pages;
        // stride_access(base + 3 * LINE_SIZE);
        // context_switch_before_trigger();

#if !NO_TRIGGER
        stride_access(array2 + ((uint64_t)(train_step - 1) * (uint64_t)stride));
#endif

        int probe_pos = (int)(atkRound % PROBE_POSITIONS);    
        probe_addr = array2 + ((uint64_t)probe_pos * LINE_SIZE);

        // probe_addr = array2 + ((uint64_t)(train_step) * (uint64_t)stride);

        time1 = timestamp();
        mStore_inline((void *)probe_addr);
        time2 = timestamp() - time1;

        latency2_sum += time2;
        latency_sum[probe_pos] += time2;
        probe_count[probe_pos]++;
    }
    // printf("avg_latency=%llu\n",
    //        (unsigned long long)(latency2_sum / rounds));
    for (int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
        long long int avg = 0;
        if (probe_count[probe_pos] > 0) {
            avg = latency_sum[probe_pos] / probe_count[probe_pos];
        }
        printf("%3d\t%12d\t%10lld\t%5d\n",
               probe_pos,
               probe_pos * LINE_SIZE,
               avg,
               probe_count[probe_pos]);
    }
    printf("\n");

    return 0;
}
