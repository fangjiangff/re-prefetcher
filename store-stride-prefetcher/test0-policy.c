#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "until.h"

#define Items 10240

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef ACCESS_SEQUENCE
#define ACCESS_SEQUENCE 0, 24, 11
#endif

#ifndef ACCESS_SEQUENCE_LEN
#define ACCESS_SEQUENCE_LEN 3
#endif

#ifdef ACCESS_TYPE_SEQUENCE
#define HAS_ACCESS_TYPE_SEQUENCE 1
#else
#define HAS_ACCESS_TYPE_SEQUENCE 0
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef TRAIN_ACCESS_PREFETCH
#define TRAIN_ACCESS_PREFETCH 0
#endif

#ifndef DUMMY_BUFFER_PAGES
#define DUMMY_BUFFER_PAGES 10
#endif

#if TRAIN_ACCESS_LOAD && TRAIN_ACCESS_PREFETCH
#error "Only one train access mode can be enabled"
#endif

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static const int access_sequence[ACCESS_SEQUENCE_LEN] = { ACCESS_SEQUENCE };
#if HAS_ACCESS_TYPE_SEQUENCE
static const char access_type_sequence[ACCESS_SEQUENCE_LEN] = { ACCESS_TYPE_SEQUENCE };
#endif
static uint8_t *dummy_buffer;

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));
uint8_t array1[100 * LINE_SIZE] = {0};

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

static void dummyAccesses(void) {
    for (uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j += LINE_SIZE) {
        mPrefetch_inline(dummy_buffer + j);
    }
}

static inline __attribute__((always_inline)) void policy_access(void *addr, char type) {
#if HAS_ACCESS_TYPE_SEQUENCE
    if (type == 'l') {
        mLoad_noinline(addr);
    } else {
        mStore_noinline(addr);
    }
#else
    (void)type;
#if TRAIN_ACCESS_PREFETCH
    mPrefetch_noinline(addr);
#elif TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
    mStore_noinline(addr);
#endif
#endif
}

static void delay_after_accesses(void) {
    uint64_t dummy = 0;

    for (int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
        mfence();
    }
    for (int i = 0; i < 100; i++) {
        nop();
    }

    (void)dummy;
}

static void print_header(uint64_t rounds) {
    printf("# %s policy access prefetch map\n",
#ifdef __x86_64__
           "x86_64"
#elif defined(__aarch64__)
           "arm64"
#else
           "unknown"
#endif
    );
#if HAS_ACCESS_TYPE_SEQUENCE
    printf("# access types:");
    for (int i = 0; i < ACCESS_SEQUENCE_LEN; i++) {
        printf(" %c", access_type_sequence[i]);
    }
    printf("\n");
#else
    printf("# access mode: %s\n",
#if TRAIN_ACCESS_PREFETCH
           "prefetch"
#elif TRAIN_ACCESS_LOAD
           "load"
#else
           "store"
#endif
    );
#endif
    printf("# sequence_lines:");
    for (int i = 0; i < ACCESS_SEQUENCE_LEN; i++) {
        printf(" %d", access_sequence[i]);
    }
    printf("\n");
    printf("# rounds=%llu probe_positions=%d\n",
           (unsigned long long)rounds, PROBE_POSITIONS);
    printf("# timer: %s %s\n", TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME);
    printf("# position\toffset_bytes\tavg_%s\tprobes\n", TIMESTAMP_UNIT_NAME);
}

int main(void) {
    register uint64_t time1, time2;
    volatile uint8_t *probe_addr;
    unsigned int junk = 0;
    uint64_t rounds = ROUNDS;

    if (ACCESS_SEQUENCE_LEN <= 0) {
        fprintf(stderr, "ACCESS_SEQUENCE_LEN must be positive\n");
        return 1;
    }
    if (PROBE_POSITIONS <= 0) {
        fprintf(stderr, "PROBE_POSITIONS must be positive\n");
        return 1;
    }
    for (int i = 0; i < ACCESS_SEQUENCE_LEN; i++) {
        if (access_sequence[i] < 0 || access_sequence[i] >= Items) {
            fprintf(stderr, "access sequence line out of array2 range: %d\n",
                    access_sequence[i]);
            return 1;
        }
#if HAS_ACCESS_TYPE_SEQUENCE
        if (access_type_sequence[i] != 's' && access_type_sequence[i] != 'l') {
            fprintf(stderr, "access type must be 's' or 'l': %c\n",
                    access_type_sequence[i]);
            return 1;
        }
#endif
    }
    if (PROBE_POSITIONS > Items) {
        fprintf(stderr, "PROBE_POSITIONS exceeds array2 line count\n");
        return 1;
    }

    memset(array2, -1, Items * LINE_SIZE * sizeof(uint8_t));

    dummy_buffer = (uint8_t *)mmap(NULL, DUMMY_BUFFER_SIZE,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                   0, 0);
    if (dummy_buffer == MAP_FAILED) {
        fprintf(stderr, "failed to map dummy buffer\n");
        return 1;
    }

    // for (int i = 0; i < Items; i++) {
    //     mLoad(&array2[i * LINE_SIZE]);
    // }

    // for (uint64_t offset = 0; offset < Items * LINE_SIZE; offset += LINE_SIZE) {
    //     flush(&array2[offset]);
    // }
    // mfence();

    print_header(rounds);

    for (uint64_t atkRound = 0; atkRound < rounds; atkRound++) {
        // dummyAccesses();
        cpp_rctx();

        for (uint64_t offset = 0; offset < Items * LINE_SIZE; offset += LINE_SIZE) {
            flush(&array2[offset]);
        }
        // mfence();

        for (int i = 0; i < ACCESS_SEQUENCE_LEN; i++) {
#if HAS_ACCESS_TYPE_SEQUENCE
            policy_access(array2 + access_sequence[i] * LINE_SIZE,
                          access_type_sequence[i]);
#else
            policy_access(array2 + access_sequence[i] * LINE_SIZE, 0);
#endif
        }

        // delay_after_accesses();

        int probe_pos = (atkRound) % PROBE_POSITIONS;
        probe_addr = array2 + probe_pos * LINE_SIZE;

        time1 = timestamp();
        // junk = *probe_addr;
        mStore_inline(probe_addr);
        time2 = timestamp() - time1;

        latency_sum[probe_pos] += time2;
        probe_count[probe_pos]++;
    }

    for (int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
        long long int avg = 0;
        if (probe_count[probe_pos] > 0) {
            avg = latency_sum[probe_pos] / probe_count[probe_pos];
        }
        printf("%3d\t%12d\t%10lld\t%5d\n",
               probe_pos, probe_pos * LINE_SIZE, avg, probe_count[probe_pos]);
    }

    (void)junk;
    return 0;
}
