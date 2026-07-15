#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
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

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef TRAIN_ACCESS_PREFETCH
#define TRAIN_ACCESS_PREFETCH 0
#endif

#if TRAIN_ACCESS_LOAD && TRAIN_ACCESS_PREFETCH
#error "Only one train access mode can be enabled"
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

#define ARRAY2_SIZE (Items * LINE_SIZE * sizeof(uint8_t))

static uint8_t *array2;
static long long int latency_sum[PROBE_POSITIONS] = {0};
static int probe_count[PROBE_POSITIONS] = {0};

static pthread_mutex_t trigger_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t trigger_cond = PTHREAD_COND_INITIALIZER;
static int trigger_state = 0;
static int trigger_train_step = 0;
static int trigger_stride = 0;

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void context_switch_before_trigger(void) {
#if CONTEXT_SWITCH_BEFORE_TRIGGER
    for (int i = 0; i < CONTEXT_SWITCH_YIELDS; i++) {
        sched_yield();
    }
#endif
}

static inline __attribute__((always_inline)) void train_access(void *addr) {
#if TRAIN_ACCESS_PREFETCH
    mPrefetch_noinline(addr);
#elif TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
    mStore_inline(addr);
#endif
}

static inline __attribute__((always_inline)) void trigger_access(int train_step, int stride) {
#if TRAIN_ACCESS_PREFETCH
    mPrefetch_noinline(array2 + ((train_step - 1) * stride));
#elif TRAIN_ACCESS_LOAD
    mLoad_noinline(array2 + ((train_step - 1) * stride));
#else
    mStore_inline(array2 + ((train_step - 1) * stride));
#endif
    nops();
}

static void *trigger_thread_main(void *arg) {
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&trigger_mutex);
        while (trigger_state == 0) {
            pthread_cond_wait(&trigger_cond, &trigger_mutex);
        }
        if (trigger_state == 3) {
            pthread_mutex_unlock(&trigger_mutex);
            return NULL;
        }
        int train_step = trigger_train_step;
        int stride = trigger_stride;
        pthread_mutex_unlock(&trigger_mutex);

        trigger_access(train_step, stride);

        pthread_mutex_lock(&trigger_mutex);
        trigger_state = 2;
        pthread_cond_signal(&trigger_cond);
        pthread_mutex_unlock(&trigger_mutex);
    }
}

static void start_trigger_thread(pthread_t *thread, int train_step, int stride) {
    trigger_train_step = train_step;
    trigger_stride = stride;
    if (pthread_create(thread, NULL, trigger_thread_main, NULL) != 0) {
        die("pthread_create trigger thread");
    }
}

static void request_thread_trigger(void) {
    pthread_mutex_lock(&trigger_mutex);
    trigger_state = 1;
    pthread_cond_signal(&trigger_cond);
    while (trigger_state != 2) {
        pthread_cond_wait(&trigger_cond, &trigger_mutex);
    }
    trigger_state = 0;
    pthread_mutex_unlock(&trigger_mutex);
}

static void stop_trigger_thread(pthread_t thread) {
    pthread_mutex_lock(&trigger_mutex);
    trigger_state = 3;
    pthread_cond_signal(&trigger_cond);
    pthread_mutex_unlock(&trigger_mutex);
    pthread_join(thread, NULL);
}

int main(int argc, char **argv) {
    register uint64_t time1, time2;
    volatile uint8_t *probe_addr;
    unsigned int junk = 0;
    int thread_trigger = !(argc > 1 && strcmp(argv[1], "--parent-trigger") == 0);

    array2 = (uint8_t*)mmap(NULL, ARRAY2_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                            -1, 0);
    if (array2 == MAP_FAILED) {
        perror("mmap array2");
        return 1;
    }

    memset(array2, -1, ARRAY2_SIZE);
    if (mlock(array2, ARRAY2_SIZE) != 0) {
        perror("mlock array2");
    }

    uint64_t rounds = ROUNDS;
    int stride = STRIDE_BYTES;
    int train_step = TRAIN_STEP;
    if ((uint64_t)(train_step - 1) * (uint64_t)stride >= Items * LINE_SIZE) {
        fprintf(stderr, "training range exceeds array2 size\n");
        return 1;
    }

    pthread_t trigger_thread;
    if (thread_trigger) {
        start_trigger_thread(&trigger_thread, train_step, stride);
    }

    for (uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
        cpp_rctx();

        for (uint64_t offset = 0; offset < Items * LINE_SIZE; offset += LINE_SIZE) {
            flush(&array2[offset]);
        }

        for (int step = 0; step < train_step - 1; step++) {
            train_access(array2 + (step * stride));
            nops();
        }

        // context_switch_before_trigger();

#if !NO_TRIGGER
        if (thread_trigger) {
            request_thread_trigger();
        } else {
            trigger_access(train_step, stride);
        }
#endif

        int probe_pos = (atkRound * 13) % PROBE_POSITIONS;
        probe_addr = array2 + (probe_pos * LINE_SIZE);
        time1 = timestamp();
        mStore_inline((void*)probe_addr);
        time2 = timestamp() - time1;

        latency_sum[probe_pos] += time2;
        probe_count[probe_pos]++;
    }

    if (thread_trigger) {
        stop_trigger_thread(trigger_thread);
    }

    for (int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
        long long int avg_ns = 0;
        if (probe_count[probe_pos] > 0) {
            avg_ns = latency_sum[probe_pos] / probe_count[probe_pos];
        }
        printf("%3d\t%12d\t%10lld\t%5d\n",
               probe_pos,
               probe_pos * LINE_SIZE,
               avg_ns,
               probe_count[probe_pos]);
    }
    printf("\n");

    (void)junk;
    return 0;
}
