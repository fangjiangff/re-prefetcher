#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "until.h"

#define PAGE_LINES (PAGE_SIZE / LINE_SIZE)
#define SHM_NAME_SIZE 128

#ifndef ACCESS_LINE
#define ACCESS_LINE 24
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef CPU_ID
#define CPU_ID -1
#endif

#ifndef ACCESS_IS_LOAD
#define ACCESS_IS_LOAD 0
#endif

#ifndef USE_NOINLINE_ACCESS
#define USE_NOINLINE_ACCESS 1
#endif

#ifndef RELOAD_STEP
#define RELOAD_STEP 17
#endif

#ifndef WAIT_NOPS
#define WAIT_NOPS 1000
#endif

static uint8_t *shared_page;
static long long latency_sum[PAGE_LINES];
static int probe_count[PAGE_LINES];

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

static int create_shared_object(char *name, size_t name_size) {
    int fd;

    snprintf(name, name_size, "/single_shared_page_%ld_%ld",
             (long)getpid(), (long)timestamp());

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        die("shm_open");
    }
    if (ftruncate(fd, PAGE_SIZE) != 0) {
        die("ftruncate");
    }
    return fd;
}

static uint8_t *map_shared_page(int fd) {
    void *mapping = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, fd, 0);

    if (mapping == MAP_FAILED) {
        die("mmap shared page");
    }
    return (uint8_t *)mapping;
}

static void flush_shared_page_forward(void) {
    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        flush(shared_page + offset);
    }
    mfence();
}

static void access_target_line(void) {
    uint8_t *addr = shared_page + ((size_t)ACCESS_LINE * LINE_SIZE);

#if ACCESS_IS_LOAD
#if USE_NOINLINE_ACCESS
    mLoad_noinline(addr);
#else
    mLoad_inline(addr);
#endif
#else
#if USE_NOINLINE_ACCESS
    mStore_noinline(addr);
#else
    mStore_inline(addr);
#endif
#endif
}

static void wait_after_access(void) {
    for (int i = 0; i < WAIT_NOPS; i++) {
        nop();
    }
}

static void reload_one_line(int pos) {
    unsigned int junk = 0;
    volatile uint8_t *addr = shared_page + ((size_t)pos * LINE_SIZE);
    uint64_t time1 = timestamp();
    junk += *addr;
    uint64_t time2 = timestamp() - time1;

    latency_sum[pos] += (long long)time2;
    probe_count[pos]++;

    (void)junk;
}

int main(void) {
    char shm_name[SHM_NAME_SIZE];
    int shm_fd;

    if (ACCESS_LINE < 0 || ACCESS_LINE >= PAGE_LINES) {
        fprintf(stderr, "ACCESS_LINE must be in [0, %d]\n", PAGE_LINES - 1);
        return 1;
    }

    set_cpu_if_requested();

    shm_fd = create_shared_object(shm_name, sizeof(shm_name));
    shared_page = map_shared_page(shm_fd);
    memset(shared_page, 0xff, PAGE_SIZE);

    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        mLoad_inline(shared_page + offset);
    }
    flush_shared_page_forward();

    shm_unlink(shm_name);
    close(shm_fd);

    printf("# single shared-page access one-probe-per-round reload test\n");
    printf("# page=0x%016lx access_line=%d access=%s pc=%s rounds=%d reload_step=%d wait_nops=%d\n",
           (unsigned long)(uintptr_t)shared_page,
           ACCESS_LINE,
#if ACCESS_IS_LOAD
           "load",
#else
           "store",
#endif
#if USE_NOINLINE_ACCESS
           "noinline",
#else
           "inline",
#endif
           ROUNDS, RELOAD_STEP, WAIT_NOPS);
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");

    for (int round = 0; round < ROUNDS; round++) {
        int probe_pos = (round * RELOAD_STEP) % PAGE_LINES;

        flush_shared_page_forward();
        access_target_line();
        wait_after_access();
        reload_one_line(probe_pos);
    }

    for (int pos = 0; pos < PAGE_LINES; pos++) {
        long long avg_ns = 0;

        if (probe_count[pos] > 0) {
            avg_ns = latency_sum[pos] / probe_count[pos];
        }
        printf("%3d\t%12d\t%10lld\t%5d\n",
               pos, pos * LINE_SIZE, avg_ns, probe_count[pos]);
    }

    return 0;
}
