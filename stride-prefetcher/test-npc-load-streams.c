#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef __aarch64__
#error "test-npc-load-streams.c expects AArch64 instructions."
#endif

#define LINE_SIZE 64
#define DEFAULT_BASE_PC 0x500200140ull
#define DEFAULT_BUFFER_ADDR 0x610000000ull
#define DEFAULT_N_PCS 12
#define DEFAULT_ROUNDS 100
#define MIN_M_STREAMS 1
#define MAX_M_STREAMS 12
#define TRAIN_ACCESSES 18
#define PROBE_AFTER_TRIGGER 15
#define STRIDE (17 * LINE_SIZE)
#define PC_SPACING 0x20ull
#define USE_SEPARATE_TRIGGER_PC 0
#define A76_LOW15_PC_SLOTS (0x8000ull / PC_SPACING)
#define STREAM_REGION_PAGES 16
#define DUMMY_SEQUENTIAL_BUFFER_PAGES 10
#define MAX_GADGET_PAGES 512

typedef void (*load_gadget_f)(void *);

static uint8_t *test_buffer;
static uint8_t *dummy_buffer;
static size_t page_size;
static size_t stream_region_size;
static size_t test_buffer_size;
static size_t dummy_buffer_size;
static uintptr_t mapped_gadget_pages[MAX_GADGET_PAGES];
static int mapped_gadget_page_count;

extern char _load_gadget_asm_start[];
extern char _load_gadget_asm_end[];

asm(
    ".pushsection .text\n"
    ".global _load_gadget_asm_start\n"
    ".global _load_gadget_asm_end\n"
    "_load_gadget_asm_start:\n"
    "    hint #34\n"
    "    ldrb w0, [x0]\n"
    "    ret\n"
    "_load_gadget_asm_end:\n"
    "    nop\n"
    ".popsection\n"
);

static inline void mfence(void) {
    asm volatile("DSB SY\nISB" ::: "memory");
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

    mapped_gadget_pages[mapped_gadget_page_count++] = page;
    return 0;
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
        flush(test_buffer + offset);
    }
    mfence();
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t time1 = timestamp_ns();
    maccess(addr);
    uint64_t time2 = timestamp_ns();
    return time2 - time1;
}

static void train_streams(load_gadget_f *loads, int n_pcs, int m_streams) {
    for (int stream = 0; stream < m_streams; stream++) {
        uint8_t *base = test_buffer + ((size_t)stream * stream_region_size);
        load_gadget_f load = loads[stream % n_pcs];
        for (int access = 0; access < TRAIN_ACCESSES-1; access++) {
            load(base + ((uint64_t)access * STRIDE));
            mfence();
        }
    }
}

static inline void dummyAccesses2() {
    // printf("dummyAccesses2\n");
    for (uint64_t i = 0; i < dummy_buffer_size; i += LINE_SIZE) {
        asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
        // maccess(&dummy_buffer[i]);
    }
    mfence();
}

static uint64_t run_one_round(load_gadget_f *loads,
                              int n_pcs,
                              int m_streams,
                              int probe_stream,
                              load_gadget_f trigger_load) {
    dummyAccesses2();
    flush_test_buffer();
    train_streams(loads, n_pcs, m_streams);
    flush_test_buffer();

    uint8_t *base = test_buffer + ((size_t)probe_stream * stream_region_size);
#if USE_SEPARATE_TRIGGER_PC
    trigger_load(base + ((uint64_t)(TRAIN_ACCESSES - 1) * STRIDE));
#else
    load_gadget_f load = loads[probe_stream % n_pcs];
    load(base + ((uint64_t)(TRAIN_ACCESSES - 1) * STRIDE));
#endif
    mfence();

    uint8_t *probe = base + ((uint64_t)(TRAIN_ACCESSES + PROBE_AFTER_TRIGGER) * STRIDE);
    return probe_latency(probe);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [n_pcs rounds base_pc buffer_addr]\n"
            "default: n_pcs=%d rounds=%d base_pc=0x%lx buffer_addr=0x%lx\n",
            prog,
            DEFAULT_N_PCS,
            DEFAULT_ROUNDS,
            (unsigned long)DEFAULT_BASE_PC,
            (unsigned long)DEFAULT_BUFFER_ADDR);
}

int main(int argc, char **argv) {
    int n_pcs = DEFAULT_N_PCS;
    int rounds = DEFAULT_ROUNDS;
    uintptr_t base_pc = DEFAULT_BASE_PC;
    uintptr_t buffer_addr = DEFAULT_BUFFER_ADDR;

    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect OS page size\n");
        return 1;
    }
    page_size = (size_t)detected_page_size;
    stream_region_size = page_size * STREAM_REGION_PAGES;
    test_buffer_size = stream_region_size * MAX_M_STREAMS;
    dummy_buffer_size = page_size * DUMMY_SEQUENTIAL_BUFFER_PAGES;

    if (argc != 1 && argc != 3 && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc >= 3) {
        n_pcs = atoi(argv[1]);
        rounds = atoi(argv[2]);
    }
    if (argc == 5) {
        base_pc = strtoull(argv[3], NULL, 0);
        buffer_addr = strtoull(argv[4], NULL, 0);
    }
    if (n_pcs <= 0 || n_pcs > 1024 || rounds <= 0) {
        print_usage(argv[0]);
        return 1;
    }
    if ((uint64_t)n_pcs + USE_SEPARATE_TRIGGER_PC > A76_LOW15_PC_SLOTS) {
        fprintf(stderr,
                "too many PCs for distinct A76 low-15-bit entries: train_pcs=%d separate_trigger=%d slots=%lu\n",
                n_pcs, USE_SEPARATE_TRIGGER_PC, (unsigned long)A76_LOW15_PC_SLOTS);
        return 1;
    }

    uint64_t max_offset = (uint64_t)(TRAIN_ACCESSES + PROBE_AFTER_TRIGGER) * STRIDE;
    if (max_offset + LINE_SIZE > stream_region_size) {
        fprintf(stderr, "stream region is too small for probe offset %lu\n",
                (unsigned long)max_offset);
        return 1;
    }

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
    memset(dummy_buffer, -1, dummy_buffer_size);

    load_gadget_f *loads = (load_gadget_f *)calloc((size_t)n_pcs, sizeof(load_gadget_f));
    if (!loads) {
        fprintf(stderr, "failed to allocate load gadgets\n");
        return 1;
    }

    for (int i = 0; i < n_pcs; i++) {
        uintptr_t pc = base_pc + ((uintptr_t)i * PC_SPACING);
        loads[i] = map_load_gadget(pc);
        if (!loads[i]) {
            return 1;
        }
    }

    uintptr_t trigger_pc = base_pc + ((uintptr_t)n_pcs * PC_SPACING);
    load_gadget_f trigger_load = NULL;
#if USE_SEPARATE_TRIGGER_PC
    trigger_load = map_load_gadget(trigger_pc);
    if (!trigger_load) {
        return 1;
    }
#endif

    printf("# n_pcs=%d m_streams=%d..%d train_accesses=%d stride=%d probe_after_trigger=%d rounds=%d\n",
           n_pcs, MIN_M_STREAMS, MAX_M_STREAMS,
           TRAIN_ACCESSES, STRIDE, PROBE_AFTER_TRIGGER, rounds);
    printf("# base_pc=0x%016lx trigger_mode=%s trigger_pc=0x%016lx buffer_addr=0x%016lx page_size=%lu dummy_pages=%d\n",
           (unsigned long)base_pc,
           USE_SEPARATE_TRIGGER_PC ? "separate_pc" : "same_pc",
           (unsigned long)trigger_pc,
           (unsigned long)buffer_addr,
           (unsigned long)page_size,
           DUMMY_SEQUENTIAL_BUFFER_PAGES);
    printf("# each cell trains m_streams streams, then triggers/probes only that stream\n");
    printf("m_streams");
    for (int stream = 0; stream < MAX_M_STREAMS; stream++) {
        printf("\tstream%d", stream);
    }
    printf("\n");

    for (int m_streams = MIN_M_STREAMS; m_streams <= MAX_M_STREAMS; m_streams++) {
        uint64_t latencies[MAX_M_STREAMS] = {0};
        for (int stream = 0; stream < m_streams; stream++) {
            for (int round = 0; round < rounds; round++) {
                latencies[stream] += run_one_round(loads,
                                                   n_pcs,
                                                   m_streams,
                                                   stream,
                                                   trigger_load);
            }
        }

        printf("%d", m_streams);
        for (int stream = 0; stream < MAX_M_STREAMS; stream++) {
            if (stream < m_streams) {
                printf("\t%lu", (unsigned long)(latencies[stream] / (uint64_t)rounds));
            } else {
                printf("\t");
            }
        }
        printf("\n");
    }

    return 0;
}
