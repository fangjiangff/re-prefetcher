#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../until.h"

#define ITEMS 10240
#define ARRAY_SIZE (ITEMS * LINE_SIZE)
#define PROBE_POSITIONS 10
#define PROBE_REPETITIONS 20
#define DUMMY_BUFFER_PAGES 64
#define DUMMY_BUFFER_SIZE (DUMMY_BUFFER_PAGES * PAGE_SIZE)
#define CACHE_HIT_THRESHOLD 100

#ifndef STRIDE_BYTES
#define STRIDE_BYTES 64
#endif

#ifndef TRAIN_STEP
#define TRAIN_STEP 10
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef TRAIN_ACCESS_PREFETCH
#define TRAIN_ACCESS_PREFETCH 0
#endif

#if TRAIN_ACCESS_LOAD && TRAIN_ACCESS_PREFETCH
#error "Only one train access mode can be enabled"
#endif

#if !defined(__aarch64__)
#error "covert-channel-cache requires AArch64"
#endif

static uint8_t array2[ARRAY_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t dummy_buffer[DUMMY_BUFFER_SIZE]
    __attribute__((aligned(PAGE_SIZE)));

static inline __attribute__((always_inline)) void stride_access(void *addr) {
#if TRAIN_ACCESS_PREFETCH
    mPrefetch_noinline(addr);
#elif TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
    mStore_inline(addr);
#endif
}

static void reset_prefetcher_state(int stride, int train_step) {
    for (size_t offset = 0; offset < DUMMY_BUFFER_SIZE; offset += 256) {
        mPrefetch_inline(dummy_buffer + offset);
    }

    for (int step = 0; step < train_step + PROBE_POSITIONS; step++) {
        flush(array2 + step * stride);
    }
    mfence();
    cpp_rctx();
}

static void transmit_bit(int bit, int stride, int train_step) {
    if (bit) {
        for (int step = 0; step < train_step - 1; step++) {
            stride_access(array2 + step * stride);
        }
    }

    stride_access(array2 + (train_step - 1) * stride);
}

static int receive_bit(int transmitted_bit, int stride, int train_step) {
    uint64_t latency_sum[PROBE_POSITIONS] = {0};
    unsigned int probe_count[PROBE_POSITIONS] = {0};

    for (int repeat = 0; repeat < PROBE_REPETITIONS; repeat++) {
        int probe_position = repeat % PROBE_POSITIONS;
        reset_prefetcher_state(stride, train_step);
        transmit_bit(transmitted_bit, stride, train_step);

        uint8_t *probe_addr =
            array2 + (train_step + probe_position) * stride;
        uint64_t start = timestamp();
        mStore_inline(probe_addr);
        uint64_t latency = timestamp() - start;

        latency_sum[probe_position] += latency;
        probe_count[probe_position]++;
    }

    for (int probe_position = 0; probe_position < PROBE_POSITIONS;
         probe_position++) {
        uint64_t average =
            latency_sum[probe_position] / probe_count[probe_position];
        if (average < CACHE_HIT_THRESHOLD) {
            return 1;
        }
    }

    return 0;
}

int main(void) {
    const uint64_t rounds = ROUNDS;
    const int stride = STRIDE_BYTES;
    const int train_step = TRAIN_STEP;

    if (rounds == 0 || stride <= 0 || train_step < 1 ||
        (uint64_t)(train_step + PROBE_POSITIONS - 1) * (uint64_t)stride >=
            ARRAY_SIZE) {
        fprintf(stderr, "invalid rounds, stride, or train-step\n");
        return 1;
    }

    memset(array2, -1, sizeof(array2));
    memset(dummy_buffer, -1, sizeof(dummy_buffer));

    uint64_t correct_rounds = 0;
    clock_t start = clock();

    for (uint64_t round = 0; round < rounds; round++) {
        int transmitted_bit = rand() % 2;
        int received_bit = receive_bit(transmitted_bit, stride, train_step);
        correct_rounds += (uint64_t)(received_bit == transmitted_bit);
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double accuracy = 100.0 * (double)correct_rounds / (double)rounds;
    double bandwidth_kib_s =
        elapsed > 0.0 ? (double)rounds / elapsed / 1024.0 : 0.0;

    printf("# Execution time %.6f s\n", elapsed);
    printf("# accuracy=%.2f%% (%llu/%llu)\n",
           accuracy,
           (unsigned long long)correct_rounds,
           (unsigned long long)rounds);
    printf("# bandwidth=%.2f Kib/s\n", bandwidth_kib_s);

    return 0;
}
