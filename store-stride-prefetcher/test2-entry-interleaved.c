#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "until.h"

#define DUMMY_BUFFER_PAGES 10

#define DEFAULT_STORE_PC 0x500000120ull
#define DEFAULT_STREAM_BUFFER_ADDR 0x600000000ull
#define DEFAULT_ACTIVE_PAGES 512
#define DEFAULT_ROUNDS 1000
#define DEFAULT_PAGE_STEP 1

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 3
#endif

#define MAX_MAPPED_REGIONS 1024

typedef void (*store_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *stream_buffer;
static uint8_t *dummy_buffer;

static size_t page_size;
static size_t stream_buffer_size;
static size_t dummy_buffer_size;

struct mapped_region {
    uintptr_t base;
    uintptr_t end;
};

static struct mapped_region mapped_regions[MAX_MAPPED_REGIONS];
static int mapped_region_count;

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

static uintptr_t page_base(uintptr_t address) {
    return address - (address % page_size);
}

static uintptr_t line_base(uintptr_t address) {
    return address & ~((uintptr_t)LINE_SIZE - 1);
}

static void flush_line_addr(void *addr) {
    flush((void *)line_base((uintptr_t)addr));
}

static int find_region_containing(uintptr_t address) {
    for (int i = 0; i < mapped_region_count; i++) {
        if (mapped_regions[i].base <= address &&
            address < mapped_regions[i].end) {
            return i;
        }
    }

    return -1;
}

static uintptr_t next_region_base_after(uintptr_t address, uintptr_t limit) {
    uintptr_t next = limit;

    for (int i = 0; i < mapped_region_count; i++) {
        if (mapped_regions[i].base > address &&
            mapped_regions[i].base < next) {
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

static void *map_fixed_region(uintptr_t requested_address,
                              size_t size,
                              int prot,
                              const char *name) {
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

        void *mapping = mmap((void *)cursor,
                             gap_size,
                             prot,
                             MAP_FIXED_NOREPLACE | MAP_ANONYMOUS |
                             MAP_PRIVATE | MAP_POPULATE,
                             -1,
                             0);

        if (mapping == MAP_FAILED) {
            fprintf(stderr,
                    "mmap %s at 0x%016lx size 0x%lx failed: %s\n",
                    name,
                    (unsigned long)cursor,
                    (unsigned long)gap_size,
                    strerror(errno));
            return NULL;
        }

        if ((uintptr_t)mapping != cursor) {
            fprintf(stderr,
                    "mmap %s returned wrong address: expected 0x%016lx got %p\n",
                    name,
                    (unsigned long)cursor,
                    mapping);
            return NULL;
        }

        if (remember_region(cursor, gap_end) != 0) {
            return NULL;
        }

        cursor = gap_end;
    }

    return (void *)requested_address;
}

static uint8_t *map_data_buffer(uintptr_t address,
                                size_t size,
                                const char *name) {
    return (uint8_t *)map_fixed_region(address,
                                       size,
                                       PROT_READ | PROT_WRITE,
                                       name);
}

static store_gadget_f map_store_gadget(uintptr_t address) {
    size_t gadget_size =
        (size_t)(_store_gadget_asm_end - _store_gadget_asm_start);

    uint8_t *code = (uint8_t *)map_fixed_region(address,
                                                gadget_size,
                                                PROT_READ | PROT_WRITE | PROT_EXEC,
                                                "store gadget");

    if (!code) {
        return NULL;
    }

    memcpy(code, _store_gadget_asm_start, gadget_size);
    __builtin___clear_cache((char *)code, (char *)(code + gadget_size));

    return (store_gadget_f)(void *)code;
}

static uint8_t *stream_page(int index, int page_step) {
    return stream_buffer + ((size_t)index * (size_t)page_step * page_size);
}

static void dummy_accesses(void) {
    dummyAccess(dummy_buffer, dummy_buffer_size);
    // mfence();
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
    // mfence();
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t time1 = timestamp();
    maccess(addr);
    uint64_t time2 = timestamp();

    return time2 - time1;
}

static void flush_stream_lines(int active_pages, int page_step, int stride_bytes) {
    for (int page = 0; page < active_pages; page++) {
        uint8_t *base = stream_page(page, page_step);

        for (int step = 0; step <= TRAIN_ACCESSES; step++) {
            flush_line_addr(base + ((size_t)step * (size_t)stride_bytes));
        }
    }
    // mfence();
}

static void train_streams_interleaved(store_gadget_f store_gadget,
                                      int active_pages,
                                      int page_step,
                                      int stride_bytes) {
    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        for (int page = 0; page < active_pages; page++) {
            store_gadget(stream_page(page, page_step) +
                         ((size_t)step * (size_t)stride_bytes));
        }
    }
}

static void run_one_round(store_gadget_f store_gadget,
                          int active_pages,
                          int page_step,
                          int stride_bytes,
                          uint64_t *latency_sum) {
    size_t predicted_offset =
        (size_t)TRAIN_ACCESSES * (size_t)stride_bytes;

    dummy_accesses();
    flush_stream_lines(active_pages, page_step, stride_bytes);

    train_streams_interleaved(store_gadget,
                              active_pages,
                              page_step,
                              stride_bytes);

    delay_after_trigger();

    for (int page = 0; page < active_pages; page++) {
        latency_sum[page] += probe_latency(stream_page(page, page_step) +
                                           predicted_offset);
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [store_pc stream_buffer_addr active_pages rounds [page_step]]\n"
            "default: store_pc=0x%lx stream_buffer_addr=0x%lx "
            "active_pages=%d rounds=%d page_step=%d\n",
            prog,
            (unsigned long)DEFAULT_STORE_PC,
            (unsigned long)DEFAULT_STREAM_BUFFER_ADDR,
            DEFAULT_ACTIVE_PAGES,
            DEFAULT_ROUNDS,
            DEFAULT_PAGE_STEP);
}

int main(int argc, char **argv) {
    uintptr_t store_pc = DEFAULT_STORE_PC;
    uintptr_t stream_buffer_addr = DEFAULT_STREAM_BUFFER_ADDR;
    int active_pages = DEFAULT_ACTIVE_PAGES;
    int rounds = DEFAULT_ROUNDS;
    int page_step = DEFAULT_PAGE_STEP;
    int stride_bytes = STRIDE_LINES * LINE_SIZE;

    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect OS page size\n");
        return 1;
    }

    page_size = (size_t)detected_page_size;

    if (argc != 1 && argc != 5 && argc != 6) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc >= 5) {
        store_pc = strtoull(argv[1], NULL, 0);
        stream_buffer_addr = strtoull(argv[2], NULL, 0);
        active_pages = atoi(argv[3]);
        rounds = atoi(argv[4]);
    }

    if (argc == 6) {
        page_step = atoi(argv[5]);
    }

    if (active_pages < 1 || active_pages > 4096 ||
        rounds <= 0 || page_step <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    size_t predicted_offset =
        (size_t)TRAIN_ACCESSES * (size_t)stride_bytes;
    if (predicted_offset + LINE_SIZE > page_size) {
        fprintf(stderr,
                "training/probe range exceeds one 4KB page: train_accesses=%d stride_lines=%d\n",
                TRAIN_ACCESSES,
                STRIDE_LINES);
        return 1;
    }

    size_t span_pages =
        ((size_t)(active_pages - 1) * (size_t)page_step) + 1;
    stream_buffer_size = span_pages * page_size;

    stream_buffer = map_data_buffer(stream_buffer_addr,
                                    stream_buffer_size,
                                    "stream buffer");
    if (!stream_buffer) {
        return 1;
    }
    memset(stream_buffer, -1, stream_buffer_size);

    dummy_buffer_size = page_size * DUMMY_BUFFER_PAGES;
    dummy_buffer = mmap(NULL,
                        dummy_buffer_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1,
                        0);
    if (dummy_buffer == MAP_FAILED) {
        fprintf(stderr, "failed to map dummy buffer: %s\n", strerror(errno));
        return 1;
    }
    memset(dummy_buffer, -1, dummy_buffer_size);

    store_gadget_f store_gadget = map_store_gadget(store_pc);
    if (!store_gadget) {
        return 1;
    }

    uint64_t *latency_sum = calloc((size_t)active_pages, sizeof(*latency_sum));
    if (!latency_sum) {
        fprintf(stderr, "failed to allocate latency sums\n");
        return 1;
    }

    printf("# store-stride interleaved entry-capacity test\n");
    printf("# access pattern: for step in train_accesses, for page in active_pages, store page+step*stride\n");
    printf("# store_pc=0x%016lx stream_buffer=0x%016lx page_size=%lu\n",
           (unsigned long)store_pc,
           (unsigned long)stream_buffer_addr,
           (unsigned long)page_size);
    printf("# stride_lines=%d stride_bytes=%d train_accesses=%d predicted_line=%d\n",
           STRIDE_LINES,
           stride_bytes,
           TRAIN_ACCESSES,
           TRAIN_ACCESSES * STRIDE_LINES);
    printf("# active_pages=%d rounds=%d page_step=%d\n",
           active_pages,
           rounds,
           page_step);
    printf("# lower avg_latency_ns means that stream's predicted line was more likely prefetched\n");
    printf("# active_pages\tstream\tavg_latency_ns\tprobes\n");

    for (int r = 0; r < rounds; r++) {
        run_one_round(store_gadget,
                      active_pages,
                      page_step,
                      stride_bytes,
                      latency_sum);
    }

    for (int page = 0; page < active_pages; page++) {
        uint64_t probes = (uint64_t)rounds;

        printf("%d\t%d\t%lu\t%lu\n",
               active_pages,
               page,
               (unsigned long)(latency_sum[page] / probes),
               (unsigned long)probes);
    }

    return 0;
}
