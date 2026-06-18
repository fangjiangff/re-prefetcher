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
#define DEFAULT_VICTIM_BUFFER_ADDR 0x600000000ull
#define DEFAULT_MAX_COMPETITORS 512
#define DEFAULT_ROUNDS 1000

/*
 * store-stride:
 *   line 0, 5, 10, 15, 20
 * predicted:
 *   line 25
 */
#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#define DEFAULT_STRIDE (STRIDE_LINES * LINE_SIZE)

#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 5
#endif

#ifndef COMPETITOR_ACCESSES
#define COMPETITOR_ACCESSES (TRAIN_ACCESSES)
#endif

#ifndef PROBE_LINES
#define PROBE_LINES 64
#endif

/*
 * victim trigger line:
 *   必须在同一个 4KB page 内；
 *   不能是训练 line: 0,5,10,15,20；
 *   不能是 probe line: 25。
 *
 * 你前面的测试里 line 31 比较稳定。
 */
#ifndef VICTIM_TRIGGER_LINE
#define VICTIM_TRIGGER_LINE 31
#endif

/*
 * competitor page 地址间隔，单位是 page。
 *
 * 默认 1 表示连续 page：
 *   competitor page 0, 1, 2, ...
 *
 * 如果你怀疑 prefetcher 是 set-associative，并且 page index 低位参与 set index，
 * 可以运行时把 page_step_pages 改大，例如 2,4,8,16,64。
 */
#define DEFAULT_COMPETITOR_PAGE_STEP 1

#define MAX_MAPPED_REGIONS 1024

/*
 * 默认不在每次 store 后加 DSB/ISB，更接近连续 store stream。
 * 如果你校准 threshold 时每次 store 后有 fence，可以改成 1。
 */
#define FENCE_AFTER_EACH_STORE 0

/*
 * 如果担心 competitor 的数据 cache footprint 干扰 probe，
 * 可以改成 1：在 competitor 训练后、victim trigger 前 flush competitor lines。
 *
 * 注意：这可能也改变实验时序，因此默认 0。
 */
#define FLUSH_COMPETITORS_AFTER_TRAIN 0

typedef void (*store_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};

static uint8_t *victim_buffer;
static uint8_t *competitor_buffer;
static uint8_t *dummy_buffer;

static size_t page_size;
static size_t dummy_buffer_size;
static size_t competitor_buffer_size;

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

    __builtin___clear_cache((char *)code,
                            (char *)(code + gadget_size));

    return (store_gadget_f)(void *)code;
}

static void dummy_accesses(void) {
    for (uint64_t i = 0; i < dummy_buffer_size; i += LINE_SIZE) {
        maccess(&dummy_buffer[i]);
    }

    // mfence();
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
    maccess(addr);
    uint64_t time2 = timestamp();

    return time2 - time1;
}

static uint8_t *competitor_page(int index, int page_step_pages) {
    return competitor_buffer +
           ((size_t)index * (size_t)page_step_pages * page_size);
}

static void flush_victim_lines(void) {
    for (size_t offset = 0; offset < page_size; offset += LINE_SIZE) {
        flush_line_addr(victim_buffer + offset);
    }
}

static void flush_competitor_lines(int competitor_count,
                                   int page_step_pages,
                                   int stride) {
    for (int i = 0; i < competitor_count; i++) {
        uint8_t *page = competitor_page(i, page_step_pages);

        for (int step = 0; step < COMPETITOR_ACCESSES; step++) {
            flush_line_addr(page +
                            ((uint64_t)step * (uint64_t)stride));
        }
    }
}

static void train_victim(store_gadget_f store_gadget, int stride) {
    (void)store_gadget;

    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        mStore_inline(victim_buffer +
                      ((uint64_t)step * (uint64_t)stride));
    }
}

static void train_competitors(store_gadget_f store_gadget,
                              int competitor_count,
                              int page_step_pages,
                              int stride) {
    (void)store_gadget;

    for (int i = 0; i < competitor_count; i++) {
        uint8_t *page = competitor_page(i, page_step_pages);

        for (int step = 0; step < COMPETITOR_ACCESSES; step++) {
            mLoad_inline(page + 3 * LINE_SIZE +
                         ((uint64_t)step * (uint64_t)stride));
        }
    }
}

static uint64_t run_one_round(store_gadget_f store_gadget,
                              int competitor_count,
                              int page_step_pages,
                              int do_trigger,
                              int stride,
                              int probe_line) {
    uint8_t *probe_addr =
        victim_buffer + ((uint64_t)probe_line * (uint64_t)LINE_SIZE);

    flush_dummy_buffer();
    mfence();
    dummy_accesses();
    

    flush_victim_lines();
    flush_competitor_lines(competitor_count, page_step_pages, stride);
    mfence();

    train_victim(store_gadget, stride);

    train_competitors(store_gadget,
                      competitor_count,
                      page_step_pages,
                      stride);

// #if FLUSH_COMPETITORS_AFTER_TRAIN
//     flush_competitor_lines(competitor_count, page_step_pages, stride);
//     // mfence();
// #endif

    /*
     * 3. victim trigger
     *    选择 victim page 内一个新 line，而不是 line 25。
     */
    if (do_trigger) {
        mStore_inline(victim_buffer +
                      ((uintptr_t)VICTIM_TRIGGER_LINE *
                       (uintptr_t)LINE_SIZE));

// #if FENCE_AFTER_EACH_STORE
//         mfence();
// #endif
    }

    // mfence();

    /*
     * 4. 等待可能的预取完成
     */
    delay_after_trigger();

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

    if (probe_line == VICTIM_TRIGGER_LINE) {
        return "trigger";
    }

    if (probe_line == predicted_line) {
        return "predicted";
    }

    return "candidate";
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [store_pc victim_buffer_addr max_competitors rounds [page_step_pages]]\n"
            "default: store_pc=0x%lx victim_buffer_addr=0x%lx "
            "max_competitors=%d rounds=%d page_step_pages=%d\n",
            prog,
            (unsigned long)DEFAULT_STORE_PC,
            (unsigned long)DEFAULT_VICTIM_BUFFER_ADDR,
            DEFAULT_MAX_COMPETITORS,
            DEFAULT_ROUNDS,
            DEFAULT_COMPETITOR_PAGE_STEP);
}

int main(int argc, char **argv) {
    uintptr_t store_pc = DEFAULT_STORE_PC;
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
        store_pc = strtoull(argv[1], NULL, 0);
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

    uint64_t predicted_offset =
        (uint64_t)TRAIN_ACCESSES * (uint64_t)stride;

    uint64_t trigger_offset =
        (uint64_t)VICTIM_TRIGGER_LINE * (uint64_t)LINE_SIZE;

    if (predicted_offset + LINE_SIZE > page_size ||
        trigger_offset + LINE_SIZE > page_size ||
        (uint64_t)PROBE_LINES * (uint64_t)LINE_SIZE > page_size) {
        fprintf(stderr,
                "victim offsets exceed one page: predicted_offset=%lu trigger_offset=%lu probe_lines=%d page_size=%lu\n",
                (unsigned long)predicted_offset,
                (unsigned long)trigger_offset,
                PROBE_LINES,
                (unsigned long)page_size);
        return 1;
    }

    int predicted_line = TRAIN_ACCESSES * STRIDE_LINES;

    /*
     * victim page
     */
    victim_buffer = map_data_buffer(victim_buffer_addr,
                                    page_size,
                                    "victim buffer");

    if (!victim_buffer) {
        return 1;
    }

    memset(victim_buffer, -1, page_size);

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

    store_gadget_f store_gadget = map_store_gadget(store_pc);
    if (!store_gadget) {
        return 1;
    }

    printf("# store-stride entry-capacity test\n");
    printf("# stride=%d rounds=%d page_size=%lu store_pc=0x%016lx victim_buffer=0x%016lx\n",
           stride,
           rounds,
           (unsigned long)page_size,
           (unsigned long)store_pc,
           (unsigned long)victim_buffer_addr);

    printf("# STRIDE_LINES=%d TRAIN_ACCESSES=%d COMPETITOR_ACCESSES=%d PROBE_LINES=%d max_competitors=%d page_step_pages=%d\n",
           STRIDE_LINES,
           TRAIN_ACCESSES,
           COMPETITOR_ACCESSES,
           PROBE_LINES,
           max_competitors,
           page_step_pages);

    printf("# victim training: %d stores, stride_lines=%d\n",
           TRAIN_ACCESSES,
           STRIDE_LINES);
    printf("# victim trigger line: %d\n", VICTIM_TRIGGER_LINE);
    printf("# victim probe/predicted line: %lu\n",
           (unsigned long)(predicted_offset / LINE_SIZE));

    printf("# competitor accesses per page: %d loads, stride_lines=%d\n",
           COMPETITOR_ACCESSES,
           STRIDE_LINES);
    printf("# lower latency means that victim line was more likely prefetched\n");
    printf("# FENCE_AFTER_EACH_STORE=%d\n", FENCE_AFTER_EACH_STORE);
    printf("# FLUSH_COMPETITORS_AFTER_TRAIN=%d\n", FLUSH_COMPETITORS_AFTER_TRAIN);

    /*
     * no-trigger calibration:
     * victim trained, no competitors, but no victim trigger.
     */
    {
        uint64_t latency = 0;

        for (int r = 0; r < rounds; r++) {
            latency += run_one_round(store_gadget,
                                     0,
                                     page_step_pages,
                                     0,
                                     stride,
                                     predicted_line);
        }

        printf("# no_trigger_n0_latency_ns=%lu\n",
               (unsigned long)(latency / (uint64_t)rounds));
    }

    printf("n\tprobe_line\toffset_bytes\trole\tavg_ns\tprobes\n");

    for (int n = 5; n <= max_competitors; n++) {
         uint64_t latency = 0;
        for (int probe_line = 0; probe_line < PROBE_LINES; probe_line++) {
        int rnd_probe_line = (probe_line * 73) % PROBE_LINES;
           latency = 0;
            for (int r = 0; r < rounds; r++) {
                latency += run_one_round(store_gadget,
                                         n,
                                         page_step_pages,
                                         0,
                                         stride,
                                         rnd_probe_line);
            }
            if(rnd_probe_line == 25){
            // if(n == 7 || n == 8){
                printf("%d\t%d\t%d\t%s\t%lu\t%lu\n",
                    n,
                    rnd_probe_line,
                    rnd_probe_line * LINE_SIZE,
                    probe_role(rnd_probe_line, predicted_line),
                    (unsigned long)(latency / (uint64_t)rounds),
                    (unsigned long)rounds);
            }
        }
        // if(max_competitors == 7 || max_competitors == 8){
        //     for(int k=0;k<64;k++){
        //            printf("%d\t%d\t%d\t%s\t%lu\t%lu\n",
        //             n,
        //             k,
        //             k * LINE_SIZE,
        //             probe_role(k, predicted_line),
        //             (unsigned long)(latency / (uint64_t)rounds),
        //             (unsigned long)rounds);
        //     }
        // }
    }

    return 0;
}
