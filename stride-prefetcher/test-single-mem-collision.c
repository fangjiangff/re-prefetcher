#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "test-single-mem-collision.c expects AArch64 PRFM/DC CIVAC instructions."
#endif

#define LINE_SIZE 64
#define BUFFER_PAGES 128
#define DUMMY_BUFFER_PAGES 10

#define DEFAULT_PREFETCH_PC 0x500000120ull
#define DEFAULT_VICTIM_BUFFER_ADDR 0x600000000ull
#define DEFAULT_MIN_DIFF_BIT 0
#define DEFAULT_MAX_DIFF_BIT 47
#define DEFAULT_ROUNDS 1000
#define DEFAULT_STRIDE (10 * LINE_SIZE)
#define DEFAULT_MAX_TRAIN_STEP 20
#define MAX_MAPPED_REGIONS 512

typedef void (*prefetch_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *victim_buffer;
static uint8_t *dummy_buffer;
static size_t page_size;
static size_t victim_buffer_size;
static size_t dummy_buffer_size;
struct mapped_region {
    uintptr_t base;
    uintptr_t end;
};
static struct mapped_region mapped_regions[MAX_MAPPED_REGIONS];
static int mapped_region_count;

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

static int find_region_containing(uintptr_t address) {
    for (int i = 0; i < mapped_region_count; i++) {
        if (mapped_regions[i].base <= address && address < mapped_regions[i].end) {
            return i;
        }
    }
    return -1;
}

static uintptr_t next_region_base_after(uintptr_t address, uintptr_t limit) {
    uintptr_t next = limit;
    for (int i = 0; i < mapped_region_count; i++) {
        if (mapped_regions[i].base > address && mapped_regions[i].base < next) {
            next = mapped_regions[i].base;
        }
    }
    return next;
}

static int remember_region(uintptr_t base, uintptr_t end) {
    if (mapped_region_count >= MAX_MAPPED_REGIONS) {
        fprintf(stderr, "too many mapped regions\n");
        return -1;
    }
    mapped_regions[mapped_region_count].base = base;
    mapped_regions[mapped_region_count].end = end;
    mapped_region_count++;
    return 0;
}

static void *map_fixed_region(uintptr_t requested_address, size_t size, int prot, const char *name) {
    uintptr_t base = page_base(requested_address);
    uintptr_t end = requested_address + size;
    size_t map_size = end - base;
    if (map_size % page_size) {
        map_size += page_size - (map_size % page_size);
    }

    uintptr_t map_end = base + map_size;
    uintptr_t cursor = base;
    while (cursor < map_end) {
        int region = find_region_containing(cursor);
        if (region >= 0) {
            cursor = mapped_regions[region].end;
            continue;
        }

        uintptr_t gap_end = next_region_base_after(cursor, map_end);
        size_t gap_size = gap_end - cursor;
        void *mapping = mmap((void *)cursor, gap_size, prot,
                             MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE,
                             -1, 0);
        if (mapping == MAP_FAILED) {
            fprintf(stderr, "mmap %s at 0x%016lx size 0x%lx failed: %s\n",
                    name, (unsigned long)cursor, (unsigned long)gap_size, strerror(errno));
            return NULL;
        }
        if ((uintptr_t)mapping != cursor) {
            fprintf(stderr, "mmap %s returned wrong address: expected 0x%016lx got %p\n",
                    name, (unsigned long)cursor, mapping);
            return NULL;
        }
        if (remember_region(cursor, gap_end) != 0) {
            return NULL;
        }
        cursor = gap_end;
    }
    return (void *)requested_address;
}

static prefetch_gadget_f map_prefetch_gadget(uintptr_t address) {
    size_t gadget_size = (size_t)(_prefetch_gadget_asm_end - _prefetch_gadget_asm_start);
    uint8_t *code = map_fixed_region(address, gadget_size,
                                     PROT_READ | PROT_WRITE | PROT_EXEC,
                                     "prefetch gadget");
    if (!code) {
        return NULL;
    }
    memcpy(code, _prefetch_gadget_asm_start, gadget_size);
    __builtin___clear_cache((char *)code, (char *)(code + gadget_size));
    return (prefetch_gadget_f)(void *)code;
}

static uint8_t *map_data_buffer(uintptr_t address, size_t size, const char *name) {
    return (uint8_t *)map_fixed_region(address, size, PROT_READ | PROT_WRITE, name);
}

static void flush_victim_buffer(void) {
    for (uint64_t offset = 0; offset < victim_buffer_size; offset += LINE_SIZE) {
        flush(&victim_buffer[offset]);
    }
    mfence();
}

static void dummy_accesses(void) {
    for (uint64_t i = 0; i < dummy_buffer_size; i += LINE_SIZE) {
        maccess(&dummy_buffer[i]);
    }
    mfence();
}

static uint64_t run_one_round(prefetch_gadget_f prefetch_gadget,
                              uint8_t *train_buffer,
                              int stride,
                              int train_step) {
    dummy_accesses();
    flush_victim_buffer();

    for (int step = 0; step < train_step - 1; step++) {
        prefetch_gadget(train_buffer + (step * stride));
        mfence();
    }

    prefetch_gadget(victim_buffer + ((train_step - 1) * stride));
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

    uint8_t *probe_addr = victim_buffer + ((train_step + 15) * stride);
    uint64_t time1 = timestamp_ns();
    maccess(probe_addr);
    uint64_t time2 = timestamp_ns();
    return time2 - time1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [prefetch_pc victim_buffer_addr min_diff_bit max_diff_bit rounds]\n"
            "default: prefetch_pc=0x%lx victim_buffer_addr=0x%lx min_diff_bit=%d max_diff_bit=%d rounds=%d\n",
            prog,
            (unsigned long)DEFAULT_PREFETCH_PC,
            (unsigned long)DEFAULT_VICTIM_BUFFER_ADDR,
            DEFAULT_MIN_DIFF_BIT,
            DEFAULT_MAX_DIFF_BIT,
            DEFAULT_ROUNDS);
}

int main(int argc, char **argv) {
    uintptr_t prefetch_pc = DEFAULT_PREFETCH_PC;
    uintptr_t victim_buffer_addr = DEFAULT_VICTIM_BUFFER_ADDR;
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
    victim_buffer_size = page_size * BUFFER_PAGES;
    dummy_buffer_size = page_size * DUMMY_BUFFER_PAGES;

    if (argc != 1 && argc != 6) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc == 6) {
        prefetch_pc = strtoull(argv[1], NULL, 0);
        victim_buffer_addr = strtoull(argv[2], NULL, 0);
        min_diff_bit = atoi(argv[3]);
        max_diff_bit = atoi(argv[4]);
        rounds = atoi(argv[5]);
    }

    if (min_diff_bit < 0 || max_diff_bit >= 48 || min_diff_bit > max_diff_bit || rounds <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    victim_buffer = map_data_buffer(victim_buffer_addr,
                                    victim_buffer_size + page_size,
                                    "victim buffer");
    if (!victim_buffer) {
        return 1;
    }
    memset(victim_buffer, -1, victim_buffer_size);

    dummy_buffer = mmap(NULL, dummy_buffer_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        fprintf(stderr, "failed to map dummy buffer: %s\n", strerror(errno));
        return 1;
    }

    for (uint64_t i = 0; i < victim_buffer_size; i += LINE_SIZE) {
        maccess(&victim_buffer[i]);
    }
    flush_victim_buffer();

    prefetch_gadget_f prefetch_gadget = map_prefetch_gadget(prefetch_pc);
    if (!prefetch_gadget) {
        return 1;
    }

    printf("# stride=%d rounds=%d page_size=%lu prefetch_pc=0x%016lx victim_buffer=0x%016lx\n",
           stride, rounds, (unsigned long)page_size,
           (unsigned long)prefetch_pc, (unsigned long)victim_buffer_addr);
    printf("# latency_ns; lower latency means victim probe line was more likely prefetched\n");
    printf("diff_bit");
    for (int train_step = 1; train_step <= max_train_step; train_step++) {
        printf("\ttrain_%d", train_step);
    }
    printf("\n");

    printf("same_addr");
    for (int train_step = 1; train_step <= max_train_step; train_step++) {
        uint64_t latency = 0;
        for (int atk_round = 0; atk_round < rounds; atk_round++) {
            latency += run_one_round(prefetch_gadget, victim_buffer, stride, train_step);
        }
        printf("\t%lu", (unsigned long)(latency / (uint64_t)rounds));
    }
    printf("\n");

    for (int diff_bit = min_diff_bit; diff_bit <= max_diff_bit; diff_bit++) {
        uintptr_t colliding_buffer_addr = victim_buffer_addr ^ (1ull << diff_bit);
        uint8_t *colliding_buffer = map_data_buffer(colliding_buffer_addr,
                                                    victim_buffer_size,
                                                    "colliding buffer");
        if (!colliding_buffer) {
            printf("%d", diff_bit);
            for (int train_step = 1; train_step <= max_train_step; train_step++) {
                printf("\t-1");
            }
            printf("\n");
            continue;
        }
        memset(colliding_buffer, -1, victim_buffer_size);

        printf("%d", diff_bit);
        for (int train_step = 1; train_step <= max_train_step; train_step++) {
            uint64_t latency = 0;
            for (int atk_round = 0; atk_round < rounds; atk_round++) {
                latency += run_one_round(prefetch_gadget, colliding_buffer, stride, train_step);
            }
            printf("\t%lu", (unsigned long)(latency / (uint64_t)rounds));
        }
        printf("\n");
    }

    return 0;
}
