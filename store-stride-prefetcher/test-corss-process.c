#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "until.h"

/*
 * Cross-process store-stride state test.
 *
 * Parent process:
 *   store line 0, 5, 10, 15, 20
 *
 * Child process:
 *   store line 25 as the 6th access / trigger
 *
 * Parent process:
 *   probe one line per round. With stride = 5 cache lines, line 30 is the
 *   expected next-line prediction if the store-stride state survives the
 *   process context switch.
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

static uint8_t array1[100 * LINE_SIZE] = {0};

static uint8_t *shared_page;
static uint8_t *dummy_buffer;

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

static void checked_read_byte(int fd) {
    uint8_t value;
    ssize_t got;

    do {
        got = read(fd, &value, 1);
    } while (got < 0 && errno == EINTR);

    if (got != 1) {
        die("read pipe");
    }
}

static void checked_write_byte(int fd) {
    uint8_t value = 1;
    ssize_t written;

    do {
        written = write(fd, &value, 1);
    } while (written < 0 && errno == EINTR);

    if (written != 1) {
        die("write pipe");
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

static inline __attribute__((always_inline)) void store_for_test(void *addr) {
#if USE_NOINLINE_STORE
    mStore_noinline(addr);
#else
    mStore_inline(addr);
#endif
}

static void train_in_parent(int stride_bytes) {
    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        store_for_test(shared_page + ((size_t)step * (size_t)stride_bytes));
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

static void print_header(int stride_bytes, int trigger_line,
                         int predicted_line) {
    printf("# arm64 cross-process store-stride retention test\n");
    printf("# parent trains %d stores, child triggers the 6th store\n",
           TRAIN_ACCESSES);
    printf("# stride_lines=%d stride_bytes=%d rounds=%d probe_positions=%d\n",
           STRIDE_LINES, stride_bytes, ROUNDS, PROBE_POSITIONS);
    printf("# trigger_line=%d predicted_line=%d store_pc=%s\n",
           trigger_line, predicted_line,
#if USE_NOINLINE_STORE
           "noinline_same_pc"
#else
           "inline_call_site_pc"
#endif
    );
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
}

static void child_loop(int start_fd, int done_fd, int stride_bytes) {
    size_t trigger_offset =
        (size_t)TRAIN_ACCESSES * (size_t)stride_bytes;

    set_cpu_if_requested();

    for (;;) {
        checked_read_byte(start_fd);
        store_for_test(shared_page + trigger_offset);
        checked_write_byte(done_fd);
    }
}

int main(void) {
    int parent_to_child[2];
    int child_to_parent[2];
    int stride_bytes = STRIDE_LINES * LINE_SIZE;
    int trigger_line = TRAIN_ACCESSES * STRIDE_LINES;
    int predicted_line = (TRAIN_ACCESSES + 1) * STRIDE_LINES;
    unsigned int junk = 0;

    if (stride_bytes <= 0 ||
        (size_t)(TRAIN_ACCESSES + 1) * (size_t)stride_bytes >= PAGE_SIZE) {
        fprintf(stderr, "training/trigger/predicted lines must fit in one page\n");
        return 1;
    }
    if (PROBE_POSITIONS > PAGE_LINES) {
        fprintf(stderr, "PROBE_POSITIONS must be <= %d\n", PAGE_LINES);
        return 1;
    }

    set_cpu_if_requested();

    shared_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
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

    if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        die("pipe");
    }

    pid_t child = fork();
    if (child < 0) {
        die("fork");
    }

    if (child == 0) {
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        child_loop(parent_to_child[0], child_to_parent[1], stride_bytes);
        return 0;
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);

    print_header(stride_bytes, trigger_line, predicted_line);

    for (uint64_t round = 0; round < ROUNDS; round++) {
        int probe_pos = round % PROBE_POSITIONS;
        volatile uint8_t *probe_addr = shared_page + (probe_pos * LINE_SIZE);
        uint64_t time1;
        uint64_t time2;

        flush_shared_page();
        dummyAccesses();

        train_in_parent(stride_bytes);
        checked_write_byte(parent_to_child[1]);
        checked_read_byte(child_to_parent[0]);

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

    close(parent_to_child[1]);
    close(child_to_parent[0]);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    (void)junk;
    return 0;
}
