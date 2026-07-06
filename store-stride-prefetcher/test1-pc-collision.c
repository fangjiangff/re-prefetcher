#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "until.h"

#define ARRAY_ALIGNMENT 4096
#define ITEMS 2048
#define DUMMY_BUFFER_PAGES 10

#define DEFAULT_BASE_STORE_PC 0x500000120ull
#define DEFAULT_FAR_SAME_LOW_TRIGGER_PC 0x700000120ull
#define DEFAULT_FAR_DIFF_LOW_TRIGGER_PC 0x7100009a0ull

/*
 * diff_bit 必须从 3 开始。
 * bit 0/1 会破坏 AArch64 4-byte alignment。
 * bit 2 可能让新 gadget 落在旧 gadget 的 strb/ret 中间。
 */
#define DEFAULT_MIN_DIFF_BIT 3
#define DEFAULT_MAX_DIFF_BIT 47
#define DEFAULT_ROUNDS 1000

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef ARCH_NAME
#define ARCH_NAME "unknown"
#endif

/*
 * 你可以按自己的 store-stride prefetcher 实验改这个 stride。
 * 这里沿用你前面代码的 15 cache lines。
 */
#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#define DEFAULT_STRIDE (STRIDE_LINES * LINE_SIZE)

/*
 * STORE_TRIGGER_ACCESS 是 train+trigger 总访问数。
 *
 * 1-based:
 *   第 1..STORE_TRIGGER_ACCESS-1 次 access: training
 *   第 STORE_TRIGGER_ACCESS 次 access:       trigger
 *   第 STORE_TRIGGER_ACCESS+1 个位置:       probe
 *
 * 0-based:
 *   training index: 0..STORE_TRIGGER_ACCESS-2
 *   trigger index:  STORE_TRIGGER_ACCESS-1
 *   probe index:    STORE_TRIGGER_ACCESS
 */
#ifndef STORE_TRIGGER_ACCESS
#define STORE_TRIGGER_ACCESS 6
#endif

#define MAX_MAPPED_PAGES 256

/*
 * 默认不在每次 stride access 后加 DSB/ISB。
 * 如果你之前校准 trigger access 时每次访问后都有 fence，
 * 可以把这里改成 1 保持实验条件一致。
 */
#define FENCE_AFTER_EACH_STORE 0

typedef void (*store_gadget_f)(void *);

#ifndef STRIDE_ACCESS_PREFETCH
#define STRIDE_ACCESS_PREFETCH 0
#endif

#ifndef STRIDE_ACCESS_LOAD
#define STRIDE_ACCESS_LOAD 0
#endif

#if STRIDE_ACCESS_PREFETCH && STRIDE_ACCESS_LOAD
#error "Only one stride access mode can be enabled"
#endif

#if STRIDE_ACCESS_PREFETCH
#define STRIDE_ACCESS_NAME "prefetch"
#define STRIDE_ACCESS_ASM "    PRFM PLDL1KEEP, [x0]\n"
#elif STRIDE_ACCESS_LOAD
#define STRIDE_ACCESS_NAME "load"
#define STRIDE_ACCESS_ASM "    ldr x1, [x0]\n"
#else
#define STRIDE_ACCESS_NAME "store"
#define STRIDE_ACCESS_ASM "    strb w0, [x0]\n"
#endif

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t array2[ITEMS * LINE_SIZE] __attribute__((aligned(ARRAY_ALIGNMENT)));

static uint8_t *dummy_buffer;
static size_t page_size;
static size_t dummy_buffer_size;

static uintptr_t mapped_pages[MAX_MAPPED_PAGES];
static int mapped_page_count;

extern char _store_gadget_asm_start[];
extern char _store_gadget_asm_end[];

/*
 * stride access gadget:
 *
 *   x0 = target address
 *   store mode:    strb w0, [x0]
 *   load mode:     ldr x1, [x0]
 *   prefetch mode: PRFM PLDL1KEEP, [x0]
 *   ret
 *
 * 对本实验来说访问的值不重要，重要的是访问指令的 PC 可控。
 */
asm(
    ".global _store_gadget_asm_start\n"
    ".global _store_gadget_asm_end\n"
    "_store_gadget_asm_start:\n"
    STRIDE_ACCESS_ASM
    "    ret\n"
    "_store_gadget_asm_end:\n"
    "    nop\n"
);

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
                         MAP_FIXED_NOREPLACE | MAP_ANONYMOUS |
                         MAP_PRIVATE | MAP_POPULATE,
                         -1, 0);

    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap gadget page 0x%016lx failed: %s\n",
                (unsigned long)page, strerror(errno));
        return -1;
    }

    if ((uintptr_t)mapping != page) {
        fprintf(stderr,
                "mmap returned wrong page: expected 0x%016lx got %p\n",
                (unsigned long)page, mapping);
        return -1;
    }

    mapped_pages[mapped_page_count++] = page;
    return 0;
}

static store_gadget_f map_store_gadget(uintptr_t address) {
    uintptr_t page = page_base(address);
    size_t page_offset = address - page;
    size_t gadget_size =
        (size_t)(_store_gadget_asm_end - _store_gadget_asm_start);

    if (page_offset + gadget_size > page_size) {
        fprintf(stderr, "store gadget at 0x%016lx crosses a page boundary\n",
                (unsigned long)address);
        return NULL;
    }

    if (ensure_gadget_page(page) != 0) {
        return NULL;
    }

    memcpy((void *)address, _store_gadget_asm_start, gadget_size);

    __builtin___clear_cache((char *)address,
                            (char *)(address + gadget_size));

    return (store_gadget_f)(void *)address;
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

static void delay_after_trigger(void) {
    /*
     * 用真实 load + nop 给硬件预取请求一点完成时间。
     * 用 maccess 防止编译器把 array1 访问优化掉。
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
 * do_train = 1:
 *   前 STORE_TRIGGER_ACCESS-1 次 access 用 train_store
 *
 * do_trigger = 1:
 *   第 STORE_TRIGGER_ACCESS 次 access 用 trigger_store
 *
 * probe 第 STORE_TRIGGER_ACCESS+1 个位置。
 *
 * no_trigger baseline 很重要：
 * 如果 no_trigger 也低延迟，说明 training accesses 已经能预取到 predicted 位置，
 * 那这个 trigger access 假设在当前配置下不干净。
 */
static uint64_t run_one_round(store_gadget_f train_store,
                              store_gadget_f trigger_store,
                              int do_train,
                              int do_trigger,
                              int stride,
                              int probe_pos) {
    dummy_accesses();
    cpp_rctx();
    flush_array2();

    /*
     * 第 1..STORE_TRIGGER_ACCESS-1 次 access: training
     * 0-based index: 0..STORE_TRIGGER_ACCESS-2
     */
    if (do_train) {
        for (int step = 0; step < STORE_TRIGGER_ACCESS - 1; step++) {
            train_store(array2 + ((uint64_t)step * (uint64_t)stride));

#if FENCE_AFTER_EACH_STORE
            mfence();
#endif
        }
    }

    /*
     * 第 STORE_TRIGGER_ACCESS 次 access: trigger
     * 0-based index: STORE_TRIGGER_ACCESS-1
     */
    if (do_trigger) {
        trigger_store(array2 +
                      ((uint64_t)(STORE_TRIGGER_ACCESS - 1) *
                       (uint64_t)stride));

#if FENCE_AFTER_EACH_STORE
        mfence();
#endif
    }

    // mfence();

    delay_after_trigger();

    uint8_t *probe_addr = array2 + ((uint64_t)probe_pos * LINE_SIZE);

    return probe_latency(probe_addr);
}

static void print_case_results(const char *case_name,
                               store_gadget_f train_store,
                               store_gadget_f trigger_store,
                               int do_train,
                               int do_trigger,
                               int stride,
                               int rounds) {
    uint64_t latency_sum[PROBE_POSITIONS] = {0};
    int probe_count[PROBE_POSITIONS] = {0};

    for (int atk_round = 0; atk_round < rounds; atk_round++) {
        int probe_pos = atk_round % PROBE_POSITIONS;
        latency_sum[probe_pos] += run_one_round(train_store,
                                                trigger_store,
                                                do_train,
                                                do_trigger,
                                                stride,
                                                probe_pos);
        probe_count[probe_pos]++;
    }

    for (int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
        unsigned long avg_ns = 0;
        if (probe_count[probe_pos] > 0) {
            avg_ns = (unsigned long)(latency_sum[probe_pos] /
                                     (uint64_t)probe_count[probe_pos]);
        }
        printf("# probe_detail\t%s\t%d\t%d\t%lu\t%d\n",
               case_name,
               probe_pos,
               probe_pos * LINE_SIZE,
               avg_ns,
               probe_count[probe_pos]);
    }

    int predicted_probe_pos = (STORE_TRIGGER_ACCESS * stride) / LINE_SIZE;
    unsigned long predicted_avg_ns = 0;
    if (probe_count[predicted_probe_pos] > 0) {
        predicted_avg_ns = (unsigned long)(latency_sum[predicted_probe_pos] /
                                           (uint64_t)probe_count[predicted_probe_pos]);
    }
    printf("%s\t%lu\n", case_name, predicted_avg_ns);
}

static void print_unmapped_case_results(const char *case_name) {
    for (int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
        printf("# probe_detail\t%s\t%d\t%d\t-1\t0\n",
               case_name,
               probe_pos,
               probe_pos * LINE_SIZE);
    }
    printf("%s\t-1\n", case_name);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [base_access_pc min_diff_bit max_diff_bit rounds]\n"
            "default: access=%s base_access_pc=0x%lx min_diff_bit=%d max_diff_bit=%d rounds=%d\n",
            prog,
            STRIDE_ACCESS_NAME,
            (unsigned long)DEFAULT_BASE_STORE_PC,
            DEFAULT_MIN_DIFF_BIT,
            DEFAULT_MAX_DIFF_BIT,
            DEFAULT_ROUNDS);
}

int main(int argc, char **argv) {
    uintptr_t base_pc = DEFAULT_BASE_STORE_PC;
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

    if (min_diff_bit < 3 ||
        max_diff_bit >= 48 ||
        min_diff_bit > max_diff_bit ||
        rounds <= 0 ||
        STORE_TRIGGER_ACCESS < 2 ||
        STRIDE_LINES <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    uint64_t trigger_offset =
        (uint64_t)(STORE_TRIGGER_ACCESS - 1) * (uint64_t)stride;

    uint64_t probe_offset =
        (uint64_t)STORE_TRIGGER_ACCESS * (uint64_t)stride;
    uint64_t max_probe_offset = (uint64_t)(PROBE_POSITIONS - 1) * LINE_SIZE;
    int predicted_probe_pos = (int)(probe_offset / LINE_SIZE);

    if (predicted_probe_pos < 0 || predicted_probe_pos >= PROBE_POSITIONS) {
        fprintf(stderr,
                "predicted probe position %d must be inside PROBE_POSITIONS=%d\n",
                predicted_probe_pos,
                PROBE_POSITIONS);
        return 1;
    }

    if (trigger_offset + LINE_SIZE > sizeof(array2) ||
        max_probe_offset + LINE_SIZE > sizeof(array2)) {
        fprintf(stderr,
                "array2 is too small: trigger_offset=%lu max_probe_offset=%lu array2_size=%lu\n",
                (unsigned long)trigger_offset,
                (unsigned long)max_probe_offset,
                (unsigned long)sizeof(array2));
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

    /*
     * 预先 fault in array2，避免后面测量混入 page fault。
     */
    for (int i = 0; i < ITEMS; i++) {
        maccess(&array2[i * LINE_SIZE]);
    }

    flush_array2();

    store_gadget_f train_store = map_store_gadget(base_pc);
    if (!train_store) {
        return 1;
    }

    printf("# %s %s-stride single-PC collision test\n",
           ARCH_NAME,
           STRIDE_ACCESS_NAME);
    printf("# stride_lines=%d stride=%d rounds=%d page_size=%lu base_access_pc=0x%016lx\n",
           STRIDE_LINES,
           stride,
           rounds,
           (unsigned long)page_size,
           (unsigned long)base_pc);

    printf("# accesses=%d train_only_accesses=%d trigger_accesses=1 probe_positions=%d\n",
           STORE_TRIGGER_ACCESS,
           STORE_TRIGGER_ACCESS - 1,
           PROBE_POSITIONS);
    printf("# trigger_offset=%lu probe_offset=%lu max_probe_offset=%lu\n",
           (unsigned long)trigger_offset,
           (unsigned long)probe_offset,
           (unsigned long)max_probe_offset);
    printf("# final output uses probe_position=%d offset_bytes=%lu\n",
           predicted_probe_pos,
           (unsigned long)probe_offset);
    printf("# all probed positions are recorded as # probe_detail lines in the raw txt\n");

    printf("# lower latency means the predicted probe position was more likely prefetched\n");
    printf("# FENCE_AFTER_EACH_STORE=%d\n", FENCE_AFTER_EACH_STORE);
    printf("case\tlatency_ns\n");

    print_case_results("no_trigger", train_store, train_store, 1, 0, stride, rounds);
    print_case_results("trigger_only", train_store, train_store, 0, 1, stride, rounds);
    print_case_results("same_pc", train_store, train_store, 1, 1, stride, rounds);
    print_case_results("load_trigger", train_store, mLoad_noinline, 1, 1, stride, rounds);

    {
        uintptr_t far_pc = DEFAULT_FAR_SAME_LOW_TRIGGER_PC;
        store_gadget_f far_trigger_store = map_store_gadget(far_pc);
        char case_name[64];

        snprintf(case_name, sizeof(case_name), "far_same_low_pc_0x%016lx",
                 (unsigned long)far_pc);
        if (!far_trigger_store) {
            print_unmapped_case_results(case_name);
        } else {
            print_case_results(case_name, train_store, far_trigger_store, 1, 1,
                               stride, rounds);
        }
    }

    {
        uintptr_t far_pc = DEFAULT_FAR_DIFF_LOW_TRIGGER_PC;
        store_gadget_f far_trigger_store = map_store_gadget(far_pc);
        char case_name[64];

        snprintf(case_name, sizeof(case_name), "far_diff_low_pc_0x%016lx",
                 (unsigned long)far_pc);
        if (!far_trigger_store) {
            print_unmapped_case_results(case_name);
        } else {
            print_case_results(case_name, train_store, far_trigger_store, 1, 1,
                               stride, rounds);
        }
    }

    for (int diff_bit = min_diff_bit; diff_bit <= max_diff_bit; diff_bit++) {
        uintptr_t trigger_pc = base_pc ^ (1ull << diff_bit);
        store_gadget_f trigger_store = map_store_gadget(trigger_pc);
        char case_name[32];

        snprintf(case_name, sizeof(case_name), "bit_%d", diff_bit);
        if (!trigger_store) {
            print_unmapped_case_results(case_name);
            continue;
        }

        print_case_results(case_name, train_store, trigger_store, 1, 1,
                           stride, rounds);
    }

    return 0;
}
