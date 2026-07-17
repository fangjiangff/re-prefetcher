#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../until.h"

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#ifndef ROUNDS
#define ROUNDS 40000
#endif

#ifndef SCAN_LINES
#define SCAN_LINES 101
#endif

#ifndef BUDDY_PAGES
#define BUDDY_PAGES 16384
#endif

#ifndef DUMMY_BUFFER_PAGES
#define DUMMY_BUFFER_PAGES 10
#endif

#define TRAIN_POS 0
#define TRIGGER_POS STRIDE_LINES
#define PREDICTED_POS (2 * STRIDE_LINES)
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)
#define SAME_MASK ((((uint64_t)1 << 34) - 1ULL) & ~(((uint64_t)1 << 12) - 1ULL))
#define HIGH_DIFF_SHIFT 34

struct page_info {
    uint8_t *va;
    uint64_t pa;
    int valid;
};

static uint8_t *dummy_buffer;
static uint8_t *buddy_pool;
static struct page_info *pages;
static int page_count;
static int pagemap_fd = -1;

static void dummy_accesses(void) {
    dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
}

static int open_pagemap(void) {
    pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    return pagemap_fd >= 0 ? 0 : -1;
}

static int virt_to_phys(void *addr, uint64_t *pa) {
    uint64_t entry;
    uint64_t pfn;
    off_t offset;
    ssize_t got;
    uintptr_t va = (uintptr_t)addr;

    if (pagemap_fd < 0) {
        return -1;
    }

    offset = (off_t)((va / PAGE_SIZE) * sizeof(entry));
    got = pread(pagemap_fd, &entry, sizeof(entry), offset);
    if (got != (ssize_t)sizeof(entry)) {
        return -1;
    }
    if (((entry >> 63) & 1ULL) == 0) {
        return -1;
    }

    pfn = entry & ((1ULL << 55) - 1ULL);
    if (pfn == 0) {
        return -1;
    }

    *pa = (pfn * PAGE_SIZE) + (va % PAGE_SIZE);
    return 0;
}

static void warm_pages(uint8_t *base, size_t pages_count) {
    for (size_t page = 0; page < pages_count; page++) {
        uint8_t *addr = base + page * PAGE_SIZE;
        memset(addr, (int)(page & 0xff), PAGE_SIZE);
        mLoad_inline(addr);
    }
}

static uint64_t same_key(uint64_t pa) {
    return pa & SAME_MASK;
}

static int collect_page_info(void) {
    int valid = 0;

    pages = calloc((size_t)BUDDY_PAGES, sizeof(*pages));
    if (!pages) {
        perror("calloc pages");
        return -1;
    }

    for (int i = 0; i < BUDDY_PAGES; i++) {
        uint8_t *va = buddy_pool + (size_t)i * PAGE_SIZE;
        uint64_t pa;

        if (virt_to_phys(va, &pa) == 0) {
            pages[valid].va = va;
            pages[valid].pa = pa & ~((uint64_t)PAGE_SIZE - 1ULL);
            pages[valid].valid = 1;
            valid++;
        }
    }
    page_count = valid;
    return valid;
}

static int compare_page_key(const void *left, const void *right) {
    const struct page_info *a = (const struct page_info *)left;
    const struct page_info *b = (const struct page_info *)right;
    uint64_t key_a = same_key(a->pa);
    uint64_t key_b = same_key(b->pa);

    if (key_a < key_b) {
        return -1;
    }
    if (key_a > key_b) {
        return 1;
    }
    if (a->pa < b->pa) {
        return -1;
    }
    if (a->pa > b->pa) {
        return 1;
    }
    return 0;
}

static int scan_range_fits(uint8_t *va) {
    return (size_t)(va - buddy_pool) + (size_t)SCAN_LINES * LINE_SIZE <=
           (size_t)BUDDY_PAGES * PAGE_SIZE;
}

static int find_same_12_33_pair(int *index1, int *index2) {
    int best_index1 = -1;
    int best_index2 = -1;
    int best_high_diff_bits = -1;
    uint64_t best_high_xor = 0;
    uint64_t best_pa_xor = 0;

    qsort(pages, (size_t)page_count, sizeof(*pages), compare_page_key);

    for (int i = 0; i < page_count; i++) {
        uint64_t key = same_key(pages[i].pa);
        int group_end = i + 1;

        while (group_end < page_count && same_key(pages[group_end].pa) == key) {
            group_end++;
        }

        for (int left = i; left < group_end; left++) {
            if (!scan_range_fits(pages[left].va)) {
                continue;
            }
            for (int right = left + 1; right < group_end; right++) {
                uint64_t pa_xor;
                uint64_t high_xor;
                int high_diff_bits;

                if (!scan_range_fits(pages[right].va) || pages[left].pa == pages[right].pa) {
                    continue;
                }

                pa_xor = pages[left].pa ^ pages[right].pa;
                high_xor = pa_xor >> HIGH_DIFF_SHIFT;
                high_diff_bits = __builtin_popcountll(high_xor);

                if (high_diff_bits > best_high_diff_bits ||
                    (high_diff_bits == best_high_diff_bits && high_xor > best_high_xor) ||
                    (high_diff_bits == best_high_diff_bits && high_xor == best_high_xor &&
                     pa_xor > best_pa_xor)) {
                    best_index1 = left;
                    best_index2 = right;
                    best_high_diff_bits = high_diff_bits;
                    best_high_xor = high_xor;
                    best_pa_xor = pa_xor;
                }
            }
        }

        i = group_end - 1;
    }

    if (best_index1 >= 0) {
        *index1 = best_index1;
        *index2 = best_index2;
        return 1;
    }
    return 0;
}

static void flush_experiment_lines(uint8_t *pa1_va, uint8_t *pa2_va) {
    for (int line = 0; line < SCAN_LINES; line++) {
        flush(pa1_va + (uint64_t)line * LINE_SIZE);
        flush(pa2_va + (uint64_t)line * LINE_SIZE);
    }
    mfence();
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t start = timestamp();
    mLoad_inline(addr);
    return timestamp() - start;
}

__attribute__((noinline)) static void run_trainer(uint8_t *pa1_va) {
    mStore_inline(pa1_va + TRAIN_POS * LINE_SIZE);
}

__attribute__((noinline)) static void run_trigger(uint8_t *pa2_va) {
    mStore_inline(pa2_va + TRIGGER_POS * LINE_SIZE);
}

static void print_probe_row(const char *region,
                            int line,
                            uint8_t *base_va,
                            uint64_t latency_sum,
                            int probe_count) {
    uint8_t *probe_va = base_va + (uint64_t)line * LINE_SIZE;
    uint64_t probe_pa = 0;
    long avg_ns = -1;

    if (probe_count > 0) {
        avg_ns = (long)(latency_sum / (uint64_t)probe_count);
    }

    printf("%s\t%d\t%d\t0x%016" PRIx64 "\t",
           region,
           line,
           line * LINE_SIZE,
           (uint64_t)(uintptr_t)probe_va);
    if (virt_to_phys(probe_va, &probe_pa) == 0) {
        printf("0x%016" PRIx64, probe_pa);
    } else {
        printf("unknown");
    }
    printf("\t%ld\t%d\n", avg_ns, probe_count);
}

static void run_scan(uint8_t *pa1_va, uint8_t *pa2_va) {
    uint64_t latency_sum[2][SCAN_LINES] = {{0}};
    int probe_count[2][SCAN_LINES] = {{0}};

    for (uint64_t round = 0; round < ROUNDS; round++) {
        int probe_pos = (int)(round % SCAN_LINES);
        cpp_rctx();
        dummy_accesses();
        flush_experiment_lines(pa1_va, pa2_va);

    
        mStore_inline(pa1_va + 0 * LINE_SIZE);
        nops();
        mStore_inline(pa1_va + 5 * LINE_SIZE);
        nops();
        mStore_inline(pa1_va + 10 * LINE_SIZE);
        nops();
        mStore_inline(pa1_va + 15 * LINE_SIZE);
        nops();
        mStore_inline(pa2_va + 20 * LINE_SIZE);
        nops();
       
        // mStore_inline(pa1_va + 25 * LINE_SIZE);
        // run_trainer(pa1_va);// mStore_inline(pa1_va + 0 * LINE_SIZE);
        // run_trigger(pa2_va);// mStore_inline(pa2_va + 5 * LINE_SIZE);
        // mStore_inline(pa1_va + 30 * LINE_SIZE);

        latency_sum[0][probe_pos] += probe_latency(pa1_va + (uint64_t)probe_pos * LINE_SIZE);
        probe_count[0][probe_pos]++;

        cpp_rctx();
        dummy_accesses();
        flush_experiment_lines(pa1_va, pa2_va);
        // run_trainer(pa1_va);
        // run_trigger(pa2_va);

        mStore_inline(pa1_va + 0 * LINE_SIZE);
        nops();
        mStore_inline(pa1_va + 5 * LINE_SIZE);
        nops();
        mStore_inline(pa1_va + 10 * LINE_SIZE);
        nops();
        mStore_inline(pa1_va + 15 * LINE_SIZE);
        nops();
        mStore_inline(pa2_va + 20 * LINE_SIZE);
        nops();
       
        // mStore_inline(pa1_va + 25 * LINE_SIZE);
        // mStore_inline(pa1_va + 30 * LINE_SIZE);
        latency_sum[1][probe_pos] += probe_latency(pa2_va + (uint64_t)probe_pos * LINE_SIZE);
        probe_count[1][probe_pos]++;
    }

    printf("region\tprobe_pos\toffset_bytes\tprobe_va\tprobe_pa\tlatency_ns\tprobes\n");
    for (int line = 0; line < SCAN_LINES; line++) {
        print_probe_row("PA1", line, pa1_va, latency_sum[0][line], probe_count[0][line]);
    }
    for (int line = 0; line < SCAN_LINES; line++) {
        print_probe_row("PA2", line, pa2_va, latency_sum[1][line], probe_count[1][line]);
    }
}

static void print_failure_rows(void) {
    printf("region\tprobe_pos\toffset_bytes\tprobe_va\tprobe_pa\tlatency_ns\tprobes\n");
    for (int region = 0; region < 2; region++) {
        const char *name = region == 0 ? "PA1" : "PA2";
        for (int line = 0; line < SCAN_LINES; line++) {
            printf("%s\t%d\t%d\tunknown\tunknown\t-1\t0\n",
                   name,
                   line,
                   line * LINE_SIZE);
        }
    }
}

int main(void) {
    size_t buddy_bytes = (size_t)BUDDY_PAGES * PAGE_SIZE;
    int valid_pages;
    int index1 = -1;
    int index2 = -1;

    if (STRIDE_LINES <= 0 || ROUNDS <= 0 || SCAN_LINES <= 0) {
        fprintf(stderr, "invalid STRIDE_LINES/ROUNDS/SCAN_LINES\n");
        return 1;
    }
    if ((size_t)SCAN_LINES * LINE_SIZE > buddy_bytes) {
        fprintf(stderr, "SCAN_LINES range exceeds buddy pool\n");
        return 1;
    }

    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        perror("mmap dummy_buffer");
        return 1;
    }

    buddy_pool = mmap(NULL, buddy_bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (buddy_pool == MAP_FAILED) {
        perror("mmap buddy_pool");
        return 1;
    }
    warm_pages(buddy_pool, (size_t)BUDDY_PAGES);

    printf("# store-stride same PA[12:33] validation test\n");
    printf("# buddy_pages=%d buddy_bytes=%" PRIu64 " stride_lines=%d rounds=%d scan_lines=%d\n",
           BUDDY_PAGES,
           (uint64_t)buddy_bytes,
           STRIDE_LINES,
           ROUNDS,
           SCAN_LINES);
    printf("# same_key_mask=0x%016" PRIx64 "\n", (uint64_t)SAME_MASK);
    printf("# train: PA1 + 0*LINE_SIZE, trigger: PA2 + %d*LINE_SIZE\n",
           TRIGGER_POS);

    if (open_pagemap() != 0) {
        fprintf(stderr, "open /proc/self/pagemap failed: %s\n", strerror(errno));
        fprintf(stderr, "run with sudo or CAP_SYS_ADMIN to expose PFNs\n");
        printf("# pair_detail\tnot_found\tno_pagemap\n");
        print_failure_rows();
        return 0;
    }

    valid_pages = collect_page_info();
    if (valid_pages <= 0) {
        fprintf(stderr, "no valid PFNs from pagemap; run with sudo or CAP_SYS_ADMIN\n");
        printf("# pair_detail\tnot_found\tno_valid_pfns\n");
        print_failure_rows();
        return 0;
    }
    printf("# valid_pages=%d\n", valid_pages);

    if (!find_same_12_33_pair(&index1, &index2)) {
        printf("# pair_detail\tnot_found\tno_same_PA12_33_pair\n");
        print_failure_rows();
        return 0;
    }

    printf("# pair_detail\tfound\tva1=0x%016" PRIx64 "\tpa1=0x%016" PRIx64
           "\tva2=0x%016" PRIx64 "\tpa2=0x%016" PRIx64
           "\tpa_xor=0x%016" PRIx64 "\thigh_xor=0x%016" PRIx64
           "\thigh_diff_bits=%d\tsame_key=0x%016" PRIx64 "\n",
           (uint64_t)(uintptr_t)pages[index1].va,
           pages[index1].pa,
           (uint64_t)(uintptr_t)pages[index2].va,
           pages[index2].pa,
           pages[index1].pa ^ pages[index2].pa,
           (pages[index1].pa ^ pages[index2].pa) >> HIGH_DIFF_SHIFT,
           __builtin_popcountll((pages[index1].pa ^ pages[index2].pa) >> HIGH_DIFF_SHIFT),
           same_key(pages[index1].pa));

    run_scan(pages[index1].va, pages[index2].va);

    if (pagemap_fd >= 0) {
        close(pagemap_fd);
    }
    munmap(buddy_pool, buddy_bytes);
    munmap(dummy_buffer, DUMMY_BUFFER_SIZE);
    free(pages);
    return 0;
}
