#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "until.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PAGE_LINES (PAGE_SIZE / LINE_SIZE)
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)
#define SHM_NAME_SIZE 128

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

#ifndef SAME_PROCESS_TRIGGER
#define SAME_PROCESS_TRIGGER 0
#endif

#ifndef PROCESS_SWITCH_ONLY
#define PROCESS_SWITCH_ONLY 0
#endif

#ifndef CHILD_MAP_ADDR
#define CHILD_MAP_ADDR 0x700000000ULL
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

static void read_exact(int fd, void *buffer, size_t size) {
    uint8_t *cursor = buffer;

    while (size > 0) {
        ssize_t got = read(fd, cursor, size);

        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            die("read pipe");
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
            die("write pipe");
        }
        cursor += written;
        size -= (size_t)written;
    }
}

static uint8_t read_command(int fd) {
    uint8_t command;

    read_exact(fd, &command, sizeof(command));
    return command;
}

static void write_command(int fd, uint8_t command) {
    write_exact(fd, &command, sizeof(command));
}

static void *map_shared_page(int fd, uintptr_t requested_addr) {
    void *mapping = MAP_FAILED;

    if (requested_addr != 0) {
        mapping = mmap((void *)requested_addr, PAGE_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_POPULATE,
                       fd, 0);
    }

    if (mapping == MAP_FAILED) {
        mapping = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, fd, 0);
    }
    if (mapping == MAP_FAILED) {
        die("mmap shared page");
    }

    return mapping;
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

static void train_in_parent(int stride_bytes) {
    for (int step = 0; step < TRAIN_ONLY_ACCESSES; step++) {
        access_for_test(shared_page + ((size_t)(step) * (size_t)stride_bytes));
    }
}

#if !NO_TRIGGER && (SAME_PROCESS_TRIGGER || PROCESS_SWITCH_ONLY)
static void trigger_in_parent(size_t trigger_offset) {
    access_for_test(shared_page + trigger_offset);
}
#endif

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

static int parse_int_arg(const char *arg, const char *name) {
    char *end = NULL;
    long value = strtol(arg, &end, 10);

    if (*arg == '\0' || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, arg);
        exit(1);
    }
    return (int)value;
}

static int open_shared_object(const char *name) {
    int fd = shm_open(name, O_RDWR, 0600);

    if (fd < 0) {
        die("shm_open child");
    }
    return fd;
}

static int child_main(int argc, char **argv) {
    const char *shm_name;
    int start_fd;
    int done_fd;
    int shm_fd;
    uintptr_t mapped_addr;
    size_t trigger_offset;

    if (argc != 5) {
        fprintf(stderr, "child usage: %s --child SHM START_FD DONE_FD\n",
                argv[0]);
        return 1;
    }

    shm_name = argv[2];
    start_fd = parse_int_arg(argv[3], "start fd");
    done_fd = parse_int_arg(argv[4], "done fd");

    set_cpu_if_requested();

    shm_fd = open_shared_object(shm_name);
    shared_page = map_shared_page(shm_fd, (uintptr_t)CHILD_MAP_ADDR);
    close(shm_fd);

    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        mLoad(shared_page + offset);
    }

    mapped_addr = (uintptr_t)shared_page;
    write_exact(done_fd, &mapped_addr, sizeof(mapped_addr));

    trigger_offset =
        (size_t)TRAIN_ONLY_ACCESSES * (size_t)STRIDE_LINES * (size_t)LINE_SIZE;

    for (;;) {
        uint8_t command = read_command(start_fd);

        if (command == 'Q') {
            return 0;
        }
        if (command == 'S') {
            write_command(done_fd, 'A');
            continue;
        }
        if (command != 'T') {
            fprintf(stderr, "unexpected child command: %u\n", command);
            return 1;
        }

        access_for_test(shared_page + trigger_offset);
        write_command(done_fd, 'A');
    }
}

static int create_shared_object(char *name, size_t name_size) {
    int fd;

    snprintf(name, name_size, "/store_stride_cross_%ld_%ld",
             (long)getpid(), (long)timestamp());

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        die("shm_open parent");
    }
    if (ftruncate(fd, PAGE_SIZE) != 0) {
        die("ftruncate shm");
    }
    return fd;
}

static void print_header(int stride_bytes, int trigger_line,
                         int predicted_line, pid_t child_pid,
                         uintptr_t child_addr) {
    printf("# arm64 strong cross-process %s-stride retention test\n",
#if TRAIN_ACCESS_LOAD
           "load"
#else
           "store"
#endif
    );
    printf("# parent trains %d %ss, %s triggers access %d\n",
           TRAIN_ONLY_ACCESSES,
#if TRAIN_ACCESS_LOAD
           "load",
#else
           "store",
#endif
#if PROCESS_SWITCH_ONLY
           "parent_after_process_switch"
#elif SAME_PROCESS_TRIGGER
           "parent"
#else
           "exec child"
#endif
           ,
           TRAIN_ACCESSES);
    printf("# parent_pid=%ld child_pid=%ld\n",
           (long)getpid(), (long)child_pid);
    printf("# parent_page=0x%016lx child_page=0x%016lx same_va=%s\n",
           (unsigned long)(uintptr_t)shared_page,
           (unsigned long)child_addr,
           ((uintptr_t)shared_page == child_addr) ? "yes" : "no");
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
#elif PROCESS_SWITCH_ONLY
           "process_switch_then_same_process"
#elif SAME_PROCESS_TRIGGER
           "same_process"
#elif TRAIN_ACCESS_LOAD
           "exec_child_load"
#else
           "exec_child_store"
#endif
    );
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
}

int main(int argc, char **argv) {
    char shm_name[SHM_NAME_SIZE];
    int parent_to_child[2];
    int child_to_parent[2];
    int shm_fd;
    int stride_bytes = STRIDE_LINES * LINE_SIZE;
    int trigger_line = TRIGGER_LINE_INDEX;
    int predicted_line = PREDICTED_LINE_INDEX;
    uintptr_t child_addr;
    pid_t child;
    unsigned int junk = 0;
#if !NO_TRIGGER && (SAME_PROCESS_TRIGGER || PROCESS_SWITCH_ONLY)
    size_t trigger_offset =
        (size_t)TRAIN_ONLY_ACCESSES * (size_t)stride_bytes;
#endif

    if (argc > 1 && strcmp(argv[1], "--child") == 0) {
        return child_main(argc, argv);
    }

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

    shm_fd = create_shared_object(shm_name, sizeof(shm_name));
    shared_page = map_shared_page(shm_fd, 0);
    memset(shared_page, 0xff, PAGE_SIZE);

    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        die("mmap dummy_buffer");
    }

    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        mLoad(shared_page + offset);
    }

    if (!SAME_PROCESS_TRIGGER &&
        (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0)) {
        die("pipe");
    }

    child = -1;
    child_addr = 0;

    if (!SAME_PROCESS_TRIGGER) {
        child = fork();
        if (child < 0) {
            die("fork");
        }

    if (child == 0) {
        char start_fd[32];
        char done_fd[32];

        snprintf(start_fd, sizeof(start_fd), "%d", parent_to_child[0]);
        snprintf(done_fd, sizeof(done_fd), "%d", child_to_parent[1]);

        close(parent_to_child[1]);
        close(child_to_parent[0]);
        close(shm_fd);

        execl(argv[0], argv[0], "--child", shm_name, start_fd, done_fd,
              (char *)NULL);
        die("execl child");
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);
    close(shm_fd);

    read_exact(child_to_parent[0], &child_addr, sizeof(child_addr));
    } else {
        close(shm_fd);
        child_addr = (uintptr_t)shared_page;
    }
    shm_unlink(shm_name);

    print_header(stride_bytes, trigger_line, predicted_line, child, child_addr);

    for (uint64_t round = 0; round < ROUNDS; round++) {
        // int probe_pos = round % PROBE_POSITIONS;
        int probe_pos = (round * 73) % PROBE_POSITIONS;
        volatile uint8_t *probe_addr = shared_page + (probe_pos * LINE_SIZE);
        uint64_t time1;
        uint64_t time2;

        flush_shared_page();
        dummyAccesses();

        train_in_parent(stride_bytes);
#if !NO_TRIGGER
#if SAME_PROCESS_TRIGGER || PROCESS_SWITCH_ONLY
#if PROCESS_SWITCH_ONLY
        write_command(parent_to_child[1], 'S');
        if (read_command(child_to_parent[0]) != 'A') {
            fprintf(stderr, "child did not acknowledge switch\n");
            break;
        }
#endif
        trigger_in_parent(trigger_offset);
#else
        write_command(parent_to_child[1], 'T');
        if (read_command(child_to_parent[0]) != 'A') {
            fprintf(stderr, "child did not acknowledge trigger\n");
            break;
        }
#endif
#endif

        // delay_after_trigger();
        for (int i = 0; i < 1000; i++) {
            nop();
        }


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

    if (!SAME_PROCESS_TRIGGER) {
        write_command(parent_to_child[1], 'Q');
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        waitpid(child, NULL, 0);
    }

    (void)junk;
    return 0;
}
