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

/*
 * 对 memory/address collision 测试来说，bit 0..47 都可以扫。
 * 但解释低 bit 时要注意：
 *   bit 0..5 是 cache line 内 offset；
 *   bit 6 以上才开始影响 cache line address。
 */
#define DEFAULT_MIN_DIFF_BIT 0
#define DEFAULT_MAX_DIFF_BIT 47
#define DEFAULT_ROUNDS 1000

/*
 * 你原始代码使用 30 cache lines。
 */
#define DEFAULT_STRIDE (5 * LINE_SIZE)

/*
 * 已知第 6 次 store 触发 store-stride prefetcher。
 *
 * 1-based:
 *   第 1~5 次 store: training
 *   第 6 次 store:   trigger
 *   第 7 个位置:     probe
 *
 * 0-based:
 *   training index: 0,1,2,3,4
 *   trigger index:  5
 *   probe index:    6
 */
#define STORE_TRIGGER_ACCESS 6

#define MAX_MAPPED_REGIONS 512

/*
 * 如果你之前校准“第 6 次 store 触发”时每次 store 后都有 DSB/ISB，
 * 可以把这里改成 1。
 *
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
 * 支持部分区域已经被本测试映射过的情况。
 * 这对 diff_bit 较小时很重要，因为 colliding_buffer_addr
 * 可能落在 victim_buffer 已经映射的范围内。
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

static store_gadget_f map_store_gadget(uintptr_t address) {
    size_t gadget_size =
        (size_t)(_store_gadget_asm_end - _store_gadget_asm_start);

    uint8_t *code = map_fixed_region(address,
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

static uint8_t *map_data_buffer(uintptr_t address,
                                size_t size,
                                const char *name) {
    return (uint8_t *)map_fixed_region(address,
                                       size,
                                       PROT_READ | PROT_WRITE,
                                       name);
}

static void flush_victim_buffer(void) {
    for (uint64_t offset = 0;
         offset < victim_buffer_size;
         offset += LINE_SIZE) {
        flush(&victim_buffer[offset]);
    }

    // mfence();
}

static void dummy_accesses(void) {
    for (uint64_t i = 0; i < dummy_buffer_size; i += LINE_SIZE) {
        maccess(&dummy_buffer[i]);
    }

    // mfence();
}

static void delay_after_trigger(void) {
    /*
     * 给预取请求一点完成时间。
     * array1 访问用于制造轻微延迟，避免纯 nop 过于短。
     */
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

/*
 * do_trigger = 1:
 *   前 5 次 store 使用 train_buffer
 *   第 6 次 store 使用 victim_buffer
 *   probe victim_buffer 第 7 个位置
 *
 * do_trigger = 0:
 *   只做前 5 次 store，不做第 6 次 trigger
 *   仍然 probe victim_buffer 第 7 个位置
 *
 * 地址 collision 测试的核心：
 *
 *   train_buffer 可以是：
 *     victim_buffer
 *     victim_buffer_addr ^ (1 << diff_bit)
 *
 *   trigger 永远是：
 *     victim_buffer + 5 * stride
 *
 *   probe 永远是：
 *     victim_buffer + 6 * stride
 */
static uint64_t run_one_round(store_gadget_f store_gadget,
                              uint8_t *train_buffer,
                              int do_trigger,
                              int stride) {
    dummy_accesses();
    flush_victim_buffer();

    /*
     * 第 1~5 次 store: training
     * 0-based index: 0,1,2,3,4
     */
    for (int step = 0; step < STORE_TRIGGER_ACCESS - 1; step++) {
        store_gadget(train_buffer +
                     ((uint64_t)step * (uint64_t)stride));

#if FENCE_AFTER_EACH_STORE
        mfence();
#endif
    }

    /*
     * 第 6 次 store: trigger
     * 始终回到 victim_buffer。
     * 0-based index: 5
     */
    if (do_trigger) {
        store_gadget(victim_buffer +
                     ((uint64_t)(STORE_TRIGGER_ACCESS - 1) *
                      (uint64_t)stride));

#if FENCE_AFTER_EACH_STORE
        mfence();
#endif
    }

    // mfence();

    delay_after_trigger();

    /*
     * 第 7 个位置: probe
     * 0-based index: 6
     */
    // uint8_t *probe_addr =
    //     victim_buffer +
    //     ((uint64_t)STORE_TRIGGER_ACCESS * (uint64_t)stride);
    uint8_t *probe_addr =
        train_buffer +
        ((uint64_t)(STORE_TRIGGER_ACCESS-1) * (uint64_t)stride);

    return probe_latency(probe_addr);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [store_pc victim_buffer_addr min_diff_bit max_diff_bit rounds]\n"
            "default: store_pc=0x%lx victim_buffer_addr=0x%lx "
            "min_diff_bit=%d max_diff_bit=%d rounds=%d\n",
            prog,
            (unsigned long)DEFAULT_STORE_PC,
            (unsigned long)DEFAULT_VICTIM_BUFFER_ADDR,
            DEFAULT_MIN_DIFF_BIT,
            DEFAULT_MAX_DIFF_BIT,
            DEFAULT_ROUNDS);
}

int main(int argc, char **argv) {
    uintptr_t store_pc = DEFAULT_STORE_PC;
    uintptr_t victim_buffer_addr = DEFAULT_VICTIM_BUFFER_ADDR;

    int min_diff_bit = DEFAULT_MIN_DIFF_BIT;
    int max_diff_bit = DEFAULT_MAX_DIFF_BIT;
    int rounds = DEFAULT_ROUNDS;

    int stride = DEFAULT_STRIDE;

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
        store_pc = strtoull(argv[1], NULL, 0);
        victim_buffer_addr = strtoull(argv[2], NULL, 0);
        min_diff_bit = atoi(argv[3]);
        max_diff_bit = atoi(argv[4]);
        rounds = atoi(argv[5]);
    }

    if (min_diff_bit < 0 ||
        max_diff_bit >= 48 ||
        min_diff_bit > max_diff_bit ||
        rounds <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    uint64_t trigger_offset =
        (uint64_t)(STORE_TRIGGER_ACCESS - 1) * (uint64_t)stride;

    uint64_t probe_offset =
        (uint64_t)STORE_TRIGGER_ACCESS * (uint64_t)stride;

    if (probe_offset + LINE_SIZE > victim_buffer_size) {
        fprintf(stderr,
                "victim buffer is too small: probe_offset=%lu victim_size=%lu\n",
                (unsigned long)probe_offset,
                (unsigned long)victim_buffer_size);
        return 1;
    }

    /*
     * 多映射一个 page，保留你原始代码的风格。
     * 对非 page-aligned / low-bit changed address 更稳。
     */
    victim_buffer = map_data_buffer(victim_buffer_addr,
                                    victim_buffer_size + page_size,
                                    "victim buffer");

    if (!victim_buffer) {
        return 1;
    }

    memset(victim_buffer, -1, victim_buffer_size);

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

    /*
     * fault in victim buffer，避免测量混入 page fault。
     */
    for (uint64_t i = 0; i < victim_buffer_size; i += LINE_SIZE) {
        maccess(&victim_buffer[i]);
    }

    flush_victim_buffer();

    store_gadget_f store_gadget = map_store_gadget(store_pc);
    if (!store_gadget) {
        return 1;
    }

    printf("# store-stride memory/address collision test\n");
    printf("# stride=%d rounds=%d page_size=%lu store_pc=0x%016lx victim_buffer=0x%016lx\n",
           stride,
           rounds,
           (unsigned long)page_size,
           (unsigned long)store_pc,
           (unsigned long)victim_buffer_addr);

    printf("# training stores: 1..5, trigger store: 6, probe position: 7\n");
    printf("# trigger_offset=%lu probe_offset=%lu\n",
           (unsigned long)trigger_offset,
           (unsigned long)probe_offset);

    printf("# lower latency means victim probe line was more likely prefetched\n");
    printf("# FENCE_AFTER_EACH_STORE=%d\n", FENCE_AFTER_EACH_STORE);
    printf("case\tlatency_ns\n");

    /*
     * Baseline 1:
     * 只做 victim_buffer 上前 5 次 store，不做第 6 次 trigger。
     * 理想情况下这个应该偏高。
     */
    {
        uint64_t latency = 0;

        for (int atk_round = 0; atk_round < rounds; atk_round++) {
            latency += run_one_round(store_gadget,
                                     victim_buffer,
                                     0,
                                     stride);
        }

        printf("no_trigger_same_addr\t%lu\n",
               (unsigned long)(latency / (uint64_t)rounds));
    }

    /*
     * Baseline 2:
     * 前 5 次 store 和第 6 次 trigger 都在 victim_buffer 上。
     * 理想情况下这个应该偏低。
     */
    {
        uint64_t latency = 0;

        for (int atk_round = 0; atk_round < rounds; atk_round++) {
            latency += run_one_round(store_gadget,
                                     victim_buffer,
                                     1,
                                     stride);
        }

        printf("same_addr\t%lu\n",
               (unsigned long)(latency / (uint64_t)rounds));
    }

    /*
     * Address bit collision test:
     *
     * train_buffer:
     *   victim_buffer_addr ^ (1 << diff_bit)
     *
     * trigger:
     *   victim_buffer + 5 * stride
     *
     * probe:
     *   victim_buffer + 6 * stride
     */
    for (int diff_bit = min_diff_bit;
         diff_bit <= max_diff_bit;
         diff_bit++) {
        uintptr_t colliding_buffer_addr =
            victim_buffer_addr ^ (1ull << diff_bit);

        uint8_t *colliding_buffer =
            map_data_buffer(colliding_buffer_addr,
                            victim_buffer_size + page_size,
                            "colliding buffer");

        if (!colliding_buffer) {
            printf("bit_%d\t-1\n", diff_bit);
            continue;
        }

        /*
         * fault in / initialize colliding buffer。
         * 如果 colliding_buffer 与 victim_buffer 部分重叠，
         * 这里可能也会写到 victim 映射范围。
         * 每轮 run_one_round() 开头会 flush victim_buffer，
         * 所以 probe 仍然以 victim flush 后的状态为准。
         */
        memset(colliding_buffer, -1, victim_buffer_size);

        uint64_t latency = 0;

        for (int atk_round = 0; atk_round < rounds; atk_round++) {
            latency += run_one_round(store_gadget,
                                     colliding_buffer,
                                     1,
                                     stride);
        }

        printf("bit_%d\t%lu\n",
               diff_bit,
               (unsigned long)(latency / (uint64_t)rounds));
    }

    return 0;
}
