#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "until.h"

#define PAGE_LINES (PAGE_SIZE / LINE_SIZE)
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 5
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS PAGE_LINES
#endif

#ifndef CPU_ID
#define CPU_ID -1
#endif

#ifndef USE_NOINLINE_STORE
#define USE_NOINLINE_STORE 1
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#if TRAIN_ACCESS_LOAD
#define TRAIN_ONLY_ACCESSES (TRAIN_ACCESSES - 1)
#define TRIGGER_LINE_INDEX ((TRAIN_ACCESSES - 1) * STRIDE_LINES)
#define PREDICTED_LINE_INDEX (TRAIN_ACCESSES * STRIDE_LINES)
#else
#define TRAIN_ONLY_ACCESSES TRAIN_ACCESSES
#define TRIGGER_LINE_INDEX (TRAIN_ACCESSES * STRIDE_LINES)
#define PREDICTED_LINE_INDEX ((TRAIN_ACCESSES + 1) * STRIDE_LINES)
#endif

#ifndef NO_TRIGGER
#define NO_TRIGGER 0
#endif

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *shared_page;
static uint8_t *dummy_buffer;

static long long latency_sum[PROBE_POSITIONS] = {0};
static int probe_count[PROBE_POSITIONS] = {0};

static size_t trigger_offset;
#if !NO_TRIGGER
static pthread_mutex_t trigger_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t trigger_cond = PTHREAD_COND_INITIALIZER;
static int trigger_state = 0;
#endif

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void set_cpu_if_requested(void) {
#if CPU_ID >= 0
    cpu_set_t mask;

    CPU_ZERO(&mask);
    CPU_SET(CPU_ID, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        die("sched_setaffinity");
    }
#endif
}

static void flush_shared_page(void) {
    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        flush(shared_page + offset);
    }
    mfence();
}

static void dummyAccesses(void) {
#if TRAIN_ACCESS_LOAD
    for (size_t i = 0; i < DUMMY_BUFFER_SIZE; i += LINE_SIZE) {
        mStore_inline(dummy_buffer + i);
    }
    mfence();
#else
    dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
#endif
}

static inline __attribute__((always_inline)) void access_for_test(void *addr) {
#if TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
#if USE_NOINLINE_STORE
    mStore_noinline(addr);
#else
    mStore_inline(addr);
#endif
#endif
}

static void train_in_thread0(int stride_bytes) {
    for (int step = 0; step < TRAIN_ONLY_ACCESSES; step++) {
        access_for_test(shared_page + ((size_t)step * (size_t)stride_bytes));
    }
}

static void delay_after_trigger(void) {
    uint64_t dummy = 0;

    for (int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
    }
    for (int i = 0; i < 1000; i++) {
        nop();
    }

    (void)dummy;
}

#if !NO_TRIGGER
static void request_trigger(void) {
    pthread_mutex_lock(&trigger_mutex);
    trigger_state = 1;
    pthread_cond_signal(&trigger_cond);
    while (trigger_state != 2) {
        pthread_cond_wait(&trigger_cond, &trigger_mutex);
    }
    trigger_state = 0;
    pthread_mutex_unlock(&trigger_mutex);
}

static void stop_trigger_thread(void) {
    pthread_mutex_lock(&trigger_mutex);
    trigger_state = 3;
    pthread_cond_signal(&trigger_cond);
    pthread_mutex_unlock(&trigger_mutex);
}

static void *trigger_thread_main(void *arg) {
    (void)arg;

    set_cpu_if_requested();
    for (;;) {
        pthread_mutex_lock(&trigger_mutex);
        while (trigger_state != 1 && trigger_state != 3) {
            pthread_cond_wait(&trigger_cond, &trigger_mutex);
        }
        if (trigger_state == 3) {
            pthread_mutex_unlock(&trigger_mutex);
            return NULL;
        }
        pthread_mutex_unlock(&trigger_mutex);

        access_for_test(shared_page + trigger_offset);

        pthread_mutex_lock(&trigger_mutex);
        trigger_state = 2;
        pthread_cond_signal(&trigger_cond);
        pthread_mutex_unlock(&trigger_mutex);
    }
}
#endif

static void print_header(int stride_bytes, int trigger_line,
                         int predicted_line) {
    printf("# arm64 cross-thread %s-stride retention test\n",
#if TRAIN_ACCESS_LOAD
           "load"
#else
           "store"
#endif
    );
    printf("# thread0 trains %d %ss, thread1 triggers access %d\n",
           TRAIN_ONLY_ACCESSES,
#if TRAIN_ACCESS_LOAD
           "load",
#else
           "store",
#endif
           TRAIN_ACCESSES);
    printf("# shared_page=0x%016lx\n", (unsigned long)(uintptr_t)shared_page);
    printf("# stride_lines=%d stride_bytes=%d rounds=%d probe_positions=%d\n",
           STRIDE_LINES, stride_bytes, ROUNDS, PROBE_POSITIONS);
    printf("# trigger_line=%d predicted_line=%d access=%s pc=%s\n",
           trigger_line, predicted_line,
#if TRAIN_ACCESS_LOAD
           "load",
           "noinline_same_pc_per_process"
#else
           "store",
#if USE_NOINLINE_STORE
           "noinline_same_pc_per_process"
#else
           "inline_call_site_pc"
#endif
#endif
    );
    printf("# trigger=%s\n",
#if NO_TRIGGER
           "disabled"
#else
           "thread1"
#endif
    );
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
}

int main(void) {
    int stride_bytes = STRIDE_LINES * LINE_SIZE;
    int trigger_line = TRIGGER_LINE_INDEX;
    int predicted_line = PREDICTED_LINE_INDEX;
    unsigned int junk = 0;
#if !NO_TRIGGER
    pthread_t trigger_thread;
#endif

    if (stride_bytes <= 0 ||
        (size_t)PREDICTED_LINE_INDEX * LINE_SIZE >= PAGE_SIZE) {
        fprintf(stderr, "training/trigger/predicted lines must fit in one page\n");
        return 1;
    }
    if (TRAIN_ACCESS_LOAD && TRAIN_ACCESSES < 2) {
        fprintf(stderr, "TRAIN_ACCESSES must be >= 2 for load mode\n");
        return 1;
    }
    if (PROBE_POSITIONS > PAGE_LINES) {
        fprintf(stderr, "PROBE_POSITIONS must be <= %d\n", PAGE_LINES);
        return 1;
    }

    set_cpu_if_requested();

    shared_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (shared_page == MAP_FAILED) {
        die("mmap shared_page");
    }
    memset(shared_page, 0xff, PAGE_SIZE);

    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        die("mmap dummy_buffer");
    }

    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        mLoad(shared_page + offset);
    }

    trigger_offset = (size_t)TRAIN_ONLY_ACCESSES * (size_t)stride_bytes;

#if !NO_TRIGGER
    if (pthread_create(&trigger_thread, NULL, trigger_thread_main, NULL) != 0) {
        die("pthread_create");
    }
#endif

    print_header(stride_bytes, trigger_line, predicted_line);

    for (uint64_t round = 0; round < ROUNDS; round++) {
        int probe_pos = round % PROBE_POSITIONS;
        volatile uint8_t *probe_addr = shared_page + (probe_pos * LINE_SIZE);
        uint64_t time1;
        uint64_t time2;

        flush_shared_page();
        dummyAccesses();

        train_in_thread0(stride_bytes);
#if !NO_TRIGGER
        request_trigger();
#endif
        delay_after_trigger();

        time1 = timestamp();
        junk += *probe_addr;
        time2 = timestamp() - time1;

        latency_sum[probe_pos] += (long long)time2;
        probe_count[probe_pos]++;
    }

    for (int pos = 0; pos < PROBE_POSITIONS; pos++) {
        long long avg_ns = 0;

        if (probe_count[pos] > 0) {
            avg_ns = latency_sum[pos] / probe_count[pos];
        }
        printf("%3d\t%12d\t%10lld\t%5d\n",
               pos, pos * LINE_SIZE, avg_ns, probe_count[pos]);
    }

#if !NO_TRIGGER
    stop_trigger_thread();
    pthread_join(trigger_thread, NULL);
#endif

    (void)junk;
    return 0;
}
