#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../until.h"

#define ITEMS 10240
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)
#define SHARED_BYTES (ITEMS * LINE_SIZE)
#define FIXED_T3_VA 0x600000000ULL

#ifndef STRIDE_BYTES
#define STRIDE_BYTES 64
#endif

#ifndef TRAIN_STEP
#define TRAIN_STEP 6
#endif

#ifndef ROUNDS
#define ROUNDS 40000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef REPEAT
#define REPEAT 5
#endif

#ifndef CPU_ID
#define CPU_ID -1
#endif

#ifndef ENABLE_CPP_RCTX
#define ENABLE_CPP_RCTX 0
#endif

#ifndef PMU_WINDOW_NAME
#define PMU_WINDOW_NAME "train-trigger-per-round"
#endif

#ifndef PMU_CORE_X925
#define PMU_CORE_X925 ENABLE_CPP_RCTX
#endif

#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif

#include "../pmu.h"

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#define TEST_T0_NO_TRIGGER 0
#define TEST_T0 1
#define TEST_T1 2
#define TEST_T2 3
#define TEST_T3 4
#define TEST_T4 5
#define TEST_T5 6
#define TEST_T6 7

#ifndef SELECTED_TEST
#define SELECTED_TEST TEST_T0
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define TRIGGER_NONE 0
#define TRIGGER_NOINLINE 1
#define TRIGGER_INLINE 2

uint8_t array2[ITEMS * LINE_SIZE] __attribute__((aligned(4096)));
uint8_t array1[100 * LINE_SIZE] = {0};

static uint8_t *dummy_buffer;
static int pmu_ready;

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void read_exact(int fd, void *buffer, size_t size) {
    uint8_t *cursor = buffer;

    while (size > 0) {
        ssize_t got = read(fd, cursor, size);

        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            die("read");
        }
        cursor += got;
        size -= (size_t)got;
    }
}

static void write_exact(int fd, const void *buffer, size_t size) {
    const uint8_t *cursor = buffer;

    while (size > 0) {
        ssize_t written = write(fd, cursor, size);

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            die("write");
        }
        cursor += written;
        size -= (size_t)written;
    }
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

// static void dummy_accesses(void) {
//     dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
// }

void dummyAccesses(void){
    // printf("dummySize %d\n", DUMMY_BUFFER_SIZE);
  // dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
    for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
        // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[j]) : "memory", "w0");
    }
}

static inline void reset_prefetcher_context(void) {
#if ENABLE_CPP_RCTX
    cpp_rctx();
#endif
}

static int create_memfd_object(void) {
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "store_stride_index_exist", 0);
#else
    int fd = -1;
    errno = ENOSYS;
#endif

    if (fd < 0) {
        char name[128];

        snprintf(name, sizeof(name), "/store_stride_index_exist_%ld",
                 (long)getpid());
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            die("shm_open");
        }
        shm_unlink(name);
    }
    if (ftruncate(fd, SHARED_BYTES) != 0) {
        die("ftruncate");
    }
    return fd;
}

static uint8_t *map_shared_anywhere(int fd) {
    void *mapping = mmap(NULL, SHARED_BYTES, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, fd, 0);

    if (mapping == MAP_FAILED) {
        die("mmap shared");
    }
    return mapping;
}

static uint8_t *map_shared_at(uintptr_t addr, int fd) {
    void *mapping = mmap((void *)addr, SHARED_BYTES,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_POPULATE,
                         fd, 0);

    if (mapping == MAP_FAILED) {
        die("mmap fixed shared");
    }
    if ((uintptr_t)mapping != addr) {
        fprintf(stderr, "wrong fixed mapping: expected 0x%016lx got %p\n",
                (unsigned long)addr, mapping);
        exit(1);
    }
    return mapping;
}


static uint8_t *map_anon_at(uintptr_t addr) {
    void *mapping = mmap((void *)addr, SHARED_BYTES, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE |
                             MAP_POPULATE,
                         -1, 0);

    if (mapping == MAP_FAILED) {
        die("mmap fixed anon");
    }
    if ((uintptr_t)mapping != addr) {
        fprintf(stderr, "wrong fixed mapping: expected 0x%016lx got %p\n",
                (unsigned long)addr, mapping);
        exit(1);
    }
    return mapping;
}


static void flush_lines(uint8_t *base) {
    for (uint64_t offset = 0; offset < ITEMS * LINE_SIZE; offset += LINE_SIZE) {
        flush(base + offset);
    }
    // mfence();
}

static void warm_lines(uint8_t *base) {
    for (int i = 0; i < ITEMS; i++) {
        mLoad(base + (i * LINE_SIZE));
    }
}

static void delay_after_trigger(void) {
    volatile uint64_t dummy = 0;

    for (int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
    }
    for (int i = 0; i < 100; i++) {
        nop();
    }
    (void)dummy;
}

static uint64_t probe_latency(volatile uint8_t *probe_addr) {
    uint64_t time1,time2;
    volatile unsigned int junk;

    time1 = timestamp();
    junk = *probe_addr;
    return timestamp() - time1;
}

static void train_noinline(uint8_t *base, int stride) {
    for (int step = 0; step < TRAIN_STEP - 1; step++) {
#if TRAIN_ACCESS_LOAD
        mLoad_noinline(base + ((uint64_t)step * (uint64_t)stride));
#else
        mStore_noinline(base + ((uint64_t)step * (uint64_t)stride));
#endif
        nops();
    }
}

static void trigger_access(uint8_t *base, int stride, int trigger_mode) {
    uint8_t *trigger_addr = base + ((uint64_t)(TRAIN_STEP - 1) * (uint64_t)stride);

    if (trigger_mode == TRIGGER_NOINLINE) {
#if TRAIN_ACCESS_LOAD
        mLoad_noinline(trigger_addr);
#else
        mStore_noinline(trigger_addr);
#endif
        nops();
    } else if (trigger_mode == TRIGGER_INLINE) {
#if TRAIN_ACCESS_LOAD
        mLoad_inline(trigger_addr);
#else
        mStore_inline(trigger_addr);
#endif
        nops();
    }
}

static void wait_after_trigger_access(void) {
    nops();
    struct timespec prefetch_wait = {.tv_sec = 0, .tv_nsec = 100};
    nanosleep(&prefetch_wait, NULL);
}

static int start_train_trigger_pmu(void) {
    if (!pmu_ready) {
        return 0;
    }
    if (pmu_start() == 0) {
        return 1;
    }
    printf("# PMU unavailable: counter group could not be started\n");
    pmu_ready = 0;
    return 0;
}

static void stop_train_trigger_pmu(int pmu_running) {
    if (pmu_running) {
        pmu_stop_and_accumulate();
    }
}

static uint64_t run_case(uint8_t *train_base,
                         uint8_t *trigger_base,
                         uint8_t *flush_base_a,
                         uint8_t *flush_base_b,
                         int trigger_mode,
                         int stride) {
    uint64_t latency_sum[PROBE_POSITIONS] = {0};
    uint64_t probe_count[PROBE_POSITIONS] = {0};
    int predicted_position = (TRAIN_STEP * stride) / LINE_SIZE;

    if (predicted_position < 0 || predicted_position >= PROBE_POSITIONS) {
        fprintf(stderr, "predicted position exceeds PROBE_POSITIONS\n");
        exit(1);
    }

    for (uint64_t round = 0; round < ROUNDS; round++) {
        reset_prefetcher_context();
        int probe_pos;
        volatile uint8_t *probe_addr;

        // dummyAccesses();
        occupy_store_prefetcher_entries(dummy_buffer,
                                        DUMMY_BUFFER_SIZE / PAGE_SIZE, 6);

        flush_lines(flush_base_a);
        if (flush_base_b != NULL && flush_base_b != flush_base_a) {
            flush_lines(flush_base_b);
        }

        int pmu_running = start_train_trigger_pmu();
        train_noinline(train_base, stride);
        trigger_access(trigger_base, stride, trigger_mode);
        stop_train_trigger_pmu(pmu_running);

        // delay_after_trigger();
        wait_after_trigger_access();

        probe_pos = (round) % PROBE_POSITIONS;
        probe_addr = trigger_base + ((uint64_t)probe_pos * LINE_SIZE);
        latency_sum[probe_pos] += probe_latency(probe_addr);
        probe_count[probe_pos]++;
    }

    if (probe_count[predicted_position] == 0) {
        return 0;
    }
    return latency_sum[predicted_position] / probe_count[predicted_position];
}


static void t3_child_loop(uint8_t *base,
                          int to_parent,
                          int from_parent,
                          int trigger_mode,
                          int stride) {
    int predicted_position = (TRAIN_STEP * stride) / LINE_SIZE;
    uint8_t ready = 'R';

    if (predicted_position < 0 || predicted_position >= PROBE_POSITIONS) {
        fprintf(stderr, "predicted position exceeds PROBE_POSITIONS\n");
        exit(1);
    }

#if SELECTED_TEST == TEST_T3
    pmu_cleanup();
    printf("# PMU note: T3 PMU is split across parent-train and child-trigger windows\n");
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        printf("# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
    } else {
        pmu_reset_accumulated();
    }
#endif

    warm_lines(base);
    write_exact(to_parent, &ready, sizeof(ready));

    for (;;) {
        uint8_t cmd;
        int probe_pos;
        uint64_t latency;
        volatile uint8_t *probe_addr;

        read_exact(from_parent, &cmd, sizeof(cmd));
        if (cmd == 'Q') {
#if SELECTED_TEST == TEST_T3
            if (pmu_ready) {
                pmu_print_accumulated(ROUNDS);
            }
            pmu_cleanup();
#endif
            return;
        }
        if (cmd != 'T') {
            fprintf(stderr, "unexpected T3 child command: %u\n", cmd);
            exit(1);
        }

        flush_lines(base);
        int pmu_running = start_train_trigger_pmu();
        trigger_access(base, stride, trigger_mode);
        stop_train_trigger_pmu(pmu_running);
        // delay_after_trigger();
        wait_after_trigger_access();

        read_exact(from_parent, &probe_pos, sizeof(probe_pos));
        probe_addr = base + ((uint64_t)probe_pos * LINE_SIZE);
        latency = probe_latency(probe_addr);
        write_exact(to_parent, &latency, sizeof(latency));
    }
}

static uint64_t run_same_va_different_pa_case(uint8_t *parent_base,
                                              int trigger_mode,
                                              int stride) {
    int parent_to_child[2];
    int child_to_parent[2];
    pid_t pid;
    uint64_t latency_sum[PROBE_POSITIONS] = {0};
    uint64_t probe_count[PROBE_POSITIONS] = {0};
    int predicted_position = (TRAIN_STEP * stride) / LINE_SIZE;
    uint8_t ready;

    if (predicted_position < 0 || predicted_position >= PROBE_POSITIONS) {
        fprintf(stderr, "predicted position exceeds PROBE_POSITIONS\n");
        exit(1);
    }
    if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        die("pipe");
    }

    pid = fork();
    if (pid < 0) {
        die("fork");
    }
    if (pid == 0) {
        uint8_t *child_base;

        close(parent_to_child[1]);
        close(child_to_parent[0]);
        set_cpu_if_requested();

        if (munmap(parent_base, SHARED_BYTES) != 0) {
            die("munmap child inherited fixed VA");
        }
        child_base = map_anon_at(FIXED_T3_VA);
        t3_child_loop(child_base, child_to_parent[1], parent_to_child[0],
                      trigger_mode, stride);
        exit(0);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);
    read_exact(child_to_parent[0], &ready, sizeof(ready));
    if (ready != 'R') {
        fprintf(stderr, "T3 child failed to become ready\n");
        exit(1);
    }

    for (uint64_t round = 0; round < ROUNDS; round++) {
        reset_prefetcher_context();
        uint8_t cmd = 'T';
        int probe_pos = (int)((round * 73) % PROBE_POSITIONS);
        uint64_t latency;

        // dummyAccesses();
        occupy_store_prefetcher_entries(dummy_buffer,
                                        DUMMY_BUFFER_SIZE / PAGE_SIZE, 6);
        flush_lines(parent_base);
        int pmu_running = start_train_trigger_pmu();
        train_noinline(parent_base, stride);
        stop_train_trigger_pmu(pmu_running);

        write_exact(parent_to_child[1], &cmd, sizeof(cmd));
        write_exact(parent_to_child[1], &probe_pos, sizeof(probe_pos));
        read_exact(child_to_parent[0], &latency, sizeof(latency));

        latency_sum[probe_pos] += latency;
        probe_count[probe_pos]++;
    }

    if (probe_count[predicted_position] == 0) {
        fprintf(stderr, "T3 predicted position was not probed\n");
        exit(1);
    }

    {
        uint8_t cmd = 'Q';
        int status;

        write_exact(parent_to_child[1], &cmd, sizeof(cmd));
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        waitpid(pid, &status, 0);
    }

    return latency_sum[predicted_position] / probe_count[predicted_position];
}

static uint64_t run_same_process_remap_case(uint8_t *reserved_base,
                                            int trigger_mode,
                                            int stride) {
    uintptr_t fixed_va = (uintptr_t)reserved_base;
    int predicted_position = (TRAIN_STEP * stride) / LINE_SIZE;
    int train_fd;
    int trigger_fd;
    uint8_t *train_keeper;
    uint8_t *trigger_keeper;
    uint64_t latency_sum[PROBE_POSITIONS] = {0};
    uint64_t probe_count[PROBE_POSITIONS] = {0};

    if (predicted_position < 0 || predicted_position >= PROBE_POSITIONS) {
        fprintf(stderr, "T5 predicted position exceeds PROBE_POSITIONS\n");
        exit(1);
    }

    train_fd = create_memfd_object();
    trigger_fd = create_memfd_object();
    train_keeper = map_shared_anywhere(train_fd);
    trigger_keeper = map_shared_anywhere(trigger_fd);
    memset(train_keeper, -1, SHARED_BYTES);
    memset(trigger_keeper, -1, SHARED_BYTES);
    warm_lines(train_keeper);
    warm_lines(trigger_keeper);

    if (munmap(reserved_base, SHARED_BYTES) != 0) {
        die("munmap T5 reserved VA");
    }

    for (uint64_t round = 0; round < ROUNDS; round++) {
        reset_prefetcher_context();
        int probe_pos = (int)((round * 73) % PROBE_POSITIONS);
        uint8_t *active;
        volatile uint8_t *probe_addr;

        // dummyAccesses();
        occupy_store_prefetcher_entries(dummy_buffer,
                                        DUMMY_BUFFER_SIZE / PAGE_SIZE, 6);

        flush_lines(train_keeper);
        flush_lines(trigger_keeper);

        active = map_shared_at(fixed_va, train_fd);
        int pmu_running = start_train_trigger_pmu();
        train_noinline(active, stride);
        stop_train_trigger_pmu(pmu_running);
        if (munmap(active, SHARED_BYTES) != 0) {
            die("munmap T5 train mapping");
        }

        active = map_shared_at(fixed_va, trigger_fd);
        pmu_running = start_train_trigger_pmu();
        trigger_access(active, stride, trigger_mode);
        stop_train_trigger_pmu(pmu_running);
        // delay_after_trigger();

        wait_after_trigger_access();

        probe_addr = active + ((uint64_t)probe_pos * LINE_SIZE);
        latency_sum[probe_pos] += probe_latency(probe_addr);
        probe_count[probe_pos]++;
        if (munmap(active, SHARED_BYTES) != 0) {
            die("munmap T5 trigger mapping");
        }
    }

    munmap(trigger_keeper, SHARED_BYTES);
    munmap(train_keeper, SHARED_BYTES);
    close(trigger_fd);
    close(train_fd);

    if (probe_count[predicted_position] == 0) {
        fprintf(stderr, "T5 predicted position was not probed\n");
        exit(1);
    }
    return latency_sum[predicted_position] / probe_count[predicted_position];
}

static void print_result(const char *id,
                         const char *description,
                         const char *pc,
                         const char *va,
                         const char *pa,
                         uint64_t latency) {
    printf("%s\t%s\t%s\t%s\t%s\t%lu\n",
           id, description, pc, va, pa, (unsigned long)latency);
}

int main(void) {
    int stride = STRIDE_BYTES;
    uint64_t trigger_offset;
    uint64_t probe_offset;
    uint64_t latency = 0;
    int shared_fd;
    uint8_t *shared_a;
    uint8_t *shared_b;
    uint8_t *t3_parent_va;
    const char *test_id = NULL;
    const char *description = NULL;
    const char *pc = NULL;
    const char *va = NULL;
    const char *pa = NULL;

    if (TRAIN_STEP < 2) {
        fprintf(stderr, "TRAIN_STEP must be >= 2\n");
        return 1;
    }
    if (stride <= 0) {
        fprintf(stderr, "STRIDE_BYTES must be positive\n");
        return 1;
    }

    trigger_offset = (uint64_t)(TRAIN_STEP - 1) * (uint64_t)stride;
    probe_offset = (uint64_t)TRAIN_STEP * (uint64_t)stride;
    if (probe_offset >= ITEMS * LINE_SIZE) {
        fprintf(stderr, "probe offset exceeds array2 size\n");
        return 1;
    }

    set_cpu_if_requested();

    memset(array2, -1, sizeof(array2));
    t3_parent_va = map_anon_at(FIXED_T3_VA);
    memset(t3_parent_va, -1, SHARED_BYTES);

    shared_fd = create_memfd_object();
    shared_a = map_shared_anywhere(shared_fd);
    shared_b = map_shared_anywhere(shared_fd);
    memset(shared_a, -1, SHARED_BYTES);

    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        die("mmap dummy_buffer");
    }

    warm_lines(array2);
    warm_lines(shared_a);
    warm_lines(shared_b);
    warm_lines(t3_parent_va);
    flush_lines(array2);
    flush_lines(shared_a);
    flush_lines(shared_b);
    flush_lines(t3_parent_va);
    mfence();

    printf("# arm64 store-stride index-mode test based on test0-exist\n");
    printf("# SELECTED_TEST=%d\n", SELECTED_TEST);
    printf("# TRAIN_STEP=%d STRIDE_BYTES=%d ROUNDS=%d PROBE_POSITIONS=%d REPEAT=%d CPU_ID=%d ENABLE_CPP_RCTX=%d TRAIN_ACCESS_LOAD=%d\n",
           TRAIN_STEP, STRIDE_BYTES, ROUNDS, PROBE_POSITIONS, REPEAT, CPU_ID,
           ENABLE_CPP_RCTX, TRAIN_ACCESS_LOAD);
    printf("# train: m%s_noinline offsets 0..%d * stride\n",
#if TRAIN_ACCESS_LOAD
           "Load",
#else
           "Store",
#endif
           TRAIN_STEP - 2);
    printf("# trigger offset=%lu probe offset=%lu\n",
           (unsigned long)trigger_offset, (unsigned long)probe_offset);
    printf("# T0 uses same noinline access for train and trigger: same PC/VA/PA\n");
    printf("# T1 uses noinline train and inline trigger access: different PC, same VA/PA\n");
    printf("# T2 uses two mappings of one shared object: same PC, different VA, same PA\n");
    printf("# T2_train_va=%p T2_trigger_va=%p\n", shared_a, shared_b);
    printf("# T3 uses parent/child anonymous mappings at one fixed VA: same VA, different PA, cross-process\n");
    printf("# T3_va=%p\n", t3_parent_va);
    printf("# T4 uses two mappings of one shared object and an inline trigger: different PC/VA, same PA\n");
    printf("# T5 remaps one fixed VA between two resident memfd objects: same PC/VA, different PA, same process\n");
    printf("# T6 uses the same-process remap with an inline trigger: different PC, same VA, different PA\n");
    printf("id\tdescription\tPC\tVA\tPA\tavg_%s\n", TIMESTAMP_UNIT_NAME);

    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        printf("# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
    } else {
        pmu_reset_accumulated();
    }
    fflush(stdout);

    switch (SELECTED_TEST) {
    case TEST_T0_NO_TRIGGER:
        test_id = "T0_no_trigger";
        description = "same_all_without_trigger";
        pc = "same";
        va = "same";
        pa = "same";
        latency = run_case(array2, array2, array2, NULL,
                           TRIGGER_NONE, stride);
        break;
    case TEST_T0:
        test_id = "T0";
        description = "same_all_with_noinline_trigger";
        pc = "same";
        va = "same";
        pa = "same";
        latency = run_case(array2, array2, array2, NULL,
                           TRIGGER_NOINLINE, stride);
        break;
    case TEST_T1:
        test_id = "T1";
        description = "different_pc_same_va_pa";
        pc = "different";
        va = "same";
        pa = "same";
        latency = run_case(array2, array2, array2, NULL,
                           TRIGGER_INLINE, stride);
        break;
    case TEST_T2:
        test_id = "T2";
        description = "different_va_same_pa";
        pc = "same";
        va = "different";
        pa = "same";
        latency = run_case(shared_a, shared_b, shared_a, shared_b,
                           TRIGGER_NOINLINE, stride);
        break;
    case TEST_T3:
        test_id = "T3";
        description = "cross_process_same_va_different_pa";
        pc = "same";
        va = "same";
        pa = "different";
        latency = run_same_va_different_pa_case(t3_parent_va,
                                                TRIGGER_NOINLINE, stride);
        break;
    case TEST_T4:
        test_id = "T4";
        description = "different_pc_va_same_pa";
        pc = "different";
        va = "different";
        pa = "same";
        latency = run_case(shared_a, shared_b, shared_a, shared_b,
                           TRIGGER_INLINE, stride);
        break;
    case TEST_T5:
        test_id = "T5";
        description = "same_process_same_pc_va_different_pa";
        pc = "same";
        va = "same";
        pa = "different";
        latency = run_same_process_remap_case(t3_parent_va,
                                              TRIGGER_NOINLINE, stride);
        t3_parent_va = NULL;
        break;
    case TEST_T6:
        test_id = "T6";
        description = "same_process_different_pc_same_va_different_pa";
        pc = "different";
        va = "same";
        pa = "different";
        latency = run_same_process_remap_case(t3_parent_va,
                                              TRIGGER_INLINE, stride);
        t3_parent_va = NULL;
        break;
    default:
        fprintf(stderr, "unknown SELECTED_TEST=%d\n", SELECTED_TEST);
        pmu_cleanup();
        close(shared_fd);
        return 1;
    }

    print_result(test_id, description, pc, va, pa, latency);
    if (pmu_ready) {
        pmu_print_accumulated(ROUNDS);
    }
    pmu_cleanup();

    close(shared_fd);
    return 0;
}
