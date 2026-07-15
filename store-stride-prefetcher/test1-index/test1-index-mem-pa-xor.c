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

#ifndef MIN_DIFF_BIT
#define MIN_DIFF_BIT 12
#endif

#ifndef MAX_DIFF_BIT
#define MAX_DIFF_BIT 47
#endif

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#ifndef STORE_ACCESSES
#define STORE_ACCESSES 2
#endif

#ifndef ROUNDS
#define ROUNDS 40000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 64
#endif

#ifndef BUDDY_PAGES
#define BUDDY_PAGES 16384
#endif

#ifndef DUMMY_BUFFER_PAGES
#define DUMMY_BUFFER_PAGES 10
#endif

#define ALIAS_SIZE PAGE_SIZE
#define STRIDE_BYTES (STRIDE_LINES * LINE_SIZE)
#define TRAIN_ONLY_ACCESSES (STORE_ACCESSES - 1)
#define TRIGGER_POS (TRAIN_ONLY_ACCESSES * STRIDE_LINES)
#define PREDICTED_POS (STORE_ACCESSES * STRIDE_LINES)
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

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

static void flush_pages(uint8_t *va1, uint8_t *va2) {
    for (size_t offset = 0; offset < ALIAS_SIZE; offset += LINE_SIZE) {
        flush(va1 + offset);
        if (va2 != va1) {
            flush(va2 + offset);
        }
    }
    mfence();
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t start = timestamp();
    mLoad_inline(addr);
    return timestamp() - start;
}

__attribute__((noinline)) static void run_trainer(uint8_t *va1) {
    for (int access = 0; access < TRAIN_ONLY_ACCESSES; access++) {
        mStore_inline(va1 + (uint64_t)access * STRIDE_BYTES);
    }
}

__attribute__((noinline)) static void run_trigger(uint8_t *va2) {
    mStore_inline(va2 + TRIGGER_POS * LINE_SIZE);
}

static void run_case(const char *result_name,
                     const char *detail_name,
                     uint8_t *va1,
                     uint8_t *va2,
                     uint64_t rounds,
                     int enable_trainer,
                     int enable_trigger) {
    uint64_t latency_sum[PROBE_POSITIONS] = {0};
    int probe_count[PROBE_POSITIONS] = {0};

    for (uint64_t round = 0; round < rounds; round++) {
        int probe_pos = (int)(round % PROBE_POSITIONS);

        cpp_rctx();
        dummy_accesses();
        flush_pages(va1, va2);

        if (enable_trainer) {
            run_trainer(va1);
        }
        if (enable_trigger) {
            run_trigger(va2);
        }

        latency_sum[probe_pos] += probe_latency(va2 + probe_pos * LINE_SIZE);
        probe_count[probe_pos]++;
    }

    for (int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
        unsigned long avg_ns = 0;
        if (probe_count[probe_pos] > 0) {
            avg_ns = (unsigned long)(latency_sum[probe_pos] /
                                     (uint64_t)probe_count[probe_pos]);
        }
        printf("# probe_detail\t%s\t%d\t%d\t%lu\t%d\n",
               detail_name,
               probe_pos,
               probe_pos * LINE_SIZE,
               avg_ns,
               probe_count[probe_pos]);
    }

    unsigned long predicted_avg_ns = 0;
    if (PREDICTED_POS < PROBE_POSITIONS && probe_count[PREDICTED_POS] > 0) {
        predicted_avg_ns = (unsigned long)(latency_sum[PREDICTED_POS] /
                                           (uint64_t)probe_count[PREDICTED_POS]);
    }
    printf("%s\t%lu\n", result_name, predicted_avg_ns);
}

static int compare_page_pa(const void *left, const void *right) {
    const struct page_info *a = (const struct page_info *)left;
    const struct page_info *b = (const struct page_info *)right;

    if (a->pa < b->pa) {
        return -1;
    }
    if (a->pa > b->pa) {
        return 1;
    }
    return 0;
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

    qsort(pages, (size_t)valid, sizeof(*pages), compare_page_pa);
    page_count = valid;
    return valid;
}

static int find_page_by_pa(uint64_t pa) {
    int left = 0;
    int right = page_count - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (pages[mid].pa == pa) {
            return mid;
        }
        if (pages[mid].pa < pa) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return -1;
}

static int find_pair_for_mask(uint64_t mask, int *index1, int *index2) {
    for (int i = 0; i < page_count; i++) {
        int j = find_page_by_pa(pages[i].pa ^ mask);
        if (j >= 0 && j != i) {
            *index1 = i;
            *index2 = j;
            return 1;
        }
    }
    return 0;
}

static void run_same_pa_baselines(uint64_t rounds) {
    int ref = -1;

    for (int i = 0; i < page_count; i++) {
        if (pages[i].valid) {
            ref = i;
            break;
        }
    }

    if (ref < 0) {
        printf("same_pa_full\t-1\n");
        printf("same_pa_no_trigger\t-1\n");
        printf("same_pa_no_trainer\t-1\n");
        return;
    }

    printf("# pair_detail\tsame_pa\tva=0x%016" PRIx64 "\tpa=0x%016" PRIx64 "\n",
           (uint64_t)(uintptr_t)pages[ref].va,
           pages[ref].pa);
    run_case("same_pa_full", "same_pa_full", pages[ref].va, pages[ref].va,
             rounds, 1, 1);
    run_case("same_pa_no_trigger", "same_pa_no_trigger",
             pages[ref].va, pages[ref].va, rounds, 1, 0);
    run_case("same_pa_no_trainer", "same_pa_no_trainer",
             pages[ref].va, pages[ref].va, rounds, 0, 1);
}

static void run_mask_case(const char *result_name,
                          const char *detail_name,
                          uint64_t mask,
                          uint64_t rounds) {
    int index1 = -1;
    int index2 = -1;
    uint8_t *va1;
    uint8_t *va2;
    uint64_t pa1;
    uint64_t pa2;

    if (!find_pair_for_mask(mask, &index1, &index2)) {
        printf("# pair_detail\t%s\tnot_found\tmask=0x%016" PRIx64 "\n",
               detail_name,
               mask);
        printf("%s\t-1\n", result_name);
        return;
    }

    va1 = pages[index1].va;
    va2 = pages[index2].va;
    pa1 = pages[index1].pa;
    pa2 = pages[index2].pa;

    printf("# pair_detail\t%s\tva1=0x%016" PRIx64 "\tpa1=0x%016" PRIx64
           "\tva2=0x%016" PRIx64 "\tpa2=0x%016" PRIx64
           "\tmask=0x%016" PRIx64 "\tpa_xor=0x%016" PRIx64 "\n",
           detail_name,
           (uint64_t)(uintptr_t)va1,
           pa1,
           (uint64_t)(uintptr_t)va2,
           pa2,
           mask,
           pa1 ^ pa2);

    run_case(result_name, detail_name, va1, va2, rounds, 1, 1);
}

static void run_bit(int bit, uint64_t rounds) {
    char result_name[64];
    char detail_name[64];
    uint64_t mask = 1ULL << bit;

    snprintf(result_name, sizeof(result_name), "bit_%d_full", bit);
    snprintf(detail_name, sizeof(detail_name), "bit_%d_full", bit);
    run_mask_case(result_name, detail_name, mask, rounds);
}

static void run_bit_pair(int bit_a, int bit_b, uint64_t rounds) {
    char result_name[64];
    char detail_name[64];
    uint64_t mask = (1ULL << bit_a) | (1ULL << bit_b);

    snprintf(result_name, sizeof(result_name), "bits_%d_%d_full", bit_a, bit_b);
    snprintf(detail_name, sizeof(detail_name), "bits_%d_%d_full", bit_a, bit_b);
    run_mask_case(result_name, detail_name, mask, rounds);
}

static void run_bit_triple(int bit_a, int bit_b, int bit_c, uint64_t rounds) {
    char result_name[80];
    char detail_name[80];
    uint64_t mask = (1ULL << bit_a) | (1ULL << bit_b) | (1ULL << bit_c);

    snprintf(result_name, sizeof(result_name), "triple_%d_%d_%d_full", bit_a, bit_b, bit_c);
    snprintf(detail_name, sizeof(detail_name), "triple_%d_%d_%d_full", bit_a, bit_b, bit_c);
    run_mask_case(result_name, detail_name, mask, rounds);
}

static void run_pairpair(int bit_a, int bit_b, int bit_c, int bit_d, uint64_t rounds) {
    char result_name[96];
    char detail_name[96];
    uint64_t mask = (1ULL << bit_a) | (1ULL << bit_b) |
                    (1ULL << bit_c) | (1ULL << bit_d);

    snprintf(result_name, sizeof(result_name), "pairpair_%d_%d__%d_%d_full",
             bit_a, bit_b, bit_c, bit_d);
    snprintf(detail_name, sizeof(detail_name), "pairpair_%d_%d__%d_%d_full",
             bit_a, bit_b, bit_c, bit_d);
    run_mask_case(result_name, detail_name, mask, rounds);
}

static void run_targeted_hash_cases(uint64_t rounds) {
    static const int groups[][3] = {
        {16, 22, 28},
        {17, 23, 29},
        {18, 24, 30},
        {19, 25, 31},
        {20, 26, 32},
        {21, 27, 33},
    };
    const int group_count = (int)(sizeof(groups) / sizeof(groups[0]));

    for (int group = 0; group < group_count; group++) {
        run_bit_triple(groups[group][0], groups[group][1], groups[group][2], rounds);
    }

    for (int group_a = 0; group_a < group_count; group_a++) {
        for (int group_b = group_a + 1; group_b < group_count; group_b++) {
            for (int a0 = 0; a0 < 3; a0++) {
                for (int a1 = a0 + 1; a1 < 3; a1++) {
                    for (int b0 = 0; b0 < 3; b0++) {
                        for (int b1 = b0 + 1; b1 < 3; b1++) {
                            run_pairpair(groups[group_a][a0], groups[group_a][a1],
                                         groups[group_b][b0], groups[group_b][b1],
                                         rounds);
                        }
                    }
                }
            }
        }
    }
}

static void print_no_pagemap_results(void) {
    printf("same_pa_full\t-1\n");
    printf("same_pa_no_trigger\t-1\n");
    printf("same_pa_no_trainer\t-1\n");
    for (int bit = MIN_DIFF_BIT; bit <= MAX_DIFF_BIT; bit++) {
        printf("bit_%d_full\t-1\n", bit);
    }
    for (int bit_a = MIN_DIFF_BIT; bit_a <= 33; bit_a++) {
        for (int bit_b = bit_a + 1; bit_b <= 33; bit_b++) {
            printf("bits_%d_%d_full\t-1\n", bit_a, bit_b);
        }
    }
    for (int group = 0; group < 6; group++) {
        int base = 16 + group;
        printf("triple_%d_%d_%d_full\t-1\n", base, base + 6, base + 12);
    }
    for (int group_a = 0; group_a < 6; group_a++) {
        for (int group_b = group_a + 1; group_b < 6; group_b++) {
            int a[3] = {16 + group_a, 22 + group_a, 28 + group_a};
            int b[3] = {16 + group_b, 22 + group_b, 28 + group_b};
            for (int a0 = 0; a0 < 3; a0++) {
                for (int a1 = a0 + 1; a1 < 3; a1++) {
                    for (int b0 = 0; b0 < 3; b0++) {
                        for (int b1 = b0 + 1; b1 < 3; b1++) {
                            printf("pairpair_%d_%d__%d_%d_full\t-1\n",
                                   a[a0], a[a1], b[b0], b[b1]);
                        }
                    }
                }
            }
        }
    }
}

int main(void) {
    size_t buddy_bytes = (size_t)BUDDY_PAGES * PAGE_SIZE;
    int valid_pages;

    if (STRIDE_LINES <= 0 || STORE_ACCESSES < 2 ||
        ROUNDS <= 0 || PROBE_POSITIONS <= 0) {
        fprintf(stderr,
                "invalid STRIDE_LINES/STORE_ACCESSES/ROUNDS/PROBE_POSITIONS\n");
        return 1;
    }
    if (MIN_DIFF_BIT < 12 || MAX_DIFF_BIT < MIN_DIFF_BIT || MAX_DIFF_BIT >= 63) {
        fprintf(stderr, "PA diff bit range must be within [12, 62]\n");
        return 1;
    }
    if (PREDICTED_POS >= PROBE_POSITIONS) {
        fprintf(stderr,
                "predicted probe position %d must be inside PROBE_POSITIONS=%d\n",
                PREDICTED_POS,
                PROBE_POSITIONS);
        return 1;
    }
    if ((PREDICTED_POS + 1) * LINE_SIZE > ALIAS_SIZE) {
        fprintf(stderr, "predicted probe position exceeds one page\n");
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

    printf("# store-stride PA xor-index contribution test\n");
    printf("# buddy_pages=%d buddy_bytes=%" PRIu64 " stride_lines=%d accesses=%d train_only_accesses=%d rounds=%d probe_positions=%d\n",
           BUDDY_PAGES,
           (uint64_t)buddy_bytes,
           STRIDE_LINES,
           STORE_ACCESSES,
           TRAIN_ONLY_ACCESSES,
           ROUNDS,
           PROBE_POSITIONS);
    printf("# pair condition: PA1 ^ PA2 == mask, with identical page offset\n");
    printf("# single-bit controls cover MIN_DIFF_BIT..MAX_DIFF_BIT; pairwise xor masks cover MIN_DIFF_BIT..33\n");
    printf("# targeted checks: triple groups should miss; pairpair cross-group masks should hit for the candidate xor hash\n");
    printf("# trainer: PA1 + [0..%d]*%d*LINE_SIZE, trigger: PA2 + %d*LINE_SIZE, final: PA2 + %d*LINE_SIZE\n",
           TRAIN_ONLY_ACCESSES - 1,
           STRIDE_LINES,
           TRIGGER_POS,
           PREDICTED_POS);
    printf("case\tlatency_ns\n");

    if (open_pagemap() != 0) {
        fprintf(stderr, "open /proc/self/pagemap failed: %s\n", strerror(errno));
        fprintf(stderr, "run with sudo or CAP_SYS_ADMIN to expose PFNs\n");
        print_no_pagemap_results();
        return 0;
    }

    valid_pages = collect_page_info();
    if (valid_pages <= 0) {
        fprintf(stderr, "no valid PFNs from pagemap; run with sudo or CAP_SYS_ADMIN\n");
        print_no_pagemap_results();
        return 0;
    }
    printf("# valid_pages=%d\n", valid_pages);

    run_same_pa_baselines(ROUNDS);
    for (int bit = MIN_DIFF_BIT; bit <= MAX_DIFF_BIT; bit++) {
        run_bit(bit, ROUNDS);
    }
    for (int bit_a = MIN_DIFF_BIT; bit_a <= 33; bit_a++) {
        for (int bit_b = bit_a + 1; bit_b <= 33; bit_b++) {
            run_bit_pair(bit_a, bit_b, ROUNDS);
        }
    }
    run_targeted_hash_cases(ROUNDS);

    if (pagemap_fd >= 0) {
        close(pagemap_fd);
    }
    munmap(buddy_pool, buddy_bytes);
    munmap(dummy_buffer, DUMMY_BUFFER_SIZE);
    free(pages);
    return 0;
}
