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
#define DEFAULT_VICTIM_BUFFER_ADDR 0x600000000ull
#define DEFAULT_MAX_COMPETITORS 512
#define DEFAULT_ROUNDS 1000

/*
 * access-stride:
 *   train:   line 0, 5, ...
 *   trigger: the next TRIGGER_ACCESSES stride positions
 *   probe:   the next stride position after trigger
 */
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

#ifndef COMPETITOR_ACCESSES
#define COMPETITOR_ACCESSES (TRAIN_ACCESSES)
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#define VICTIM_TRIGGER0_LINE (TRAIN_ACCESSES * STRIDE_LINES)
#define VICTIM_LAST_TRIGGER_LINE ((TRAIN_ACCESSES + TRIGGER_ACCESSES - 1) * STRIDE_LINES)
#define VICTIM_PROBE_LINE ((TRAIN_ACCESSES + TRIGGER_ACCESSES) * STRIDE_LINES)


#define DEFAULT_COMPETITOR_PAGE_STEP 1

#define MAX_MAPPED_REGIONS 1024

typedef void (*access_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};

static uint8_t *victim_buffer;
static uint8_t *competitor_buffer;
static uint8_t *dummy_buffer;

static size_t page_size;
static size_t dummy_buffer_size;
static size_t competitor_buffer_size;
static size_t victim_buffer_size;

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

static inline __attribute__((always_inline)) void competitor_access(void *addr) {
#if TRAIN_ACCESS_LOAD
    mLoad_inline(addr);
#else
    mStore_inline(addr);
#endif
}

static void dummy_accesses(void) {
    // for (uint64_t i = 0; i < dummy_buffer_size; i += LINE_SIZE) {
    //     maccess(&dummy_buffer[i]);
    // }
   for(uint32_t j = 0; j < dummy_buffer_size; j+=64){
        // asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
    }
}

static void flush_dummy_buffer(void) {
    for (uint64_t i = 0; i < dummy_buffer_size; i += LINE_SIZE) {
        flush_line_addr(&dummy_buffer[i]);
    }

    // mfence();
}

static void delay_after_trigger(void) {
    for (int k = 0; k < 100; k++) {
        maccess(&array1[k * LINE_SIZE]);
    }

    for (int i = 0; i < 100; i++) {
        nop();
    }

    // mfence();
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t time1 = timestamp();
    // maccess(addr);
    mStore_inline(addr);
    uint64_t time2 = timestamp();

    return time2 - time1;
}

static uint8_t *competitor_page(int index, int page_step_pages) {
    return competitor_buffer +
           ((size_t)index * (size_t)page_step_pages * page_size);
}

static void flush_victim_lines(void) {
    for (size_t offset = 0; offset < victim_buffer_size; offset += LINE_SIZE) {
        flush_line_addr(victim_buffer + offset);
    }
}

static void flush_competitor_lines(int competitor_count,
                                   int page_step_pages,
                                   int stride) {
    for (int i = 0; i < competitor_count; i++) {
        uint8_t *page = competitor_page(i, page_step_pages);

        for (int step = 0; step < COMPETITOR_ACCESSES; step++) {
            flush_line_addr(page + 3 * LINE_SIZE +
                            ((uint64_t)step * (uint64_t)stride));
        }
    }
}

static void train_victim(access_gadget_f access_gadget, int stride) {
    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        access_gadget(victim_buffer +
                      ((uint64_t)step * (uint64_t)stride));
    }
}

static void train_competitors(access_gadget_f access_gadget,
                              int competitor_count,
                              int page_step_pages,
                              int stride) {
    (void)access_gadget;

    for (int i = 0; i < competitor_count; i++) {
        uint8_t *page = competitor_page(i, page_step_pages);

        for (int step = 0; step < COMPETITOR_ACCESSES; step++) {
            // mLoad_noinline(page + 3 * LINE_SIZE +
            //                   ((uint64_t)step * (uint64_t)stride));
            competitor_access(page + 3 * LINE_SIZE +
                              ((uint64_t)step * (uint64_t)stride));
            // access_gadget(page + 3 * LINE_SIZE +
            //              ((uint64_t)step * (uint64_t)stride));
        }
    }
}

static uint64_t run_one_round(access_gadget_f access_gadget,
                              int competitor_count,
                              int page_step_pages,
                              int do_trigger,
                              int stride,
                              int probe_line) {
    uint8_t *probe_addr =
        victim_buffer + ((uint64_t)probe_line * (uint64_t)LINE_SIZE);

    // flush_dummy_buffer();
    // mfence();
    
    // dummy_accesses();
    // cpp_rctx();
    
    flush_victim_lines();
    flush_competitor_lines(competitor_count, page_step_pages, stride);
    
    

    train_victim(access_gadget, stride);

    train_competitors(access_gadget,
                      competitor_count,
                      page_step_pages,
                      stride);
    
    // cpp_rctx();
    // flush_victim_lines();
    if (do_trigger) {
        for (int index = 0; index < TRIGGER_ACCESSES; index++) {
            access_gadget(victim_buffer +
                         ((uintptr_t)(TRAIN_ACCESSES + index) *
                          (uintptr_t)stride));
        }
    }

    // mfence();
    /*
     * 4. 等待可能的预取完成
     */
    // delay_after_trigger();
    /*
     * 5. probe one victim candidate line.
     */
    return probe_latency(probe_addr);
}

static const char *probe_role(int probe_line, int predicted_line) {
    int stride_lines = STRIDE_LINES;

    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        if (probe_line == step * stride_lines) {
            return "trained";
        }
    }

    if (probe_line >= VICTIM_TRIGGER0_LINE &&
        probe_line <= VICTIM_LAST_TRIGGER_LINE &&
        ((probe_line - VICTIM_TRIGGER0_LINE) % stride_lines) == 0) {
        return "trigger";
    }

    if (probe_line == predicted_line) {
        return "predicted";
    }

    return "candidate";
}

static void run_probe_map(access_gadget_f access_gadget,
                          int competitor_count,
                          int page_step_pages,
                          int do_trigger,
                          int stride,
                          int predicted_line,
                          int rounds) {
    uint64_t latency_sum[PROBE_POSITIONS] = {0};
    int probe_count[PROBE_POSITIONS] = {0};

    for (int r = 0; r < rounds; r++) {
        int probe_line = r % PROBE_POSITIONS;

        latency_sum[probe_line] += run_one_round(access_gadget,
                                                 competitor_count,
                                                 page_step_pages,
                                                 do_trigger,
                                                 stride,
                                                 probe_line);
        probe_count[probe_line]++;
    }

    for (int probe_line = 0; probe_line < PROBE_POSITIONS; probe_line++) {
        uint64_t avg = 0;

        if (probe_count[probe_line] > 0) {
            avg = latency_sum[probe_line] / (uint64_t)probe_count[probe_line];
        }

        printf("%d\t%d\t%d\t%s\t%lu\t%d\n",
               competitor_count,
               probe_line,
               probe_line * LINE_SIZE,
               probe_role(probe_line, predicted_line),
               (unsigned long)avg,
               probe_count[probe_line]);
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [access_pc victim_buffer_addr max_competitors rounds [page_step_pages]]\n"
            "default: access_pc=0x%lx victim_buffer_addr=0x%lx "
            "max_competitors=%d rounds=%d page_step_pages=%d\n",
            prog,
            (unsigned long)DEFAULT_ACCESS_PC,
            (unsigned long)DEFAULT_VICTIM_BUFFER_ADDR,
            DEFAULT_MAX_COMPETITORS,
            DEFAULT_ROUNDS,
            DEFAULT_COMPETITOR_PAGE_STEP);
}

int main(int argc, char **argv) {
    uintptr_t access_pc = DEFAULT_ACCESS_PC;
    uintptr_t victim_buffer_addr = DEFAULT_VICTIM_BUFFER_ADDR;

    int max_competitors = DEFAULT_MAX_COMPETITORS;
    int rounds = DEFAULT_ROUNDS;
    int page_step_pages = DEFAULT_COMPETITOR_PAGE_STEP;
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
        victim_buffer_addr = strtoull(argv[2], NULL, 0);
        max_competitors = atoi(argv[3]);
        rounds = atoi(argv[4]);
    }

    if (argc == 6) {
        page_step_pages = atoi(argv[5]);
    }

    if (max_competitors < 0 ||
        max_competitors > 4096 ||
        rounds <= 0 ||
        page_step_pages <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    if ((victim_buffer_addr % page_size) != 0) {
        fprintf(stderr,
                "warning: victim_buffer_addr is not page-aligned: 0x%016lx\n",
                (unsigned long)victim_buffer_addr);
    }

    if (TRIGGER_ACCESSES < 1 || TRIGGER_ACCESSES > 2) {
        fprintf(stderr, "TRIGGER_ACCESSES must be 1 or 2\n");
        return 1;
    }

    uint64_t predicted_offset =
        (uint64_t)VICTIM_PROBE_LINE * (uint64_t)LINE_SIZE;

    uint64_t last_trigger_offset =
        (uint64_t)VICTIM_LAST_TRIGGER_LINE * (uint64_t)LINE_SIZE;

    int predicted_line = VICTIM_PROBE_LINE;

    if (PROBE_POSITIONS <= 0 || predicted_line >= PROBE_POSITIONS) {
        fprintf(stderr,
                "PROBE_POSITIONS=%d must include predicted_line=%d\n",
                PROBE_POSITIONS,
                predicted_line);
        return 1;
    }

    victim_buffer_size = (size_t)PROBE_POSITIONS * (size_t)LINE_SIZE;
    if (victim_buffer_size < page_size) {
        victim_buffer_size = page_size;
    }
    if (victim_buffer_size % page_size) {
        victim_buffer_size += page_size - (victim_buffer_size % page_size);
    }

    if (predicted_offset + LINE_SIZE > victim_buffer_size ||
        last_trigger_offset + LINE_SIZE > victim_buffer_size) {
        fprintf(stderr,
                "victim offsets exceed victim buffer: predicted_offset=%lu last_trigger_offset=%lu probe_positions=%d victim_size=%lu\n",
                (unsigned long)predicted_offset,
                (unsigned long)last_trigger_offset,
                PROBE_POSITIONS,
                (unsigned long)victim_buffer_size);
        return 1;
    }

    /*
     * victim page
     */
    victim_buffer = map_data_buffer(victim_buffer_addr,
                                    victim_buffer_size,
                                    "victim buffer");

    if (!victim_buffer) {
        return 1;
    }

    memset(victim_buffer, -1, victim_buffer_size);

    /*
     * competitor pages.
     *
     * 如果 page_step_pages > 1，则中间会有 unused pages；
     * 这允许你测试不同 page spacing 对替换/冲突的影响。
     */
    size_t competitor_span_pages = 1;

    if (max_competitors > 0) {
        competitor_span_pages =
            ((size_t)(max_competitors - 1) *
             (size_t)page_step_pages) + 1;
    }

    competitor_buffer_size = competitor_span_pages * page_size;

    competitor_buffer = mmap(NULL,
                             competitor_buffer_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                             -1,
                             0);

    if (competitor_buffer == MAP_FAILED) {
        fprintf(stderr,
                "failed to map competitor buffer: %s\n",
                strerror(errno));
        return 1;
    }

    memset(competitor_buffer, -1, competitor_buffer_size);

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

    printf("# %s-stride entry-capacity test\n", access_name());
    printf("# access mode: %s (%s), same fixed PC for victim train and trigger\n",
           access_name(),
           access_instruction());
    printf("# stride=%d rounds=%d page_size=%lu access_pc=0x%016lx victim_buffer=0x%016lx\n",
           stride,
           rounds,
           (unsigned long)page_size,
           (unsigned long)access_pc,
           (unsigned long)victim_buffer_addr);

    printf("# STRIDE_LINES=%d TRAIN_ACCESSES=%d TRIGGER_ACCESSES=%d COMPETITOR_ACCESSES=%d PROBE_POSITIONS=%d max_competitors=%d page_step_pages=%d\n",
           STRIDE_LINES,
           TRAIN_ACCESSES,
           TRIGGER_ACCESSES,
           COMPETITOR_ACCESSES,
           PROBE_POSITIONS,
           max_competitors,
           page_step_pages);
    printf("# victim_buffer_size=%lu\n",
           (unsigned long)victim_buffer_size);

    printf("# victim accesses=%d train_only_accesses=%d trigger_accesses=%d stride_lines=%d\n",
           TRAIN_ACCESSES + TRIGGER_ACCESSES,
           TRAIN_ACCESSES,
           TRIGGER_ACCESSES,
           STRIDE_LINES);
    printf("# victim trigger lines: %d..%d\n",
           VICTIM_TRIGGER0_LINE,
           VICTIM_LAST_TRIGGER_LINE);
    printf("# victim probe/predicted line: %lu\n",
           (unsigned long)(predicted_offset / LINE_SIZE));

    printf("# competitor accesses per page: %d %ss, stride_lines=%d\n",
           COMPETITOR_ACCESSES,
           access_name(),
           STRIDE_LINES);
    printf("# lower latency means that victim line was more likely prefetched\n");

    printf("# no-trigger calibration follows as n=-1 rows\n");
    printf("n\tprobe_line\toffset_bytes\trole\tavg_ns\tprobes\n");

    run_probe_map(access_gadget,
                  -1,
                  page_step_pages,
                  0,
                  stride,
                  predicted_line,
                  rounds);

    for (int n = 0; n <= max_competitors; n++) {
        run_probe_map(access_gadget,
                      n,
                      page_step_pages,
                      1,
                      stride,
                      predicted_line,
                      rounds);
    }

    return 0;
}
