#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "until.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 5
#endif

#ifndef ROUNDS
#define ROUNDS 2000
#endif

#ifndef CPU_ID
#define CPU_ID -1
#endif

#ifndef TRAIN_PC
#define TRAIN_PC 0x500000120ULL
#endif

#ifndef TRIGGER_PC
#define TRIGGER_PC 0x7000009a0ULL
#endif

#define TEST_PAGES 2
#define DATA_BYTES (TEST_PAGES * PAGE_SIZE)
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 16)
#define FIXED_DATA_VA 0x600000000ULL
#define FIXED_CHILD_VA 0x610000000ULL
#define ALIAS_REQUEST_VA 0x620000000ULL
#define MAX_GADGET_PAGES 32

typedef void (*store_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *dummy_buffer;
static uintptr_t gadget_pages[MAX_GADGET_PAGES];
static int gadget_page_count;
static size_t page_size;

extern char _store_gadget_asm_start[];
extern char _store_gadget_asm_end[];

asm(
    ".global _store_gadget_asm_start\n"
    ".global _store_gadget_asm_end\n"
    "_store_gadget_asm_start:\n"
    "    strb w0, [x0]\n"
    "    ret\n"
    "_store_gadget_asm_end:\n"
    "    nop\n"
);

struct cross_child {
    pid_t pid;
    int to_child;
    int from_child;
    const char *name;
};

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

static uintptr_t page_base(uintptr_t address) {
    return address - (address % page_size);
}

static int gadget_page_is_mapped(uintptr_t page) {
    for (int i = 0; i < gadget_page_count; i++) {
        if (gadget_pages[i] == page) {
            return 1;
        }
    }
    return 0;
}

static int ensure_gadget_page(uintptr_t page) {
    void *mapping;

    if (gadget_page_is_mapped(page)) {
        return 0;
    }
    if (gadget_page_count >= MAX_GADGET_PAGES) {
        fprintf(stderr, "too many gadget pages\n");
        return -1;
    }

    mapping = mmap((void *)page, page_size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE |
                       MAP_POPULATE,
                   -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap gadget page 0x%016lx failed: %s\n",
                (unsigned long)page, strerror(errno));
        return -1;
    }
    if ((uintptr_t)mapping != page) {
        fprintf(stderr, "wrong gadget mapping: expected 0x%016lx got %p\n",
                (unsigned long)page, mapping);
        return -1;
    }

    gadget_pages[gadget_page_count++] = page;
    return 0;
}

static store_gadget_f map_store_gadget(uintptr_t address) {
    uintptr_t page = page_base(address);
    size_t page_offset = address - page;
    size_t gadget_size =
        (size_t)(_store_gadget_asm_end - _store_gadget_asm_start);

    if (page_offset + gadget_size > page_size) {
        fprintf(stderr, "store gadget crosses page boundary: 0x%016lx\n",
                (unsigned long)address);
        return NULL;
    }
    if (ensure_gadget_page(page) != 0) {
        return NULL;
    }

    memcpy((void *)address, _store_gadget_asm_start, gadget_size);
    __builtin___clear_cache((char *)address,
                            (char *)(address + gadget_size));
    return (store_gadget_f)(void *)address;
}

static void *map_anon_at(uintptr_t addr) {
    void *mapping = mmap((void *)addr, DATA_BYTES, PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE |
                             MAP_POPULATE,
                         -1, 0);

    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap anon at 0x%016lx failed: %s\n",
                (unsigned long)addr, strerror(errno));
        return MAP_FAILED;
    }
    return mapping;
}

static void *map_anon_anywhere(void) {
    void *mapping = mmap(NULL, DATA_BYTES, PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);

    if (mapping == MAP_FAILED) {
        die("mmap anon");
    }
    return mapping;
}

static void *map_shared_at(int fd, uintptr_t addr) {
    void *mapping = mmap((void *)addr, DATA_BYTES, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_POPULATE,
                         fd, 0);

    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap shared at 0x%016lx failed: %s\n",
                (unsigned long)addr, strerror(errno));
        return MAP_FAILED;
    }
    return mapping;
}

static void *map_shared_anywhere(int fd) {
    void *mapping = mmap(NULL, DATA_BYTES, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, fd, 0);

    if (mapping == MAP_FAILED) {
        die("mmap shared");
    }
    return mapping;
}

static int create_memfd_object(void) {
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "store_stride_index", 0);
#else
    int fd = -1;
    errno = ENOSYS;
#endif

    if (fd < 0) {
        char name[128];

        snprintf(name, sizeof(name), "/store_stride_index_%ld_%ld",
                 (long)getpid(), (long)timestamp());
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            die("shm_open");
        }
        shm_unlink(name);
    }
    if (ftruncate(fd, DATA_BYTES) != 0) {
        die("ftruncate");
    }
    return fd;
}

static void touch_lines(uint8_t *base) {
    for (size_t off = 0; off < DATA_BYTES; off += LINE_SIZE) {
        mStore_inline(base + off);
    }
}

static void flush_lines(uint8_t *base) {
    for (size_t off = 0; off < DATA_BYTES; off += LINE_SIZE) {
        flush(base + off);
    }
    mfence();
}

static void dummy_accesses(void) {
    dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
}

static void delay_after_trigger(void) {
    for (int k = 0; k < 100; k++) {
        maccess(&array1[k * LINE_SIZE]);
    }
    for (int i = 0; i < 1000; i++) {
        nop();
    }
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t t1 = timestamp();
    maccess(addr);
    return timestamp() - t1;
}

static void train_stream(store_gadget_f train_store,
                         uint8_t *train_base,
                         size_t stride_bytes) {
    for (int i = 0; i < TRAIN_ACCESSES; i++) {
        train_store(train_base + ((size_t)i * stride_bytes));
    }
}

static uint64_t same_process_round(store_gadget_f train_store,
                                   store_gadget_f trigger_store,
                                   uint8_t *train_base,
                                   uint8_t *trigger_base,
                                   uint8_t *flush_base_a,
                                   uint8_t *flush_base_b,
                                   int do_trigger,
                                   size_t stride_bytes) {
    size_t trigger_offset = (size_t)TRAIN_ACCESSES * stride_bytes;
    size_t probe_offset = (size_t)(TRAIN_ACCESSES + 1) * stride_bytes;

    flush_lines(flush_base_a);
    if (flush_base_b != NULL && flush_base_b != flush_base_a) {
        flush_lines(flush_base_b);
    }
    dummy_accesses();

    train_stream(train_store, train_base, stride_bytes);
    if (do_trigger) {
        trigger_store(trigger_base + trigger_offset);
    }
    delay_after_trigger();

    return probe_latency(trigger_base + probe_offset);
}

static uint64_t run_same_process_case(store_gadget_f train_store,
                                      store_gadget_f trigger_store,
                                      uint8_t *train_base,
                                      uint8_t *trigger_base,
                                      uint8_t *flush_base_a,
                                      uint8_t *flush_base_b,
                                      int do_trigger,
                                      size_t stride_bytes) {
    uint64_t sum = 0;

    for (int round = 0; round < ROUNDS; round++) {
        sum += same_process_round(train_store, trigger_store, train_base,
                                  trigger_base, flush_base_a, flush_base_b,
                                  do_trigger, stride_bytes);
    }
    return sum / (uint64_t)ROUNDS;
}

static void child_loop(uint8_t *child_base,
                       store_gadget_f child_trigger_store,
                       int to_parent,
                       int from_parent) {
    size_t stride_bytes = STRIDE_LINES * LINE_SIZE;
    size_t trigger_offset = (size_t)TRAIN_ACCESSES * stride_bytes;
    size_t probe_offset = (size_t)(TRAIN_ACCESSES + 1) * stride_bytes;
    uint8_t ready = 'R';

    touch_lines(child_base);
    write_exact(to_parent, &ready, sizeof(ready));

    for (;;) {
        uint8_t cmd;
        uint64_t latency;

        read_exact(from_parent, &cmd, sizeof(cmd));
        if (cmd == 'Q') {
            return;
        }
        if (cmd == 'P') {
            flush_lines(child_base);
            dummy_accesses();
            cmd = 'A';
            write_exact(to_parent, &cmd, sizeof(cmd));
            continue;
        }
        if (cmd != 'T') {
            fprintf(stderr, "unexpected child command: %u\n", cmd);
            exit(1);
        }

        child_trigger_store(child_base + trigger_offset);
        delay_after_trigger();
        latency = probe_latency(child_base + probe_offset);
        write_exact(to_parent, &latency, sizeof(latency));
    }
}

static struct cross_child spawn_child(const char *name,
                                      int shared_fd,
                                      int use_shared,
                                      uintptr_t fixed_va,
                                      uintptr_t inherited_va,
                                      store_gadget_f child_trigger_store) {
    int parent_to_child[2];
    int child_to_parent[2];
    pid_t pid;
    struct cross_child child;

    if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        die("pipe");
    }

    pid = fork();
    if (pid < 0) {
        die("fork");
    }
    if (pid == 0) {
        void *mapping;

        close(parent_to_child[1]);
        close(child_to_parent[0]);
        set_cpu_if_requested();

        if (inherited_va != 0) {
            munmap((void *)inherited_va, DATA_BYTES);
        }

        if (use_shared) {
            mapping = map_shared_at(shared_fd, fixed_va);
        } else {
            mapping = map_anon_at(fixed_va);
        }
        if (mapping == MAP_FAILED) {
            exit(1);
        }

        child_loop((uint8_t *)mapping, child_trigger_store,
                   child_to_parent[1], parent_to_child[0]);
        exit(0);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);

    child.pid = pid;
    child.to_child = parent_to_child[1];
    child.from_child = child_to_parent[0];
    child.name = name;
    return child;
}

static void wait_child_ready(struct cross_child *child) {
    uint8_t ready;

    read_exact(child->from_child, &ready, sizeof(ready));
    if (ready != 'R') {
        fprintf(stderr, "%s child failed to become ready\n", child->name);
        exit(1);
    }
}

static uint64_t cross_process_round(struct cross_child *child,
                                    store_gadget_f train_store,
                                    uint8_t *train_base,
                                    uint8_t *flush_base,
                                    size_t stride_bytes) {
    uint8_t cmd = 'P';
    uint8_t ack;
    uint64_t latency;

    write_exact(child->to_child, &cmd, sizeof(cmd));
    read_exact(child->from_child, &ack, sizeof(ack));
    if (ack != 'A') {
        fprintf(stderr, "%s child failed to prepare\n", child->name);
        exit(1);
    }

    flush_lines(flush_base);
    dummy_accesses();
    train_stream(train_store, train_base, stride_bytes);

    cmd = 'T';
    write_exact(child->to_child, &cmd, sizeof(cmd));
    read_exact(child->from_child, &latency, sizeof(latency));
    return latency;
}

static uint64_t run_cross_process_case(struct cross_child *child,
                                       store_gadget_f train_store,
                                       uint8_t *train_base,
                                       uint8_t *flush_base,
                                       size_t stride_bytes) {
    uint64_t sum = 0;

    for (int round = 0; round < ROUNDS; round++) {
        sum += cross_process_round(child, train_store, train_base, flush_base,
                                   stride_bytes);
    }
    return sum / (uint64_t)ROUNDS;
}

static void stop_child(struct cross_child *child) {
    uint8_t cmd = 'Q';
    int status;

    write_exact(child->to_child, &cmd, sizeof(cmd));
    close(child->to_child);
    close(child->from_child);
    waitpid(child->pid, &status, 0);
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

static int virtual_to_physical(void *addr, uint64_t *pa_out) {
    uint64_t entry;
    uint64_t pfn;
    off_t offset;
    int fd;
    ssize_t got;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    offset = (off_t)(((uintptr_t)addr / page_size) * sizeof(entry));
    got = pread(fd, &entry, sizeof(entry), offset);
    close(fd);

    if (got != (ssize_t)sizeof(entry)) {
        return -1;
    }
    if ((entry & (1ULL << 63)) == 0) {
        return -1;
    }

    pfn = entry & ((1ULL << 55) - 1);
    if (pfn == 0) {
        return -1;
    }

    *pa_out = (pfn * (uint64_t)page_size) +
              ((uintptr_t)addr % (uint64_t)page_size);
    return 0;
}

static void print_pa_or_unavailable(const char *label, void *addr) {
    uint64_t pa;

    if (virtual_to_physical(addr, &pa) == 0) {
        printf("# %s=0x%016lx\n", label, (unsigned long)pa);
    } else {
        printf("# %s=unavailable\n", label);
    }
}

int main(void) {
    size_t stride_bytes = STRIDE_LINES * LINE_SIZE;
    size_t probe_offset = (size_t)(TRAIN_ACCESSES + 1) * stride_bytes;
    store_gadget_f train_store;
    store_gadget_f trigger_store;
    uint8_t *private_a;
    uint8_t *private_b;
    uint8_t *shared_a;
    uint8_t *shared_b;
    uint8_t *fixed_private;
    uint8_t *fixed_shared;
    int shared_fd;
    struct cross_child t2_child;
    struct cross_child t4_child;
    uint64_t no_trigger;
    uint64_t same_all;
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    uint64_t t4;

    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect page size\n");
        return 1;
    }
    page_size = (size_t)detected_page_size;

    if (page_size != PAGE_SIZE) {
        fprintf(stderr, "compile-time PAGE_SIZE=%d runtime page_size=%lu\n",
                PAGE_SIZE, (unsigned long)page_size);
        return 1;
    }
    if (probe_offset + LINE_SIZE > DATA_BYTES) {
        fprintf(stderr,
                "probe offset does not fit: train=%d stride_lines=%d data=%d\n",
                TRAIN_ACCESSES, STRIDE_LINES, DATA_BYTES);
        return 1;
    }

    set_cpu_if_requested();

    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        die("mmap dummy_buffer");
    }

    train_store = map_store_gadget(TRAIN_PC);
    trigger_store = map_store_gadget(TRIGGER_PC);
    if (train_store == NULL || trigger_store == NULL) {
        return 1;
    }

    private_a = map_anon_anywhere();
    private_b = map_anon_anywhere();
    shared_fd = create_memfd_object();
    shared_a = map_shared_anywhere(shared_fd);
    shared_b = map_shared_at(shared_fd, ALIAS_REQUEST_VA);
    fixed_private = map_anon_at(FIXED_DATA_VA);
    fixed_shared = map_shared_at(shared_fd, FIXED_CHILD_VA);
    if (shared_b == MAP_FAILED ||
        fixed_private == MAP_FAILED ||
        fixed_shared == MAP_FAILED) {
        return 1;
    }

    touch_lines(private_a);
    touch_lines(private_b);
    touch_lines(shared_a);
    touch_lines(shared_b);
    touch_lines(fixed_private);
    touch_lines(fixed_shared);

    t2_child = spawn_child("T2", -1, 0, FIXED_DATA_VA,
                           FIXED_DATA_VA, trigger_store);
    t4_child = spawn_child("T4", shared_fd, 1, FIXED_CHILD_VA,
                           FIXED_CHILD_VA, trigger_store);
    wait_child_ready(&t2_child);
    wait_child_ready(&t4_child);

    no_trigger = run_same_process_case(train_store, train_store,
                                       private_a, private_a,
                                       private_a, NULL, 0, stride_bytes);
    same_all = run_same_process_case(train_store, train_store,
                                     private_a, private_a,
                                     private_a, NULL, 1, stride_bytes);

    t1 = run_same_process_case(train_store, train_store,
                               private_a, private_b,
                               private_a, private_b, 1, stride_bytes);

    t2 = run_cross_process_case(&t2_child, train_store, fixed_private,
                                fixed_private, stride_bytes);

    t3 = run_same_process_case(train_store, trigger_store,
                               shared_a, shared_b,
                               shared_a, NULL, 1, stride_bytes);

    t4 = run_cross_process_case(&t4_child, train_store, fixed_shared,
                                fixed_shared, stride_bytes);

    printf("# arm64 store-stride index-mode test\n");
    printf("# TRAIN_ACCESSES=%d STRIDE_LINES=%d ROUNDS=%d CPU_ID=%d\n",
           TRAIN_ACCESSES, STRIDE_LINES, ROUNDS, CPU_ID);
    printf("# train_pc=0x%016lx trigger_pc=0x%016lx\n",
           (unsigned long)TRAIN_PC, (unsigned long)TRIGGER_PC);
    printf("# train stores: offsets 0..%d * stride; trigger offset=%lu; probe offset=%lu\n",
           TRAIN_ACCESSES - 1,
           (unsigned long)((size_t)TRAIN_ACCESSES * stride_bytes),
           (unsigned long)probe_offset);
    printf("# T1_train_pc=0x%016lx\n", (unsigned long)TRAIN_PC);
    printf("# T1_trigger_pc=0x%016lx\n", (unsigned long)TRAIN_PC);
    printf("# T2_parent_va=0x%016lx\n", (unsigned long)(uintptr_t)fixed_private);
    printf("# T2_child_va=0x%016lx\n", (unsigned long)(uintptr_t)FIXED_DATA_VA);
    printf("# T3_train_va=0x%016lx\n", (unsigned long)(uintptr_t)shared_a);
    printf("# T3_trigger_va=0x%016lx\n", (unsigned long)(uintptr_t)shared_b);
    printf("# T3_shared_offset=0\n");
    print_pa_or_unavailable("T3_train_pa", shared_a);
    print_pa_or_unavailable("T3_trigger_pa", shared_b);
    printf("# T0_no_trigger must be slow and T0_trigger must be fast.\n");
    printf("# Lower latency close to T0_trigger means this index hypothesis is more likely.\n");
    printf("id\tdescription\tPC\tVA\tPA\tavg_ns\n");

    print_result("T0_no_trigger", "same_all_without_trigger", "same", "same",
                 "same", no_trigger);
    print_result("T0_trigger", "same_all_with_trigger", "same", "same",
                 "same", same_all);
    print_result("T1", "PC_index", "same", "different", "different", t1);
    print_result("T2", "VA_index", "different", "same", "different", t2);
    print_result("T3", "PA_index", "different", "different", "same", t3);
    print_result("T4", "VA_PA_index", "different", "same", "same", t4);

    stop_child(&t2_child);
    stop_child(&t4_child);
    close(shared_fd);

    return 0;
}
