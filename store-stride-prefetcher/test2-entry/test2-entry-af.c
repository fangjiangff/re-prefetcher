#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../until.h"

#define DUMMY_BUFFER_PAGES 10

#define DEFAULT_ACCESS_PC 0x500000120ull
#define DEFAULT_BUFFER_ADDR 0x600000000ull
#define DEFAULT_ENTRIES 18
#define DEFAULT_ROUNDS 1000
#define DEFAULT_PAGE_STEP 1

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#define DEFAULT_STRIDE (STRIDE_LINES * LINE_SIZE)

#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 5
#endif

#ifndef TRIGGER_ACCESSES
#define TRIGGER_ACCESSES 1
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 64
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#define TRIGGER0_LINE (TRAIN_ACCESSES * STRIDE_LINES)
#define LAST_TRIGGER_LINE ((TRAIN_ACCESSES + TRIGGER_ACCESSES - 1) * STRIDE_LINES)
#define PREDICTED_LINE ((TRAIN_ACCESSES + TRIGGER_ACCESSES) * STRIDE_LINES)

#define MAX_MAPPED_REGIONS 1024

typedef void (*access_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *page_buffer;
static uint8_t *dummy_buffer;
static size_t page_size;
static size_t page_buffer_size;
static size_t per_stream_size;
static size_t dummy_buffer_size;

struct mapped_region {
    uintptr_t base;
    uintptr_t end;
};

static struct mapped_region mapped_regions[MAX_MAPPED_REGIONS];
static int mapped_region_count;

extern char _access_gadget_asm_start[];
extern char _access_gadget_asm_end[];

asm(
    ".global _access_gadget_asm_start\n"
    ".global _access_gadget_asm_end\n"
    "_access_gadget_asm_start:\n"
#if TRAIN_ACCESS_LOAD
    "    ldrb w0, [x0]\n"
#else
    "    strb w0, [x0]\n"
#endif
    "    ret\n"
    "_access_gadget_asm_end:\n"
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

static access_gadget_f map_access_gadget(uintptr_t address) {
    size_t gadget_size =
        (size_t)(_access_gadget_asm_end - _access_gadget_asm_start);

    uint8_t *code = (uint8_t *)map_fixed_region(address,
                                                gadget_size,
                                                PROT_READ | PROT_WRITE | PROT_EXEC,
                                                "access gadget");

    if (!code) {
        return NULL;
    }

    memcpy(code, _access_gadget_asm_start, gadget_size);

    __builtin___clear_cache((char *)code,
                            (char *)(code + gadget_size));

    return (access_gadget_f)(void *)code;
}

static inline __attribute__((always_inline)) const char *access_name(void) {
#if TRAIN_ACCESS_LOAD
    return "load";
#else
    return "store";
#endif
}

static inline __attribute__((always_inline)) const char *access_instruction(void) {
#if TRAIN_ACCESS_LOAD
    return "ldrb";
#else
    return "strb";
#endif
}

static void dummy_accesses(void) {
    for (uint32_t j = 0; j < dummy_buffer_size; j += LINE_SIZE) {
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
    }
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t time1 = timestamp();
    maccess(addr);
    uint64_t time2 = timestamp();

    return time2 - time1;
}

static uint8_t *stream_page(int page_index, int page_step_pages) {
    return page_buffer +
           ((size_t)page_index * (size_t)page_step_pages * page_size);
}

static void flush_stream_lines(int entry_count,
                               int page_step_pages) {
    for (int i = 0; i < entry_count; i++) {
        uint8_t *page = stream_page(i, page_step_pages);

        for (size_t offset = 0; offset < per_stream_size; offset += LINE_SIZE) {
            flush_line_addr(page + offset);
        }
    }
}

static void train_streams(access_gadget_f access_gadget,
                          int entry_count,
                          int page_step_pages,
                          int stride) {
    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        for (int i = 0; i < entry_count; i++) {
            access_gadget(stream_page(i, page_step_pages) +
                          ((uint64_t)step * (uint64_t)stride));
        }
    }
}

static void trigger_streams(access_gadget_f access_gadget,
                            int entry_count,
                            int page_step_pages,
                            int stride) {
    for (int index = 0; index < TRIGGER_ACCESSES; index++) {
        for (int i = 0; i < entry_count; i++) {
            access_gadget(stream_page(i, page_step_pages) +
                         ((uintptr_t)(TRAIN_ACCESSES + index) *
                          (uintptr_t)stride));
        }
    }
}

static const char *probe_role(int probe_line, int predicted_line) {
    int stride_lines = STRIDE_LINES;

    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        if (probe_line == step * stride_lines) {
            return "trained";
        }
    }

    if (probe_line >= TRIGGER0_LINE &&
        probe_line <= LAST_TRIGGER_LINE &&
        ((probe_line - TRIGGER0_LINE) % stride_lines) == 0) {
        return "trigger";
    }

    if (probe_line == predicted_line) {
        return "predicted";
    }

    return "candidate";
}

static void run_probe_map(access_gadget_f access_gadget,
                          int entry_count,
                          int page_step_pages,
                          int do_trigger,
                          int stride,
                          int predicted_line,
                          int rounds) {
    uint64_t *latency_sum = calloc((size_t)entry_count * PROBE_POSITIONS,
                                   sizeof(uint64_t));
    int *probe_count = calloc((size_t)entry_count * PROBE_POSITIONS,
                              sizeof(int));

    if (!latency_sum || !probe_count) {
        fprintf(stderr, "failed to allocate latency buffers\n");
        free(latency_sum);
        free(probe_count);
        return;
    }

    for (int r = 0; r < rounds; r++) {
        int probe_line = r % PROBE_POSITIONS;

        dummy_accesses();
        flush_stream_lines(entry_count, page_step_pages);
        train_streams(access_gadget, entry_count, page_step_pages, stride);

        if (do_trigger) {
            trigger_streams(access_gadget, entry_count, page_step_pages, stride);
        }

        for (int i = 0; i < entry_count; i++) {
            size_t idx = ((size_t)i * PROBE_POSITIONS) + (size_t)probe_line;
            uint8_t *probe_addr = stream_page(i, page_step_pages) +
                                  ((uint64_t)probe_line * (uint64_t)LINE_SIZE);
            latency_sum[idx] += probe_latency(probe_addr);
            probe_count[idx]++;
        }
    }

    int reported_n = do_trigger ? entry_count : -entry_count;

    for (int i = 0; i < entry_count; i++) {
        for (int probe_line = 0; probe_line < PROBE_POSITIONS; probe_line++) {
            size_t idx = ((size_t)i * PROBE_POSITIONS) + (size_t)probe_line;
            uint64_t avg = 0;

            if (probe_count[idx] > 0) {
                avg = latency_sum[idx] / (uint64_t)probe_count[idx];
            }

            printf("%d\t%d\t%d\t%d\t%s\t%lu\t%d\n",
                   reported_n,
                   i,
                   probe_line,
                   probe_line * LINE_SIZE,
                   probe_role(probe_line, predicted_line),
                   (unsigned long)avg,
                   probe_count[idx]);
        }
    }

    free(latency_sum);
    free(probe_count);
}

static void run_predicted_lines(access_gadget_f access_gadget,
                                int entry_count,
                                int page_step_pages,
                                int stride,
                                int predicted_line,
                                int rounds) {
    uint64_t *latency_sum = calloc((size_t)entry_count, sizeof(uint64_t));
    int *probe_count = calloc((size_t)entry_count, sizeof(int));

    if (!latency_sum || !probe_count) {
        fprintf(stderr, "failed to allocate latency buffers\n");
        free(latency_sum);
        free(probe_count);
        return;
    }

    for (int r = 0; r < rounds; r++) {
        int target_page = r % entry_count;
        uint8_t *probe_addr = stream_page(target_page, page_step_pages) +
                              ((uint64_t)predicted_line * (uint64_t)LINE_SIZE);

        dummy_accesses();
        flush_stream_lines(entry_count, page_step_pages);
        train_streams(access_gadget, entry_count, page_step_pages, stride);
        trigger_streams(access_gadget, entry_count, page_step_pages, stride);

        latency_sum[target_page] += probe_latency(probe_addr);
        probe_count[target_page]++;
    }

    for (int i = 0; i < entry_count; i++) {
        uint64_t avg = 0;

        if (probe_count[i] > 0) {
            avg = latency_sum[i] / (uint64_t)probe_count[i];
        }

        printf("%d\t%d\t%d\t%d\t%lu\t%d\n",
               entry_count,
               i,
               predicted_line,
               predicted_line * LINE_SIZE,
               (unsigned long)avg,
               probe_count[i]);
    }

    free(latency_sum);
    free(probe_count);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [access_pc buffer_addr entries rounds [page_step_pages]]\n"
            "default: access_pc=0x%lx buffer_addr=0x%lx "
            "entries=%d rounds=%d page_step_pages=%d\n",
            prog,
            (unsigned long)DEFAULT_ACCESS_PC,
            (unsigned long)DEFAULT_BUFFER_ADDR,
            DEFAULT_ENTRIES,
            DEFAULT_ROUNDS,
            DEFAULT_PAGE_STEP);
}

int main(int argc, char **argv) {
    uintptr_t access_pc = DEFAULT_ACCESS_PC;
    uintptr_t buffer_addr = DEFAULT_BUFFER_ADDR;
    int entries = DEFAULT_ENTRIES;
    int rounds = DEFAULT_ROUNDS;
    int page_step_pages = DEFAULT_PAGE_STEP;
    int stride = DEFAULT_STRIDE;

    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect OS page size\n");
        return 1;
    }

    page_size = (size_t)detected_page_size;

    if (page_size != 4096) {
        fprintf(stderr,
                "warning: page_size=%lu, this experiment assumes 4KB page organization\n",
                (unsigned long)page_size);
    }

    if (argc != 1 && argc != 5 && argc != 6) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc >= 5) {
        access_pc = strtoull(argv[1], NULL, 0);
        buffer_addr = strtoull(argv[2], NULL, 0);
        entries = atoi(argv[3]);
        rounds = atoi(argv[4]);
    }

    if (argc == 6) {
        page_step_pages = atoi(argv[5]);
    }

    if (entries < 1 ||
        entries > 4096 ||
        rounds <= 0 ||
        page_step_pages <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    if ((buffer_addr % page_size) != 0) {
        fprintf(stderr,
                "warning: buffer_addr is not page-aligned: 0x%016lx\n",
                (unsigned long)buffer_addr);
    }

    if (TRIGGER_ACCESSES < 1 || TRIGGER_ACCESSES > 2) {
        fprintf(stderr, "TRIGGER_ACCESSES must be 1 or 2\n");
        return 1;
    }

    int predicted_line = PREDICTED_LINE;
    uint64_t predicted_offset =
        (uint64_t)predicted_line * (uint64_t)LINE_SIZE;
    uint64_t last_trigger_offset =
        (uint64_t)LAST_TRIGGER_LINE * (uint64_t)LINE_SIZE;

    if (PROBE_POSITIONS <= 0 || predicted_line >= PROBE_POSITIONS) {
        fprintf(stderr,
                "PROBE_POSITIONS=%d must include predicted_line=%d\n",
                PROBE_POSITIONS,
                predicted_line);
        return 1;
    }

    per_stream_size = (size_t)PROBE_POSITIONS * (size_t)LINE_SIZE;
    if (per_stream_size < page_size) {
        per_stream_size = page_size;
    }
    if (per_stream_size % page_size) {
        per_stream_size += page_size - (per_stream_size % page_size);
    }

    if (predicted_offset + LINE_SIZE > per_stream_size ||
        last_trigger_offset + LINE_SIZE > per_stream_size) {
        fprintf(stderr,
                "stream offsets exceed stream buffer: predicted_offset=%lu last_trigger_offset=%lu probe_positions=%d stream_size=%lu\n",
                (unsigned long)predicted_offset,
                (unsigned long)last_trigger_offset,
                PROBE_POSITIONS,
                (unsigned long)per_stream_size);
        return 1;
    }

    size_t span_pages = ((size_t)(entries - 1) *
                         (size_t)page_step_pages) +
                        (per_stream_size / page_size);
    page_buffer_size = span_pages * page_size;

    page_buffer = map_data_buffer(buffer_addr,
                                  page_buffer_size,
                                  "afterimage stream buffer");
    if (!page_buffer) {
        return 1;
    }
    memset(page_buffer, -1, page_buffer_size);

    dummy_buffer_size = page_size * DUMMY_BUFFER_PAGES;
    dummy_buffer = mmap(NULL,
                        dummy_buffer_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1,
                        0);
    if (dummy_buffer == MAP_FAILED) {
        fprintf(stderr,
                "failed to map dummy buffer: %s\n",
                strerror(errno));
        return 1;
    }
    memset(dummy_buffer, -1, dummy_buffer_size);

    access_gadget_f access_gadget = map_access_gadget(access_pc);
    if (!access_gadget) {
        return 1;
    }

    printf("# %s-stride AfterImage-style fixed-N predicted-line test\n", access_name());
    printf("# access mode: %s (%s), same fixed PC for all streams\n",
           access_name(),
           access_instruction());
    printf("# stride=%d rounds=%d page_size=%lu access_pc=0x%016lx buffer=0x%016lx\n",
           stride,
           rounds,
           (unsigned long)page_size,
           (unsigned long)access_pc,
           (unsigned long)buffer_addr);
    printf("# STRIDE_LINES=%d TRAIN_ACCESSES=%d TRIGGER_ACCESSES=%d PROBE_POSITIONS=%d entries=%d page_step_pages=%d\n",
           STRIDE_LINES,
           TRAIN_ACCESSES,
           TRIGGER_ACCESSES,
           PROBE_POSITIONS,
           entries,
           page_step_pages);
    printf("# stream_size=%lu predicted_line=%d predicted_offset=%lu\n",
           (unsigned long)per_stream_size,
           predicted_line,
           (unsigned long)predicted_offset);
    printf("# train N streams, trigger N streams, then probe one stream predicted line per round\n");
    printf("n\tpage_index\tpredicted_line\toffset_bytes\tavg_ns\tprobes\n");

    run_predicted_lines(access_gadget,
                        entries,
                        page_step_pages,
                        stride,
                        predicted_line,
                        rounds);

    return 0;
}
