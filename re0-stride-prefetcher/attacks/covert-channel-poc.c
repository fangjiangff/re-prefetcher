#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../until.h"

#define ITEMS 10240
#define ARRAY_SIZE (ITEMS * LINE_SIZE)
#define REPETITIONS 2

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
#error "covert-channel-poc requires AArch64"
#endif

#define ENABLE_CPP_RCTX 1
#define PMU_CORE_X925 1
#define PMU_CORE_A55 0
#include "../pmu.h"

static uint8_t array2[ARRAY_SIZE] __attribute__((aligned(4096)));

static inline __attribute__((always_inline)) void stride_access(void *addr) {
#if TRAIN_ACCESS_PREFETCH
    mPrefetch_noinline(addr);
#elif TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
    mStore_inline(addr);
#endif
}

static void reset_training_state(int stride, int train_step) {
    for (int step = 0; step < train_step + 1; step++) {
        flush(array2 + step * stride);
    }
    cpp_rctx();
}

static void transmit_bit(int bit, int stride, int train_step) {
    if (bit) {
        for (int step = 0; step < train_step - 1; step++) {
            stride_access(array2 + step * stride);
            nops();
        }
    }

    stride_access(array2 + (train_step - 1) * stride);
    nops();
}

static int infer_bit_from_pmu(int pmu_running) {
    static uint64_t previous_value[PMU_COUNTER_COUNT];
    int inferred_bit = -1;

    for (int i = 0; i < PMU_COUNTER_COUNT; i++) {
        if (!pmu_running || pmu_counters[i].fd < 0) {
            continue;
        }

        uint64_t value =
            pmu_counters[i].accumulated_value - previous_value[i];
        previous_value[i] = pmu_counters[i].accumulated_value;

        if (strcmp(pmu_counters[i].name, "l2d_cache_hwprf") == 0) {
            inferred_bit = value > 1 ? 1 : 0;
        }
    }

    return inferred_bit;
}

int main(void) {
    const uint64_t rounds = ROUNDS;
    const int stride = STRIDE_BYTES;
    const int train_step = TRAIN_STEP;

    if (rounds == 0 || stride <= 0 || train_step < 1 ||
        (uint64_t)train_step * (uint64_t)stride >= ARRAY_SIZE) {
        fprintf(stderr, "invalid rounds, stride, or train-step\n");
        return 1;
    }

    memset(array2, -1, sizeof(array2));

    uint8_t info[ROUNDS];
    for (uint64_t i = 0; i < rounds; i++) {
        info[i] = (uint8_t)(rand() % 2);
    }

    int pmu_ready = pmu_setup() == 0;
    if (!pmu_ready) {
        fprintf(stderr, "PMU setup failed\n");
        return 1;
    }

    uint64_t correct_rounds = 0;
    clock_t start = clock();

    for (uint64_t round = 0; round < rounds; round++) {
        int pmu_running = 1;

#if TRAIN_ACCESS_LOAD
        for (int rep = 0; rep < REPETITIONS; rep++) {
            reset_training_state(stride, train_step);

            int repetition_pmu_running = pmu_start() == 0;
            if (!repetition_pmu_running) {
                pmu_running = 0;
            }

            transmit_bit(info[round], stride, train_step);

            if (repetition_pmu_running) {
                pmu_stop_and_accumulate();
            }
        }
#else
        pmu_running = pmu_start() == 0;
        for (int rep = 0; rep < REPETITIONS; rep++) {
            reset_training_state(stride, train_step);
            transmit_bit(info[round], stride, train_step);
        }
        if (pmu_running) {
            pmu_stop_and_accumulate();
        }
#endif

        int inferred_bit = infer_bit_from_pmu(pmu_running);
        correct_rounds += (uint64_t)(inferred_bit == info[round]);
    }

    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;
    double accuracy = 100.0 * (double)correct_rounds / (double)rounds;
    double bandwidth_kib_s = (double)rounds / time_spent / 1024.0;

    printf("# Execution time %.6f s\n", time_spent);
    printf("# accuracy=%.2f%% (%llu/%llu)\n",
           accuracy,
           (unsigned long long)correct_rounds,
           (unsigned long long)rounds);
    printf("# bandwidth=%.2f Kib/s\n", bandwidth_kib_s);

    pmu_cleanup();
    return 0;
}
