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
#define DEFAULT_BUFFER_ADDR 0x600000000ull
#define DEFAULT_MAX_IPS 32
#define DEFAULT_ROUNDS 1000
#define DEFAULT_STRIDE (10 * LINE_SIZE)
#define TRAIN_ITERATIONS 5
#define PROBE_EXTRA_AFTER_TRAIN_STEP 15
#define PC_SPACING 0x8ull
#define DUMMY_BUFFER_PAGES 10
#define MAX_GADGET_PAGES 512

typedef void (*prefetch_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *test_buffer;
static uint8_t *dummy_buffer;
static size_t page_size;
static size_t test_buffer_size;
static size_t dummy_buffer_size;
static uintptr_t mapped_gadget_pages[MAX_GADGET_PAGES];
static int mapped_gadget_page_count;

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

static uint8_t *map_test_buffer(uintptr_t address, size_t size) {
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
        fprintf(stderr, "mmap test buffer at 0x%016lx failed: %s\n",
                (unsigned long)base, strerror(errno));
        return NULL;
    }
    return (uint8_t *)address;
}

static void flush_test_buffer(void) {
    for (uint64_t offset = 0; offset < test_buffer_size; offset += LINE_SIZE) {
        flush(&test_buffer[offset]);
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

static void train_ips(prefetch_gadget_f *gadgets, int n_ips, int stride) {
    for (int ip = 0; ip < n_ips; ip++) {
        for (int iter = 0; iter < TRAIN_ITERATIONS; iter++) {
            uint64_t offset = (uint64_t)iter * (uint64_t)stride;
            gadgets[ip](test_buffer + ((uint64_t)ip * page_size) + offset);
            mfence();
        }
    }
}

static void trigger_one_ip(prefetch_gadget_f *gadgets, int ip, int stride) {
    uint64_t trigger_offset = (uint64_t)TRAIN_ITERATIONS * (uint64_t)stride;
    gadgets[ip](test_buffer + ((uint64_t)ip * page_size) + trigger_offset);
    mfence();
}

static uint64_t measure_one_ip(int ip, int stride) {
    uint64_t logical_train_step = (uint64_t)TRAIN_ITERATIONS + 1;
    uint64_t probe_offset = (logical_train_step + PROBE_EXTRA_AFTER_TRAIN_STEP) * (uint64_t)stride;
    uint8_t *probe_addr = test_buffer + ((uint64_t)ip * page_size) + probe_offset;
    return probe_latency(probe_addr);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [base_prefetch_pc buffer_addr max_ips rounds]\n"
            "default: base_prefetch_pc=0x%lx buffer_addr=0x%lx max_ips=%d rounds=%d\n",
            prog,
            (unsigned long)DEFAULT_PREFETCH_PC,
            (unsigned long)DEFAULT_BUFFER_ADDR,
            DEFAULT_MAX_IPS,
            DEFAULT_ROUNDS);
}

int main(int argc, char **argv) {
    uintptr_t base_pc = DEFAULT_PREFETCH_PC;
    uintptr_t buffer_addr = DEFAULT_BUFFER_ADDR;
    int max_ips = DEFAULT_MAX_IPS;
    int rounds = DEFAULT_ROUNDS;
    int stride = DEFAULT_STRIDE;

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
        buffer_addr = strtoull(argv[2], NULL, 0);
        max_ips = atoi(argv[3]);
        rounds = atoi(argv[4]);
    }
    if (max_ips <= 0 || max_ips > 32 || rounds <= 0) {
        print_usage(argv[0]);
        fprintf(stderr, "max_ips must be in [1, 32]; PC spacing 0x%lx keeps low 8 PC bits unique up to 32 IPs.\n",
                (unsigned long)PC_SPACING);
        return 1;
    }

    uint64_t logical_train_step = (uint64_t)TRAIN_ITERATIONS + 1;
    uint64_t trigger_offset = (uint64_t)TRAIN_ITERATIONS * (uint64_t)stride;
    uint64_t probe_offset = (logical_train_step + PROBE_EXTRA_AFTER_TRAIN_STEP) * (uint64_t)stride;
    uint64_t required_page_bytes = probe_offset + LINE_SIZE;
    if (required_page_bytes > page_size) {
        fprintf(stderr, "page size %lu is too small for trigger/probe offset %lu\n",
                (unsigned long)page_size, (unsigned long)required_page_bytes);
        return 1;
    }

    test_buffer_size = page_size * (size_t)max_ips;
    test_buffer = map_test_buffer(buffer_addr, test_buffer_size);
    if (!test_buffer) {
        return 1;
    }
    memset(test_buffer, -1, test_buffer_size);

    dummy_buffer = mmap(NULL, dummy_buffer_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        fprintf(stderr, "failed to map dummy buffer: %s\n", strerror(errno));
        return 1;
    }

    prefetch_gadget_f *gadgets =
        (prefetch_gadget_f *)calloc((size_t)max_ips, sizeof(prefetch_gadget_f));
    if (!gadgets) {
        fprintf(stderr, "failed to allocate gadgets\n");
        return 1;
    }

    for (int ip = 0; ip < max_ips; ip++) {
        uintptr_t pc = base_pc + ((uintptr_t)ip * PC_SPACING);
        gadgets[ip] = map_prefetch_gadget(pc);
        if (!gadgets[ip]) {
            return 1;
        }
    }

    printf("# stride=%d rounds=%d page_size=%lu base_prefetch_pc=0x%016lx buffer=0x%016lx\n",
           stride, rounds, (unsigned long)page_size,
           (unsigned long)base_pc, (unsigned long)buffer_addr);
    printf("# train_iterations=%d logical_train_step=%lu trigger_offset=%lu probe_offset=%lu pc_spacing=0x%lx max_ips=%d\n",
           TRAIN_ITERATIONS, (unsigned long)logical_train_step,
           (unsigned long)trigger_offset, (unsigned long)probe_offset,
           (unsigned long)PC_SPACING, max_ips);
    printf("# each IP uses a different page; lower latency means page_i[(logical_train_step + %d) * stride] was prefetched\n",
           PROBE_EXTRA_AFTER_TRAIN_STEP);
    printf("N");
    for (int ip = 0; ip < max_ips; ip++) {
        printf("\tIP_%d", ip);
    }
    printf("\n");

    for (int n_ips = 1; n_ips <= max_ips; n_ips++) {
        uint64_t *latencies = (uint64_t *)calloc((size_t)n_ips, sizeof(uint64_t));
        if (!latencies) {
            fprintf(stderr, "failed to allocate latency row\n");
            return 1;
        }

        for (int round = 0; round < rounds; round++) {
            for (int ip = 0; ip < n_ips; ip++) {
                dummy_accesses();
                flush_test_buffer();
                train_ips(gadgets, n_ips, stride);
                trigger_one_ip(gadgets, ip, stride);
                delay_after_trigger();
                latencies[ip] += measure_one_ip(ip, stride);
            }
        }

        printf("%d", n_ips);
        for (int ip = 0; ip < max_ips; ip++) {
            if (ip < n_ips) {
                printf("\t%lu", (unsigned long)(latencies[ip] / (uint64_t)rounds));
            } else {
                printf("\t-1");
            }
        }
        printf("\n");
        free(latencies);
    }

    return 0;
}
