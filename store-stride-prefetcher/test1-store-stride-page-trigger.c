#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "until.h"

#define BUFFER_PAGES 128
#define DUMMY_BUFFER_PAGES 10

#define DEFAULT_STORE_PC 0x500000120ull
#define DEFAULT_VICTIM_BUFFER_ADDR 0x600000000ull
#define DEFAULT_ROUNDS 1000

/*
 * 训练序列：
 *   line 0, 5, 10, 15, 20
 *
 * 预测目标：
 *   line 25
 */
#define DEFAULT_STRIDE (5 * LINE_SIZE)
#define TRAIN_ACCESSES 5

/*
 * 4KB page 内有 64 条 64B cache line。
 */
#define PAGE_LINES 64

/*
 * probe line = TRAIN_ACCESSES * stride / LINE_SIZE
 * 默认为 5 * 320 / 64 = 25。
 */
#define PREDICTED_LINE ((TRAIN_ACCESSES * DEFAULT_STRIDE) / LINE_SIZE)

/*
 * 为了避免 same-page trigger 直接 store 到 probe line，
 * same-page sweep 中会跳过 PREDICTED_LINE。
 */
#define SKIP_DIRECT_PROBE_TRIGGER 1

#define MAX_MAPPED_REGIONS 2048

/*
 * 如果你校准 threshold 时每次 store 后有 DSB/ISB，可以改成 1。
 * 默认 0 更接近连续 store stream。
 */
#define FENCE_AFTER_EACH_STORE 0

typedef void (*store_gadget_f)(void *);

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

extern char _store_gadget_asm_start[];
extern char _store_gadget_asm_end[];

/*
 * Store gadget:
 *
 *   x0 = target address
 *   strb w0, [x0]
 *   ret
 *
 * 写入值不重要，关键是这是一条 PC 可控的 store 指令。
 */
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

/*
 * 支持重复/部分重叠映射。
 * requested_address 如果落在已经由本测试映射过的区域里，就复用。
 */
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

static int touch_region(uint8_t *base, size_t size) {
    if (!base) {
        return -1;
    }

    for (size_t off = 0; off < size; off += page_size) {
        maccess(base + off);
    }

    return 0;
}

/*
 * 一轮实验：
 *
 * 前 5 次 store:
 *   train_base + 0 * stride
 *   train_base + 1 * stride
 *   train_base + 2 * stride
 *   train_base + 3 * stride
 *   train_base + 4 * stride
 *
 * 第 6 次 store:
 *   trigger_addr
 *
 * probe:
 *   probe_addr，通常是 train_base + 5 * stride
 */
static uint64_t run_one_round(store_gadget_f store_gadget,
                              uint8_t *train_base,
                              uint8_t *trigger_addr,
                              int do_trigger,
                              uint8_t *probe_addr,
                              int stride) {
    dummy_accesses();

    /*
     * 每轮只 flush 与本轮相关的 cache line。
     * 避免整段 flush 太慢，也避免跨 page 测试混入不必要的访问。
     */
    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        flush_line_addr(train_base +
                        ((uint64_t)step * (uint64_t)stride));
    }

    if (do_trigger && trigger_addr) {
        flush_line_addr(trigger_addr);
    }

    flush_line_addr(probe_addr);
    // mfence();

    /*
     * 前 5 次 fixed-stride store 训练。
     */
    for (int step = 0; step < TRAIN_ACCESSES; step++) {
        store_gadget(train_base +
                     ((uint64_t)step * (uint64_t)stride));

#if FENCE_AFTER_EACH_STORE
        mfence();
#endif
    }

    /*
     * 第 6 次 trigger store。
     * 它不需要遵循 stride；本测试就是要验证它是否只要同 page 即可。
     */
    if (do_trigger && trigger_addr) {
        store_gadget(trigger_addr);

#if FENCE_AFTER_EACH_STORE
        mfence();
#endif
    }

    // mfence();

    delay_after_trigger();

    return probe_latency(probe_addr);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [store_pc victim_buffer_addr rounds]\n"
            "default: store_pc=0x%lx victim_buffer_addr=0x%lx rounds=%d\n",
            prog,
            (unsigned long)DEFAULT_STORE_PC,
            (unsigned long)DEFAULT_VICTIM_BUFFER_ADDR,
            DEFAULT_ROUNDS);
}

/*
 * Test 1:
 * 同一 4KB page 内，扫描所有 trigger line。
 *
 * 训练 page:
 *   victim page
 *
 * trigger:
 *   victim page 内 line 0..63
 *
 * probe:
 *   victim + 5 * stride，即默认 line 25。
 *
 * 预期：
 *   除了直接访问 probe line 的 trigger_line=25 要跳过之外，
 *   如果“同 page 任意 store 都能触发”，大多数 line 应接近 same_page baseline。
 */
static void run_same_page_line_sweep(store_gadget_f store_gadget,
                                     int rounds,
                                     int stride) {
    uint8_t *train_base = victim_buffer;
    uint8_t *probe_addr =
        train_base + ((uint64_t)TRAIN_ACCESSES * (uint64_t)stride);

    printf("\n");
    printf("## same-page trigger-line sweep\n");
    printf("# training page = victim page\n");
    printf("# training lines: 0,5,10,15,20 when stride=5 lines\n");
    printf("# probe line = %d\n", PREDICTED_LINE);
    printf("# trigger lines scan: 0..63, skip probe line if enabled\n");
    printf("trigger_page_delta\ttrigger_line\tlatency_ns\n");

    /*
     * no_trigger baseline
     */
    {
        uint64_t latency = 0;
        uint8_t *dummy_trigger = train_base;

        for (int r = 0; r < rounds; r++) {
            latency += run_one_round(store_gadget,
                                     train_base,
                                     dummy_trigger,
                                     0,
                                     probe_addr,
                                     stride);
        }

        printf("no_trigger\t-1\t%lu\n",
               (unsigned long)(latency / (uint64_t)rounds));
    }

    for (int line = 0; line < PAGE_LINES; line++) {
#if SKIP_DIRECT_PROBE_TRIGGER
        if (line == PREDICTED_LINE) {
            printf("0\t%d\tSKIP_DIRECT_PROBE\n", line);
            continue;
        }
#endif

        uint8_t *trigger_addr =
            train_base + ((uintptr_t)line * (uintptr_t)LINE_SIZE);

        uint64_t latency = 0;

        for (int r = 0; r < rounds; r++) {
            latency += run_one_round(store_gadget,
                                     train_base,
                                     trigger_addr,
                                     1,
                                     probe_addr,
                                     stride);
        }

        printf("0\t%d\t%lu\n",
               line,
               (unsigned long)(latency / (uint64_t)rounds));
    }
}

/*
 * Test 2:
 * 不同 4KB page 内，扫描多个 page_delta 和所有 trigger line。
 *
 * 训练 page:
 *   victim page
 *
 * trigger:
 *   victim page + page_delta
 *   page_delta 覆盖多个正负 page，不只测一个位置。
 *
 * probe:
 *   victim + 5 * stride。
 *
 * 预期：
 *   如果 trigger 必须和训练在同一个 4KB page，
 *   那么所有 page_delta != 0 的 trigger_line 都应该接近 no_trigger。
 */
static void run_cross_page_line_sweep(store_gadget_f store_gadget,
                                      uintptr_t victim_buffer_addr,
                                      int rounds,
                                      int stride) {
    uint8_t *train_base = victim_buffer;
    uint8_t *probe_addr =
        train_base + ((uint64_t)TRAIN_ACCESSES * (uint64_t)stride);

    /*
     * 覆盖近邻 page、较远 page、正负方向。
     * +1/+2/+4 等在 victim_buffer 映射范围内；
     * 负 page 和更远 page 会通过 map_data_buffer 单独映射。
     */
    static const int page_deltas[] = {
        -64, -32, -16, -8, -4, -2, -1,
         1,   2,   4,  8, 16, 32, 64
    };

    int num_page_deltas =
        (int)(sizeof(page_deltas) / sizeof(page_deltas[0]));

    printf("\n");
    printf("## cross-page trigger page/line sweep\n");
    printf("# training page = victim page\n");
    printf("# trigger page = victim page + page_delta, page_delta != 0\n");
    printf("# for each page_delta, trigger lines scan: 0..63\n");
    printf("# probe remains victim + 5 * stride, line %d\n", PREDICTED_LINE);
    printf("trigger_page_delta\ttrigger_line\tlatency_ns\n");

    for (int p = 0; p < num_page_deltas; p++) {
        int page_delta = page_deltas[p];

        uintptr_t trigger_page_addr =
            victim_buffer_addr +
            ((intptr_t)page_delta * (intptr_t)page_size);

        uint8_t *trigger_page =
            map_data_buffer(trigger_page_addr,
                            page_size,
                            "cross-page trigger page");

        if (!trigger_page) {
            for (int line = 0; line < PAGE_LINES; line++) {
                printf("%d\t%d\t-1\n", page_delta, line);
            }
            continue;
        }

        /*
         * fault in trigger page
         */
        memset(trigger_page, -1, page_size);
        touch_region(trigger_page, page_size);

        for (int line = 0; line < PAGE_LINES; line++) {
            uint8_t *trigger_addr =
                trigger_page + ((uintptr_t)line * (uintptr_t)LINE_SIZE);

            uint64_t latency = 0;

            for (int r = 0; r < rounds; r++) {
                latency += run_one_round(store_gadget,
                                         train_base,
                                         trigger_addr,
                                         1,
                                         probe_addr,
                                         stride);
            }

            printf("%d\t%d\t%lu\n",
                   page_delta,
                   line,
                   (unsigned long)(latency / (uint64_t)rounds));
        }
    }
}

/*
 * Test 3:
 * 训练 page 本身也移动到不同 page。
 *
 * 目的：
 *   验证“预取目标跟随训练 page”。
 *
 * 对每个 train_page_delta：
 *   前 5 次 store 在 train_page_delta 的 page 内；
 *   第 6 次 trigger 在同一个 train page 内扫描多个 line；
 *   probe train_page + 5 * stride。
 *
 * 预期：
 *   如果模型正确，不管 train page 是 victim+1、victim+64 还是 victim-64，
 *   只要 trigger 也在该 train page 内，就应该能预取 train_next。
 */
static void run_train_page_follow_sweep(store_gadget_f store_gadget,
                                        uintptr_t victim_buffer_addr,
                                        int rounds,
                                        int stride) {
    static const int train_page_deltas[] = {
        -64, -16, -4, -1, 0, 1, 4, 16, 64
    };

    /*
     * 这里不扫全部 64 line，避免输出太大。
     * 但覆盖 page 内多个位置，包括训练线附近、远离训练线、page 末尾。
     * 注意跳过 predicted line，避免直接 store 到 probe。
     */
    static const int trigger_lines[] = {
        0, 1, 2, 4, 5, 7, 10, 13, 20, 31, 40, 51, 63
    };

    int num_train_pages =
        (int)(sizeof(train_page_deltas) / sizeof(train_page_deltas[0]));

    int num_trigger_lines =
        (int)(sizeof(trigger_lines) / sizeof(trigger_lines[0]));

    printf("\n");
    printf("## train-page follow sweep\n");
    printf("# train page moves across different pages\n");
    printf("# trigger page is the same as train page\n");
    printf("# probe = train page + 5 * stride\n");
    printf("# this verifies prediction target follows the training page\n");
    printf("train_page_delta\ttrigger_line\tlatency_ns\n");

    for (int p = 0; p < num_train_pages; p++) {
        int train_page_delta = train_page_deltas[p];

        uintptr_t train_page_addr =
            victim_buffer_addr +
            ((intptr_t)train_page_delta * (intptr_t)page_size);

        uint8_t *train_page =
            map_data_buffer(train_page_addr,
                            victim_buffer_size + page_size,
                            "train page follow buffer");

        if (!train_page) {
            for (int i = 0; i < num_trigger_lines; i++) {
                printf("%d\t%d\t-1\n",
                       train_page_delta,
                       trigger_lines[i]);
            }
            continue;
        }

        memset(train_page, -1, victim_buffer_size);
        touch_region(train_page, victim_buffer_size);

        uint8_t *probe_addr =
            train_page + ((uint64_t)TRAIN_ACCESSES * (uint64_t)stride);

        for (int i = 0; i < num_trigger_lines; i++) {
            int line = trigger_lines[i];

#if SKIP_DIRECT_PROBE_TRIGGER
            if (line == PREDICTED_LINE) {
                printf("%d\t%d\tSKIP_DIRECT_PROBE\n",
                       train_page_delta,
                       line);
                continue;
            }
#endif

            uint8_t *trigger_addr =
                train_page + ((uintptr_t)line * (uintptr_t)LINE_SIZE);

            uint64_t latency = 0;

            for (int r = 0; r < rounds; r++) {
                latency += run_one_round(store_gadget,
                                         train_page,
                                         trigger_addr,
                                         1,
                                         probe_addr,
                                         stride);
            }

            printf("%d\t%d\t%lu\n",
                   train_page_delta,
                   line,
                   (unsigned long)(latency / (uint64_t)rounds));
        }
    }
}

int main(int argc, char **argv) {
    uintptr_t store_pc = DEFAULT_STORE_PC;
    uintptr_t victim_buffer_addr = DEFAULT_VICTIM_BUFFER_ADDR;
    int rounds = DEFAULT_ROUNDS;
    int stride = DEFAULT_STRIDE;

    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect OS page size\n");
        return 1;
    }

    page_size = (size_t)detected_page_size;

    if (page_size != 4096) {
        fprintf(stderr,
                "warning: page_size=%lu, this test assumes 4KB page organization\n",
                (unsigned long)page_size);
    }

    victim_buffer_size = page_size * BUFFER_PAGES;
    dummy_buffer_size = page_size * DUMMY_BUFFER_PAGES;

    if (argc != 1 && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc == 4) {
        store_pc = strtoull(argv[1], NULL, 0);
        victim_buffer_addr = strtoull(argv[2], NULL, 0);
        rounds = atoi(argv[3]);
    }

    if (rounds <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    uint64_t predicted_offset =
        (uint64_t)TRAIN_ACCESSES * (uint64_t)stride;

    if (predicted_offset + LINE_SIZE > victim_buffer_size) {
        fprintf(stderr,
                "victim buffer is too small: predicted_offset=%lu victim_size=%lu\n",
                (unsigned long)predicted_offset,
                (unsigned long)victim_buffer_size);
        return 1;
    }

    victim_buffer = map_data_buffer(victim_buffer_addr,
                                    victim_buffer_size + page_size,
                                    "victim buffer");

    if (!victim_buffer) {
        return 1;
    }

    memset(victim_buffer, -1, victim_buffer_size);
    touch_region(victim_buffer, victim_buffer_size);

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
    touch_region(dummy_buffer, dummy_buffer_size);

    store_gadget_f store_gadget = map_store_gadget(store_pc);
    if (!store_gadget) {
        return 1;
    }

    printf("# store-stride page-trigger organization test\n");
    printf("# stride=%d rounds=%d page_size=%lu store_pc=0x%016lx victim_buffer=0x%016lx\n",
           stride,
           rounds,
           (unsigned long)page_size,
           (unsigned long)store_pc,
           (unsigned long)victim_buffer_addr);

    printf("# TRAIN_ACCESSES=%d\n", TRAIN_ACCESSES);
    printf("# training sequence when stride=5 lines: line 0,5,10,15,20\n");
    printf("# predicted_offset=%lu predicted_line=%lu\n",
           (unsigned long)predicted_offset,
           (unsigned long)(predicted_offset / LINE_SIZE));
    printf("# lower latency means the probed line was more likely prefetched\n");
    printf("# FENCE_AFTER_EACH_STORE=%d\n", FENCE_AFTER_EACH_STORE);
    printf("# SKIP_DIRECT_PROBE_TRIGGER=%d\n", SKIP_DIRECT_PROBE_TRIGGER);

    run_same_page_line_sweep(store_gadget,
                             rounds,
                             stride);

    run_cross_page_line_sweep(store_gadget,
                              victim_buffer_addr,
                              rounds,
                              stride);

    run_train_page_follow_sweep(store_gadget,
                                victim_buffer_addr,
                                rounds,
                                stride);

    return 0;
}
