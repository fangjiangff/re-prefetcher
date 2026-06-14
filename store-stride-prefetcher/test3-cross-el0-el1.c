#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include "until.h"

/*
 * EL0/EL1 stride prefetcher state test.
 *
 * Store mode:
 *   EL0 stores line 0, 5, 10, 15, 20.
 *   EL1 read(/dev/zero, user_page + line 25, 1) writes one byte.
 *
 * Load mode:
 *   EL0 loads line 0, 5, 10, 15.
 *   EL1 write(/dev/null, user_page + line 20, 1) reads one byte.
 *
 * EL0:
 *   probe one line per round. With stride = 5 cache lines, line 30 is the
 *   expected next-line prediction if the store-stride state survives the
 *   EL0->EL1 transition and can be triggered by the kernel store.
 */

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

#ifndef SAME_EL0_TRIGGER
#define SAME_EL0_TRIGGER 0
#endif

#ifndef CONTEXT_SWITCH_ONLY
#define CONTEXT_SWITCH_ONLY 0
#endif

static uint8_t *user_page;
static uint8_t *dummy_buffer;
static uint8_t array1[100 * LINE_SIZE] = {0};

static long long latency_sum[PROBE_POSITIONS] = {0};
static int probe_count[PROBE_POSITIONS] = {0};

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

static void flush_user_page(void) {
    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        flush(user_page + offset);
    }
    mfence();
}

static void dummyAccesses(void) {
// #if TRAIN_ACCESS_LOAD
//     for (size_t i = 0; i < DUMMY_BUFFER_SIZE; i += LINE_SIZE) {
//         mStore_inline(dummy_buffer + i);
//     }
//     mfence();
// #else
    dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
// #endif
}

static inline __attribute__((always_inline)) void access_for_train(void *addr) {
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

static void train_in_el0(int stride_bytes) {
    for (int step = 0; step < TRAIN_ONLY_ACCESSES; step++) {
        access_for_train(user_page + ((size_t)step * (size_t)stride_bytes));
    }
}

#if !NO_TRIGGER
#if SAME_EL0_TRIGGER || CONTEXT_SWITCH_ONLY
static void trigger_in_el0(size_t trigger_offset) {
#if TRAIN_ACCESS_LOAD
    mLoad_noinline(user_page + trigger_offset);
#else
#if USE_NOINLINE_STORE
    mStore_noinline(user_page + trigger_offset);
#else
    mStore_inline(user_page + trigger_offset);
#endif
#endif
}
#endif

#if !SAME_EL0_TRIGGER || CONTEXT_SWITCH_ONLY
static void context_switch_in_el1(int trigger_fd) {
    ssize_t ret;

    do {
        ret =
#if TRAIN_ACCESS_LOAD
            write(trigger_fd, dummy_buffer, 1);
#else
            read(trigger_fd, dummy_buffer, 1);
#endif
    } while (ret < 0 && errno == EINTR);

    if (ret != 1) {
#if TRAIN_ACCESS_LOAD
        die("write /dev/null context switch");
#else
        die("read /dev/zero context switch");
#endif
    }
}
#endif

#if !SAME_EL0_TRIGGER && !CONTEXT_SWITCH_ONLY
static void trigger_in_el1(int trigger_fd, size_t trigger_offset) {
    uint8_t *trigger_addr =
        user_page + trigger_offset;
    ssize_t ret;

    do {
        ret =
#if TRAIN_ACCESS_LOAD
            write(trigger_fd, trigger_addr, 1);
#else
            read(trigger_fd, trigger_addr, 1);
#endif
    } while (ret < 0 && errno == EINTR);

    if (ret != 1) {
#if TRAIN_ACCESS_LOAD
        die("write /dev/null trigger");
#else
        die("read /dev/zero trigger");
#endif
    }
}
#endif
#endif

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

static void print_header(int stride_bytes, int trigger_line,
                         int predicted_line) {
    printf("# arm64 EL0/EL1 %s-stride retention test\n",
#if TRAIN_ACCESS_LOAD
           "load"
#else
           "store"
#endif
    );
    printf("# EL0 trains %d %ss, %s triggers access %d\n",
           TRAIN_ONLY_ACCESSES,
#if TRAIN_ACCESS_LOAD
           "load",
#else
           "store",
#endif
#if CONTEXT_SWITCH_ONLY
           "EL0_after_EL1_context_switch"
#elif SAME_EL0_TRIGGER
           "EL0"
#else
           "EL1"
#endif
           ,
           TRAIN_ACCESSES);
    printf("# user_page=0x%016lx\n", (unsigned long)(uintptr_t)user_page);
    printf("# stride_lines=%d stride_bytes=%d rounds=%d probe_positions=%d\n",
           STRIDE_LINES, stride_bytes, ROUNDS, PROBE_POSITIONS);
    printf("# trigger_line=%d predicted_line=%d access=%s pc=%s\n",
           trigger_line, predicted_line,
#if TRAIN_ACCESS_LOAD
           "load",
           "noinline_same_pc"
#else
           "store",
#if USE_NOINLINE_STORE
           "noinline_same_pc"
#else
           "inline_call_site_pc"
#endif
#endif
    );
    printf("# trigger=%s\n",
#if NO_TRIGGER
           "disabled"
#elif CONTEXT_SWITCH_ONLY
           "el1_context_switch_then_same_el0"
#elif SAME_EL0_TRIGGER
           "same_el0"
#elif TRAIN_ACCESS_LOAD
           "syscall_write_dev_null_from_user_page"
#else
           "syscall_read_dev_zero_to_user_page"
#endif
    );
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
}

int main(void) {
    int trigger_fd;
    int stride_bytes = STRIDE_LINES * LINE_SIZE;
    int trigger_line = TRIGGER_LINE_INDEX;
    int predicted_line = PREDICTED_LINE_INDEX;
    unsigned int junk = 0;
#if !NO_TRIGGER
    size_t trigger_offset =
        (size_t)TRAIN_ONLY_ACCESSES * (size_t)stride_bytes;
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

    user_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (user_page == MAP_FAILED) {
        die("mmap user_page");
    }
    memset(user_page, 0xff, PAGE_SIZE);

    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        die("mmap dummy_buffer");
    }

    trigger_fd =
#if TRAIN_ACCESS_LOAD
        open("/dev/null", O_WRONLY);
#else
        open("/dev/zero", O_RDONLY);
#endif
    if (trigger_fd < 0) {
#if TRAIN_ACCESS_LOAD
        die("open /dev/null");
#else
        die("open /dev/zero");
#endif
    }

    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        mLoad(user_page + offset);
    }

    print_header(stride_bytes, trigger_line, predicted_line);

    for (uint64_t round = 0; round < ROUNDS; round++) {
        int probe_pos = round % PROBE_POSITIONS;
        volatile uint8_t *probe_addr = user_page + (probe_pos * LINE_SIZE);
        uint64_t time1;
        uint64_t time2;

        flush_user_page();
        dummyAccesses();

        train_in_el0(stride_bytes);
#if !NO_TRIGGER
#if CONTEXT_SWITCH_ONLY
        context_switch_in_el1(trigger_fd);
        trigger_in_el0(trigger_offset);
#elif SAME_EL0_TRIGGER
        trigger_in_el0(trigger_offset);
#else
        trigger_in_el1(trigger_fd, trigger_offset);
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

    close(trigger_fd);
    (void)junk;
    return 0;
}
