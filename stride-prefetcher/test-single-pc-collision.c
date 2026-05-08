#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "test-single-pc-collision.c expects AArch64 PRFM/DC CIVAC instructions."
#endif

#define LINE_SIZE 64
#define ARRAY_ALIGNMENT 4096
#define ITEMS 2048
#define DUMMY_BUFFER_PAGES 10

#define DEFAULT_BASE_PREFETCH_PC 0x500000120ull
#define DEFAULT_MIN_DIFF_BIT 3
#define DEFAULT_MAX_DIFF_BIT 47
#define DEFAULT_ROUNDS 1000
#define DEFAULT_STRIDE (10 * LINE_SIZE)
#define DEFAULT_MAX_TRAIN_STEP 20
#define MAX_MAPPED_PAGES 256

typedef void (*prefetch_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t array2[ITEMS * LINE_SIZE] __attribute__((aligned(ARRAY_ALIGNMENT)));
static uint8_t *dummy_buffer;
static size_t page_size;
static size_t dummy_buffer_size;
static uintptr_t mapped_pages[MAX_MAPPED_PAGES];
static int mapped_page_count;

extern char _prefetch_gadget_asm_start[];
extern char _prefetch_gadget_asm_end[];

asm(
    ".global _prefetch_gadget_asm_start\n"
    ".global _prefetch_gadget_asm_end\n"
    "_prefetch_gadget_asm_start:\n"
    "    prfm pldl1keep, [x0]\n"
    "    ret\n"
    "_prefetch_gadget_asm_end:\n"
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

static int page_is_mapped_by_test(uintptr_t page) {
    for (int i = 0; i < mapped_page_count; i++) {
        if (mapped_pages[i] == page) {
            return 1;
        }
    }
    return 0;
}

static int ensure_gadget_page(uintptr_t page) {
    if (page_is_mapped_by_test(page)) {
        return 0;
    }

    if (mapped_page_count >= MAX_MAPPED_PAGES) {
        fprintf(stderr, "too many mapped gadget pages\n");
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
        fprintf(stderr, "mmap returned wrong page: expected 0x%016lx got %p\n",
                (unsigned long)page, mapping);
        return -1;
    }

    mapped_pages[mapped_page_count++] = page;
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

static void flush_array2(void) {
    for (uint64_t offset = 0; offset < ITEMS * LINE_SIZE; offset += LINE_SIZE) {
        flush(&array2[offset]);
    }
    mfence();
}

static void dummy_accesses(void) {
    for (uint64_t i = 0; i < dummy_buffer_size; i += LINE_SIZE) {
        maccess(&dummy_buffer[i]);
    }
    mfence();
}

static uint64_t run_one_round(prefetch_gadget_f train_prefetch,
                              prefetch_gadget_f trigger_prefetch,
                              int stride,
                              int train_step) {
    dummy_accesses();
    flush_array2();

    for (int step = 0; step < train_step - 1; step++) {
        train_prefetch(array2 + (step * stride));
        mfence();
    }

    trigger_prefetch(array2 + ((train_step - 1) * stride));
    mfence();

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

    uint8_t *probe_addr = array2 + ((train_step + 15) * stride);
    uint64_t time1 = timestamp_ns();
    maccess(probe_addr);
    uint64_t time2 = timestamp_ns();
    return time2 - time1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [base_prefetch_pc min_diff_bit max_diff_bit rounds]\n"
            "default: base_prefetch_pc=0x%lx min_diff_bit=%d max_diff_bit=%d rounds=%d\n",
            prog,
            (unsigned long)DEFAULT_BASE_PREFETCH_PC,
            DEFAULT_MIN_DIFF_BIT,
            DEFAULT_MAX_DIFF_BIT,
            DEFAULT_ROUNDS);
}

int main(int argc, char **argv) {
    uintptr_t base_pc = DEFAULT_BASE_PREFETCH_PC;
    int min_diff_bit = DEFAULT_MIN_DIFF_BIT;
    int max_diff_bit = DEFAULT_MAX_DIFF_BIT;
    int rounds = DEFAULT_ROUNDS;
    int stride = DEFAULT_STRIDE;
    int max_train_step = DEFAULT_MAX_TRAIN_STEP;

    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect OS page size\n");
        return 1;
    }
    page_size = (size_t)detected_page_size;
    dummy_buffer_size = page_size * DUMMY_BUFFER_PAGES;

    if (argc != 1 && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 5) {
        base_pc = strtoull(argv[1], NULL, 0);
        min_diff_bit = atoi(argv[2]);
        max_diff_bit = atoi(argv[3]);
        rounds = atoi(argv[4]);
    }

    if (min_diff_bit < 3 || max_diff_bit >= 48 || min_diff_bit > max_diff_bit || rounds <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    memset(array2, -1, sizeof(array2));
    dummy_buffer = mmap(NULL, dummy_buffer_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        fprintf(stderr, "failed to map dummy buffer: %s\n", strerror(errno));
        return 1;
    }

    for (int i = 0; i < ITEMS; i++) {
        maccess(&array2[i * LINE_SIZE]);
    }
    flush_array2();

    prefetch_gadget_f train_prefetch = map_prefetch_gadget(base_pc);
    if (!train_prefetch) {
        return 1;
    }

    printf("# stride=%d rounds=%d page_size=%lu base_prefetch_pc=0x%016lx\n",
           stride, rounds, (unsigned long)page_size, (unsigned long)base_pc);
    printf("# latency_ns; lower latency means the probe line was more likely prefetched\n");
    printf("diff_bit");
    for (int train_step = 1; train_step <= max_train_step; train_step++) {
        printf("\ttrain_%d", train_step);
    }
    printf("\n");

    printf("same_pc");
    for (int train_step = 1; train_step <= max_train_step; train_step++) {
        uint64_t latency = 0;
        for (int atk_round = 0; atk_round < rounds; atk_round++) {
            latency += run_one_round(train_prefetch, train_prefetch, stride, train_step);
        }
        printf("\t%lu", (unsigned long)(latency / (uint64_t)rounds));
    }
    printf("\n");

    for (int diff_bit = min_diff_bit; diff_bit <= max_diff_bit; diff_bit++) {
        uintptr_t trigger_pc = base_pc ^ (1ull << diff_bit);
        prefetch_gadget_f trigger_prefetch = map_prefetch_gadget(trigger_pc);
        if (!trigger_prefetch) {
            printf("%d", diff_bit);
            for (int train_step = 1; train_step <= max_train_step; train_step++) {
                printf("\t-1");
            }
            printf("\n");
            continue;
        }

        printf("%d", diff_bit);
        for (int train_step = 1; train_step <= max_train_step; train_step++) {
            uint64_t latency = 0;
            for (int atk_round = 0; atk_round < rounds; atk_round++) {
                latency += run_one_round(train_prefetch, trigger_prefetch, stride, train_step);
            }
            printf("\t%lu", (unsigned long)(latency / (uint64_t)rounds));
        }
        printf("\n");
    }

    return 0;
}
