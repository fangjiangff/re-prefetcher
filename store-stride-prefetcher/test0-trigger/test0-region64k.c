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

#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 5
#endif

#ifndef ROUNDS
#define ROUNDS 40000
#endif

#ifndef BUDDY_PAGES
#define BUDDY_PAGES 16384
#endif

#define REGION_SIZE (64UL * 1024UL)
#define REGION_PAGES (REGION_SIZE / PAGE_SIZE)
#define PAGE_LINES (PAGE_SIZE / LINE_SIZE)

struct page_info {
    uint8_t *va;
    uint64_t pa;
};

struct boundary {
    const char *name;
    uint8_t *before;
    uint8_t *after;
    uint64_t before_pa;
    uint64_t after_pa;
};

enum test_mode {
    SAME_PAGE_BASELINE,
    CROSS_REQUEST,
    RESUME_AFTER_BOUNDARY,
    TRIGGER_ONLY,
};

static uint8_t *buddy_pool;
static struct page_info *pages;
static int page_count;
static int pagemap_fd = -1;

static int open_pagemap(void) {
    pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    return pagemap_fd >= 0 ? 0 : -1;
}

static int virt_to_phys(void *addr, uint64_t *pa) {
    uint64_t entry;
    uintptr_t va = (uintptr_t)addr;
    off_t offset = (off_t)((va / PAGE_SIZE) * sizeof(entry));
    ssize_t got = pread(pagemap_fd, &entry, sizeof(entry), offset);

    if (got != (ssize_t)sizeof(entry) || ((entry >> 63) & 1ULL) == 0) {
        return -1;
    }

    uint64_t pfn = entry & ((1ULL << 55) - 1ULL);
    if (pfn == 0) {
        return -1;
    }

    *pa = pfn * PAGE_SIZE + va % PAGE_SIZE;
    return 0;
}

static int compare_page_pa(const void *left, const void *right) {
    const struct page_info *a = left;
    const struct page_info *b = right;
    return (a->pa > b->pa) - (a->pa < b->pa);
}

static int collect_pages(void) {
    int valid = 0;

    pages = calloc(BUDDY_PAGES, sizeof(*pages));
    if (pages == NULL) {
        return -1;
    }

    for (int i = 0; i < BUDDY_PAGES; i++) {
        uint8_t *va = buddy_pool + (size_t)i * PAGE_SIZE;
        uint64_t pa;
        memset(va, i & 0xff, PAGE_SIZE);
        if (virt_to_phys(va, &pa) == 0) {
            pages[valid].va = va;
            pages[valid].pa = pa & ~(uint64_t)(PAGE_SIZE - 1);
            valid++;
        }
    }

    qsort(pages, valid, sizeof(*pages), compare_page_pa);
    page_count = valid;
    return valid;
}

static int find_page(uint64_t pa) {
    int lo = 0;
    int hi = page_count - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (pages[mid].pa == pa) {
            return mid;
        }
        if (pages[mid].pa < pa) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/* Find 17 consecutive physical pages starting at a 64 KiB boundary. */
static int find_region_pair(struct boundary *intra, struct boundary *inter) {
    for (int i = 0; i < page_count; i++) {
        uint64_t base = pages[i].pa;
        int indices[REGION_PAGES + 1];
        int complete = 1;

        if ((base & (REGION_SIZE - 1)) != 0) {
            continue;
        }
        for (int page = 0; page <= (int)REGION_PAGES; page++) {
            indices[page] = find_page(base + (uint64_t)page * PAGE_SIZE);
            if (indices[page] < 0) {
                complete = 0;
                break;
            }
        }
        if (!complete) {
            continue;
        }

        intra->name = "intra64k_p7_to_p8";
        intra->before = pages[indices[7]].va;
        intra->after = pages[indices[8]].va;
        intra->before_pa = pages[indices[7]].pa;
        intra->after_pa = pages[indices[8]].pa;

        inter->name = "inter64k_p15_to_p0";
        inter->before = pages[indices[15]].va;
        inter->after = pages[indices[16]].va;
        inter->before_pa = pages[indices[15]].pa;
        inter->after_pa = pages[indices[16]].pa;
        return 1;
    }
    return 0;
}

static void flush_two_pages(uint8_t *a, uint8_t *b) {
    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        flush(a + offset);
        if (b != a) {
            flush(b + offset);
        }
    }
    mfence();
}

/* All training and boundary-trigger stores must have the same instruction PC. */
__attribute__((noinline)) static void stride_store(uint8_t *addr) {
    mStore_inline(addr);
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t start = timestamp();
    mStore_inline(addr);
    return timestamp() - start;
}

static void prefetch_wait(void) {
    for (int i = 0; i < 100; i++) {
        nop();
    }
}

static uint64_t run_case(struct boundary boundary, enum test_mode mode) {
    uint64_t latency_sum = 0;
    const int boundary_start_line = PAGE_LINES - TRAIN_ACCESSES * STRIDE_LINES;
    const int baseline_start_line = 8;

    for (uint64_t round = 0; round < ROUNDS; round++) {
        uint8_t *probe;

        cpp_rctx();
        flush_two_pages(boundary.before, boundary.after);

        if (mode == SAME_PAGE_BASELINE) {
            for (int access = 0; access < TRAIN_ACCESSES; access++) {
                stride_store(boundary.before +
                             (baseline_start_line + access * STRIDE_LINES) * LINE_SIZE);
            }
            probe = boundary.before +
                    (baseline_start_line + TRAIN_ACCESSES * STRIDE_LINES) * LINE_SIZE;
        } else {
            if (mode != TRIGGER_ONLY) {
                for (int access = 0; access < TRAIN_ACCESSES; access++) {
                    stride_store(boundary.before +
                                 (boundary_start_line + access * STRIDE_LINES) * LINE_SIZE);
                }
            }

            if (mode == RESUME_AFTER_BOUNDARY || mode == TRIGGER_ONLY) {
                /* This is exactly the next address in the physical stride. */
                stride_store(boundary.after);
                probe = boundary.after + STRIDE_LINES * LINE_SIZE;
            } else {
                /* Did the hardware issue the predicted request across 4 KiB? */
                probe = boundary.after;
            }
        }

        prefetch_wait();
        latency_sum += probe_latency(probe);
    }

    return latency_sum / ROUNDS;
}

static void print_boundary_results(struct boundary boundary) {
    printf("# boundary\t%s\tbefore_va=%p\tbefore_pa=0x%016" PRIx64
           "\tafter_va=%p\tafter_pa=0x%016" PRIx64
           "\tpa_xor=0x%016" PRIx64 "\n",
           boundary.name,
           boundary.before, boundary.before_pa,
           boundary.after, boundary.after_pa,
           boundary.before_pa ^ boundary.after_pa);
    printf("%s_cross_request\t%" PRIu64 "\n", boundary.name,
           run_case(boundary, CROSS_REQUEST));
    printf("%s_resume\t%" PRIu64 "\n", boundary.name,
           run_case(boundary, RESUME_AFTER_BOUNDARY));
    printf("%s_trigger_only\t%" PRIu64 "\n", boundary.name,
           run_case(boundary, TRIGGER_ONLY));
}

int main(void) {
    size_t buddy_bytes = (size_t)BUDDY_PAGES * PAGE_SIZE;
    struct boundary intra = {0};
    struct boundary inter = {0};

    if (STRIDE_LINES <= 0 || TRAIN_ACCESSES < 2 ||
        TRAIN_ACCESSES * STRIDE_LINES >= PAGE_LINES ||
        8 + TRAIN_ACCESSES * STRIDE_LINES >= PAGE_LINES) {
        fprintf(stderr, "stride/access combination does not fit in one page\n");
        return 1;
    }

    buddy_pool = mmap(NULL, buddy_bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB, -1, 0);
    if (buddy_pool == MAP_FAILED) {
        perror("mmap buddy_pool");
        return 1;
    }
    if (open_pagemap() != 0) {
        fprintf(stderr, "open /proc/self/pagemap failed: %s\n", strerror(errno));
        fprintf(stderr, "run with sudo or CAP_SYS_ADMIN to expose PFNs\n");
        return 2;
    }
    if (collect_pages() <= 0) {
        fprintf(stderr, "no PFNs exposed by pagemap; run with sudo or CAP_SYS_ADMIN\n");
        return 2;
    }
    if (!find_region_pair(&intra, &inter)) {
        fprintf(stderr,
                "could not find 17 physically consecutive pages aligned to 64 KiB; "
                "increase BUDDY_PAGES or retry\n");
        return 3;
    }

    printf("# 64-KiB-region / 4-KiB-request-boundary test\n");
    printf("# stride_lines=%d train_accesses=%d rounds=%d valid_pages=%d\n",
           STRIDE_LINES, TRAIN_ACCESSES, ROUNDS, page_count);
    printf("# expected if hypothesis holds: baseline=hit, both cross_request=miss, "
           "intra64k_resume=hit, inter64k_resume=miss, trigger_only=miss\n");
    printf("case\tlatency\n");
    printf("same_page_baseline\t%" PRIu64 "\n",
           run_case(intra, SAME_PAGE_BASELINE));
    print_boundary_results(intra);
    print_boundary_results(inter);

    close(pagemap_fd);
    munmap(buddy_pool, buddy_bytes);
    free(pages);
    return 0;
}
