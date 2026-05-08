#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "test-entry.c expects AArch64 PRFM/DC CIVAC instructions."
#endif

#define LINE_SIZE 64
#define DEFAULT_PREFETCH_PC 0x500000120ull
#define DEFAULT_TARGET_BUFFER_ADDR 0x600000000ull
#define DEFAULT_MAX_COMPETITORS 128
#define DEFAULT_ROUNDS 1000
#define DEFAULT_STRIDE (10 * LINE_SIZE)
#define DEFAULT_TARGET_TRAIN_STEP 8
#define PROBE_EXTRA_AFTER_TRAIN_STEP 15
#define PC_SPACING 0x20ull
#define DUMMY_BUFFER_PAGES 10
#define COMPETITOR_REGION_PAGES 4
#define MAX_GADGET_PAGES 512

typedef void (*prefetch_gadget_f)(void *);
typedef void (*load_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *target_buffer;
static uint8_t *competitor_buffer;
static uint8_t *dummy_buffer;
static size_t page_size;
static size_t target_buffer_size;
static size_t competitor_region_size;
static size_t dummy_buffer_size;
static uintptr_t mapped_gadget_pages[MAX_GADGET_PAGES];
static int mapped_gadget_page_count;

extern char _prefetch_gadget_asm_start[];
extern char _prefetch_gadget_asm_end[];
extern char _load_gadget_asm_start[];
extern char _load_gadget_asm_end[];

asm(
    ".global _prefetch_gadget_asm_start\n"
    ".global _prefetch_gadget_asm_end\n"
    ".global _load_gadget_asm_start\n"
    ".global _load_gadget_asm_end\n"
    "_prefetch_gadget_asm_start:\n"
    "    prfm pldl1keep, [x0]\n"
    "    ret\n"
    "_prefetch_gadget_asm_end:\n"
    "    nop\n"
    "_load_gadget_asm_start:\n"
    "    ldrb w0, [x0]\n"
    "    ret\n"
    "_load_gadget_asm_end:\n"
    "    nop\n"
);

static inline void mfence(void) {
    asm volatile("DSB SY\nISB" ::: "memory");
}

static inline void nop(void) {
    asm volatile("nop");
}

static inline void flush(void *addr) {
    asm volatile("DC CIVAC, %0" :: "r"(addr) : "memory");
}

static inline void maccess(void *addr) {
    asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0");
}

static inline uint64_t timestamp_ns(void) {
    struct timespec t;
    asm volatile("DSB SY\nISB" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t);
    asm volatile("ISB\nDSB SY" ::: "memory");
    return t.tv_sec * 1000 * 1000 * 1000ULL + t.tv_nsec;
}

static uintptr_t page_base(uintptr_t address) {
    return address - (address % page_size);
}

static int gadget_page_is_mapped(uintptr_t page) {
    for (int i = 0; i < mapped_gadget_page_count; i++) {
        if (mapped_gadget_pages[i] == page) {
            return 1;
        }
    }
    return 0;
}

static int ensure_gadget_page(uintptr_t page) {
    if (gadget_page_is_mapped(page)) {
        return 0;
    }
    if (mapped_gadget_page_count >= MAX_GADGET_PAGES) {
        fprintf(stderr, "too many gadget pages\n");
        return -1;
    }

    void *mapping = mmap((void *)page, page_size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE,
                         -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap gadget page 0x%016lx failed: %s\n",
                (unsigned long)page, strerror(errno));
        return -1;
    }
    if ((uintptr_t)mapping != page) {
        fprintf(stderr, "mmap returned wrong gadget page: expected 0x%016lx got %p\n",
                (unsigned long)page, mapping);
        return -1;
    }

    mapped_gadget_pages[mapped_gadget_page_count++] = page;
    return 0;
}

static prefetch_gadget_f map_prefetch_gadget(uintptr_t address) {
    uintptr_t page = page_base(address);
    size_t page_offset = address - page;
    size_t gadget_size = (size_t)(_prefetch_gadget_asm_end - _prefetch_gadget_asm_start);

    if (page_offset + gadget_size > page_size) {
        fprintf(stderr, "gadget at 0x%016lx crosses a page boundary\n",
                (unsigned long)address);
        return NULL;
    }
    if (ensure_gadget_page(page) != 0) {
        return NULL;
    }

    memcpy((void *)address, _prefetch_gadget_asm_start, gadget_size);
    __builtin___clear_cache((char *)address, (char *)(address + gadget_size));
    return (prefetch_gadget_f)(void *)address;
}

static load_gadget_f map_load_gadget(uintptr_t address) {
    uintptr_t page = page_base(address);
    size_t page_offset = address - page;
    size_t gadget_size = (size_t)(_load_gadget_asm_end - _load_gadget_asm_start);

    if (page_offset + gadget_size > page_size) {
        fprintf(stderr, "load gadget at 0x%016lx crosses a page boundary\n",
                (unsigned long)address);
        return NULL;
    }
    if (ensure_gadget_page(page) != 0) {
        return NULL;
    }

    memcpy((void *)address, _load_gadget_asm_start, gadget_size);
    __builtin___clear_cache((char *)address, (char *)(address + gadget_size));
    return (load_gadget_f)(void *)address;
}

static uint8_t *map_target_buffer(uintptr_t address, size_t size) {
    uintptr_t base = page_base(address);
    uintptr_t end = address + size;
    size_t map_size = end - base;
    if (map_size % page_size) {
        map_size += page_size - (map_size % page_size);
    }

    void *mapping = mmap((void *)base, map_size,
                         PROT_READ | PROT_WRITE,
                         MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE,
                         -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap target buffer at 0x%016lx failed: %s\n",
                (unsigned long)base, strerror(errno));
        return NULL;
    }
    return (uint8_t *)address;
}

static void flush_target_buffer(void) {
    for (uint64_t offset = 0; offset < target_buffer_size; offset += LINE_SIZE) {
        flush(&target_buffer[offset]);
    }
    mfence();
}

static void dummy_accesses(void) {
    for (uint64_t i = 0; i < dummy_buffer_size; i += LINE_SIZE) {
        maccess(&dummy_buffer[i]);
    }
    mfence();
}

static void delay_after_trigger(void) {
    uint64_t dummy = 0;
    for (int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
        mfence();
    }
    (void)dummy;

    for (int i = 0; i < 100; i++) {
        nop();
    }
    mfence();
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t time1 = timestamp_ns();
    maccess(addr);
    uint64_t time2 = timestamp_ns();
    return time2 - time1;
}

static void train_target(prefetch_gadget_f target_gadget, int stride, int target_train_step) {
    for (int step = 0; step < target_train_step - 1; step++) {
        target_gadget(target_buffer + ((uint64_t)step * (uint64_t)stride));
        mfence();
    }
}

static void train_competitors(load_gadget_f *competitor_loads,
                              int competitor_count) {
    for (int competitor = 0; competitor < competitor_count; competitor++) {
        uint8_t *stream = competitor_buffer + ((size_t)competitor * competitor_region_size);
        load_gadget_f load = competitor_loads[competitor];
        load(stream);
        mfence();
    }
}

static uint64_t run_one_round(prefetch_gadget_f target_gadget,
                              load_gadget_f *competitor_loads,
                              int competitor_count,
                              int stride,
                              int target_train_step) {
    dummy_accesses();
    flush_target_buffer();

    train_target(target_gadget, stride, target_train_step);
    train_competitors(competitor_loads, competitor_count);

    target_gadget(target_buffer + ((uint64_t)(target_train_step - 1) * (uint64_t)stride));
    mfence();

    delay_after_trigger();

    uint8_t *probe_addr = target_buffer +
        ((uint64_t)(target_train_step + PROBE_EXTRA_AFTER_TRAIN_STEP) * (uint64_t)stride);
    return probe_latency(probe_addr);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [base_prefetch_pc target_buffer_addr max_competitors rounds]\n"
            "default: base_prefetch_pc=0x%lx target_buffer_addr=0x%lx max_competitors=%d rounds=%d\n",
            prog,
            (unsigned long)DEFAULT_PREFETCH_PC,
            (unsigned long)DEFAULT_TARGET_BUFFER_ADDR,
            DEFAULT_MAX_COMPETITORS,
            DEFAULT_ROUNDS);
}

int main(int argc, char **argv) {
    uintptr_t base_pc = DEFAULT_PREFETCH_PC;
    uintptr_t target_buffer_addr = DEFAULT_TARGET_BUFFER_ADDR;
    int max_competitors = DEFAULT_MAX_COMPETITORS;
    int rounds = DEFAULT_ROUNDS;
    int stride = DEFAULT_STRIDE;
    int target_train_step = DEFAULT_TARGET_TRAIN_STEP;

    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect OS page size\n");
        return 1;
    }
    page_size = (size_t)detected_page_size;
    dummy_buffer_size = page_size * DUMMY_BUFFER_PAGES;
    competitor_region_size = page_size * COMPETITOR_REGION_PAGES;
    target_buffer_size = page_size * 16;

    if (argc != 1 && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 5) {
        base_pc = strtoull(argv[1], NULL, 0);
        target_buffer_addr = strtoull(argv[2], NULL, 0);
        max_competitors = atoi(argv[3]);
        rounds = atoi(argv[4]);
    }
    if (max_competitors < 0 || max_competitors > 4096 || rounds <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    uint64_t probe_offset =
        (uint64_t)(target_train_step + PROBE_EXTRA_AFTER_TRAIN_STEP) * (uint64_t)stride;
    if (probe_offset + LINE_SIZE > target_buffer_size) {
        fprintf(stderr, "target buffer is too small for probe offset %lu\n",
                (unsigned long)probe_offset);
        return 1;
    }
    target_buffer = map_target_buffer(target_buffer_addr, target_buffer_size);
    if (!target_buffer) {
        return 1;
    }
    memset(target_buffer, -1, target_buffer_size);

    size_t competitor_buffer_size =
        competitor_region_size * (size_t)(max_competitors ? max_competitors : 1);
    competitor_buffer = mmap(NULL, competitor_buffer_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                             -1, 0);
    if (competitor_buffer == MAP_FAILED) {
        fprintf(stderr, "failed to map competitor buffer: %s\n", strerror(errno));
        return 1;
    }
    memset(competitor_buffer, -1, competitor_buffer_size);

    dummy_buffer = mmap(NULL, dummy_buffer_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        fprintf(stderr, "failed to map dummy buffer: %s\n", strerror(errno));
        return 1;
    }

    prefetch_gadget_f target_gadget = map_prefetch_gadget(base_pc);
    if (!target_gadget) {
        return 1;
    }

    load_gadget_f *competitor_loads =
        (load_gadget_f *)calloc((size_t)(max_competitors ? max_competitors : 1),
                                sizeof(load_gadget_f));
    if (!competitor_loads) {
        fprintf(stderr, "failed to allocate competitor load gadgets\n");
        return 1;
    }

    for (int i = 0; i < max_competitors; i++) {
        uintptr_t pc = base_pc + ((uintptr_t)(i + 1) * PC_SPACING);
        competitor_loads[i] = map_load_gadget(pc);
        if (!competitor_loads[i]) {
            max_competitors = i;
            break;
        }
    }

    printf("# stride=%d rounds=%d page_size=%lu base_prefetch_pc=0x%016lx target_buffer=0x%016lx\n",
           stride, rounds, (unsigned long)page_size,
           (unsigned long)base_pc, (unsigned long)target_buffer_addr);
    printf("# target_train_step=%d competitor_accesses=one_load_each probe_offset=%lu pc_spacing=0x%lx\n",
           target_train_step, (unsigned long)probe_offset, (unsigned long)PC_SPACING);
    printf("# competitors\tlatency_ns\n");

    for (int competitor_count = 0; competitor_count <= max_competitors; competitor_count++) {
        uint64_t latency = 0;
        for (int round = 0; round < rounds; round++) {
            latency += run_one_round(target_gadget,
                                     competitor_loads,
                                     competitor_count,
                                     stride,
                                     target_train_step);
        }
        printf("%d\t%lu\n", competitor_count, (unsigned long)(latency / (uint64_t)rounds));
    }

    return 0;
}
