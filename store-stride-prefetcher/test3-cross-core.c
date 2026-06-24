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

#ifndef TRAIN_CORE
#define TRAIN_CORE 1
#endif

#ifndef TRIGGER_CORE
#define TRIGGER_CORE 0
#endif

#ifndef USE_NOINLINE_STORE
#define USE_NOINLINE_STORE 1
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef TRIGGER_ACCESSES
#define TRIGGER_ACCESSES 1
#endif

#define TRAIN_ONLY_ACCESSES TRAIN_ACCESSES
#define FIRST_TRIGGER_LINE_INDEX (TRAIN_ACCESSES * STRIDE_LINES)
#define LAST_TRIGGER_LINE_INDEX ((TRAIN_ACCESSES + TRIGGER_ACCESSES - 1) * STRIDE_LINES)
#define PREDICTED_LINE_INDEX ((TRAIN_ACCESSES + TRIGGER_ACCESSES) * STRIDE_LINES)

#ifndef NO_TRIGGER
#define NO_TRIGGER 0
#endif

#ifndef CONTEXT_SWITCH_ONLY
#define CONTEXT_SWITCH_ONLY 0
#endif

#ifndef TRAIN_CORE_TRIGGER
#define TRAIN_CORE_TRIGGER 0
#endif

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *shared_page;
static uint8_t *dummy_buffer;

static long long latency_sum[PROBE_POSITIONS] = {0};
static int probe_count[PROBE_POSITIONS] = {0};

static int trigger_index;
#if !NO_TRIGGER && !TRAIN_CORE_TRIGGER
static pthread_mutex_t trigger_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t trigger_cond = PTHREAD_COND_INITIALIZER;
static int trigger_state = 0;
#endif

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void set_current_thread_cpu(int cpu) {
    cpu_set_t mask;

    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
        die("pthread_setaffinity_np");
    }
}

static void flush_shared_page(void) {
    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        flush(shared_page + offset);
    }
    mfence();
}

static void dummyAccesses(void) {
    dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
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

static void train_on_train_core(int stride_bytes) {
    for (int step = 0; step < TRAIN_ONLY_ACCESSES; step++) {
        access_for_test(shared_page + ((size_t)step * (size_t)stride_bytes));
    }
}

static size_t trigger_offset_for_index(int index, int stride_bytes) {
    return (size_t)(TRAIN_ONLY_ACCESSES + index) * (size_t)stride_bytes;
}

static void trigger_on_train_core(int stride_bytes) {
    for (int index = 0; index < TRIGGER_ACCESSES; index++) {
        access_for_test(shared_page + trigger_offset_for_index(index,
                                                              stride_bytes));
    }
}

static void delay_after_trigger(void) {
    uint64_t dummy = 0;

    for (int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
    }
    for (int i = 0; i < 100; i++) {
        nop();
    }

    (void)dummy;
}

#if !NO_TRIGGER && !TRAIN_CORE_TRIGGER
static void request_trigger(int index) {
    pthread_mutex_lock(&trigger_mutex);
    trigger_index = index;
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

    set_current_thread_cpu(TRIGGER_CORE);
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

#if CONTEXT_SWITCH_ONLY
        dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
#else
        access_for_test(shared_page + trigger_offset_for_index(trigger_index,
                                                              STRIDE_LINES * LINE_SIZE));
#endif

        pthread_mutex_lock(&trigger_mutex);
        trigger_state = 2;
        pthread_cond_signal(&trigger_cond);
        pthread_mutex_unlock(&trigger_mutex);
    }
}
#endif

static void print_header(int stride_bytes, int first_trigger_line,
                         int last_trigger_line,
                         int predicted_line) {
    printf("# arm64 cross-core %s-stride retention test\n",
#if TRAIN_ACCESS_LOAD
           "load"
#else
           "store"
#endif
    );
    printf("# train_core=%d trigger_core=%d\n", TRAIN_CORE, TRIGGER_CORE);
    printf("# accesses=%d train_only_accesses=%d trigger_accesses=%d\n",
           TRAIN_ONLY_ACCESSES + TRIGGER_ACCESSES,
           TRAIN_ONLY_ACCESSES, TRIGGER_ACCESSES);
    printf("# train core accesses %d %ss, trigger core triggers %d access(es)\n",
           TRAIN_ONLY_ACCESSES,
#if TRAIN_ACCESS_LOAD
           "load",
#else
           "store",
#endif
           TRIGGER_ACCESSES);
    printf("# shared_page=0x%016lx\n", (unsigned long)(uintptr_t)shared_page);
    printf("# stride_lines=%d stride_bytes=%d rounds=%d probe_positions=%d\n",
           STRIDE_LINES, stride_bytes, ROUNDS, PROBE_POSITIONS);
    printf("# trigger_lines=%d..%d predicted_line=%d access=%s pc=%s\n",
           first_trigger_line, last_trigger_line, predicted_line,
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
#elif TRAIN_CORE_TRIGGER
           "train_core"
#elif CONTEXT_SWITCH_ONLY
           "trigger_core_context_switch_then_train_core"
#else
           "trigger_core_thread"
#endif
    );
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
}

int main(void) {
    int stride_bytes = STRIDE_LINES * LINE_SIZE;
    int first_trigger_line = FIRST_TRIGGER_LINE_INDEX;
    int last_trigger_line = LAST_TRIGGER_LINE_INDEX;
    int predicted_line = PREDICTED_LINE_INDEX;
    unsigned int junk = 0;
#if !NO_TRIGGER && !TRAIN_CORE_TRIGGER
    pthread_t trigger_thread;
#endif

    if (TRAIN_CORE < 0 || TRIGGER_CORE < 0) {
        fprintf(stderr, "TRAIN_CORE and TRIGGER_CORE must be >= 0\n");
        return 1;
    }
    if (!TRAIN_CORE_TRIGGER && TRAIN_CORE == TRIGGER_CORE) {
        fprintf(stderr, "TRAIN_CORE and TRIGGER_CORE must be different\n");
        return 1;
    }
    if (stride_bytes <= 0 ||
        (size_t)PREDICTED_LINE_INDEX * LINE_SIZE >= PAGE_SIZE) {
        fprintf(stderr, "training/trigger/predicted lines must fit in one page\n");
        return 1;
    }
    if (TRIGGER_ACCESSES < 1 || TRIGGER_ACCESSES > 2) {
        fprintf(stderr, "TRIGGER_ACCESSES must be 1 or 2\n");
        return 1;
    }
    if (PROBE_POSITIONS > PAGE_LINES) {
        fprintf(stderr, "PROBE_POSITIONS must be <= %d\n", PAGE_LINES);
        return 1;
    }

    set_current_thread_cpu(TRAIN_CORE);

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

#if !NO_TRIGGER && !TRAIN_CORE_TRIGGER
    if (pthread_create(&trigger_thread, NULL, trigger_thread_main, NULL) != 0) {
        die("pthread_create");
    }
#endif

    print_header(stride_bytes, first_trigger_line, last_trigger_line,
                 predicted_line);

    for (uint64_t round = 0; round < ROUNDS; round++) {
        int probe_pos = round % PROBE_POSITIONS;
        volatile uint8_t *probe_addr = shared_page + (probe_pos * LINE_SIZE);
        uint64_t time1;
        uint64_t time2;

        flush_shared_page();
        dummyAccesses();

        train_on_train_core(stride_bytes);
#if !NO_TRIGGER
#if TRAIN_CORE_TRIGGER
        trigger_on_train_core(stride_bytes);
#else
#if CONTEXT_SWITCH_ONLY
        request_trigger(0);
        trigger_on_train_core(stride_bytes);
#else
        for (int index = 0; index < TRIGGER_ACCESSES; index++) {
            request_trigger(index);
        }
#endif
#endif
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

#if !NO_TRIGGER && !TRAIN_CORE_TRIGGER
    stop_trigger_thread();
    pthread_join(trigger_thread, NULL);
#endif

    (void)junk;
    return 0;
}
