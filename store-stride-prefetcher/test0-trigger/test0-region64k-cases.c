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
#define TRAIN_ACCESSES 4
#endif
#ifndef ROUNDS
#define ROUNDS 40000
#endif
#ifndef BUDDY_PAGES
#define BUDDY_PAGES 16384
#endif

#define PAGE_LINES (PAGE_SIZE / LINE_SIZE)
#define REGION_SIZE (64UL * 1024UL)

struct page_info { uint8_t *va; uint64_t pa; };
struct page_set { uint8_t *va[4]; uint64_t pa[4]; };

static uint8_t *pool;
static struct page_info *pages;
static int page_count;
static int pagemap_fd = -1;

static int virt_to_phys(void *addr, uint64_t *pa) {
    uint64_t entry;
    uintptr_t va = (uintptr_t)addr;
    off_t off = (off_t)((va / PAGE_SIZE) * sizeof(entry));
    if (pread(pagemap_fd, &entry, sizeof(entry), off) != (ssize_t)sizeof(entry) ||
        !(entry >> 63)) return -1;
    uint64_t pfn = entry & ((1ULL << 55) - 1ULL);
    if (!pfn) return -1;
    *pa = pfn * PAGE_SIZE + va % PAGE_SIZE;
    return 0;
}

static int page_cmp(const void *a, const void *b) {
    const struct page_info *x = a, *y = b;
    return (x->pa > y->pa) - (x->pa < y->pa);
}

static int find_pa(uint64_t pa) {
    int lo = 0, hi = page_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (pages[mid].pa == pa) return mid;
        if (pages[mid].pa < pa) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int collect_pages(void) {
    pages = calloc(BUDDY_PAGES, sizeof(*pages));
    if (!pages) return -1;
    for (int i = 0; i < BUDDY_PAGES; i++) {
        uint8_t *va = pool + (size_t)i * PAGE_SIZE;
        uint64_t pa;
        memset(va, i & 0xff, PAGE_SIZE);
        if (virt_to_phys(va, &pa) == 0) {
            pages[page_count].va = va;
            pages[page_count].pa = pa & ~(uint64_t)(PAGE_SIZE - 1);
            page_count++;
        }
    }
    qsort(pages, page_count, sizeof(*pages), page_cmp);
    return page_count;
}

static int find_page_set(struct page_set *set) {
    const uint64_t offsets[4] = {0, PAGE_SIZE, 15 * PAGE_SIZE, 16 * PAGE_SIZE};
    for (int i = 0; i < page_count; i++) {
        uint64_t base = pages[i].pa;
        int idx[4] = {-1, -1, -1, -1};
        if (base & (REGION_SIZE - 1)) continue;
        for (int n = 0; n < 4; n++) idx[n] = find_pa(base + offsets[n]);
        if (idx[0] < 0 || idx[1] < 0 || idx[2] < 0 || idx[3] < 0) continue;
        for (int n = 0; n < 4; n++) {
            set->va[n] = pages[idx[n]].va;
            set->pa[n] = pages[idx[n]].pa;
        }
        return 1;
    }
    return 0;
}

static void flush_set(const struct page_set *set) {
    for (int page = 0; page < 4; page++)
        for (size_t off = 0; off < PAGE_SIZE; off += LINE_SIZE)
            flush(set->va[page] + off);
    mfence();
}

__attribute__((noinline)) static void stride_store(uint8_t *addr) { mStore_inline(addr); }

static void train(uint8_t *page, int first_line) {
    for (int n = 0; n < TRAIN_ACCESSES; n++)
        stride_store(page + (first_line + n * STRIDE_LINES) * LINE_SIZE);
}

static uint64_t probe(uint8_t *addr) {
    uint64_t start = timestamp();
    mStore_inline(addr);
    return timestamp() - start;
}

static uint64_t run_case(const struct page_set *s, int id) {
    uint64_t sum = 0;
    for (uint64_t round = 0; round < ROUNDS; round++) {
        uint8_t *trigger, *target;
        cpp_rctx();
        flush_set(s);
        switch (id) {
        case 0: train(s->va[0], 8);  trigger = s->va[0] + 28 * LINE_SIZE; target = s->va[0] + 33 * LINE_SIZE; break;
        case 1: train(s->va[0], 39); trigger = s->va[0] + 59 * LINE_SIZE; target = s->va[1]; break;
        case 2: train(s->va[0], 0);  trigger = s->va[2] + 20 * LINE_SIZE; target = s->va[2] + 25 * LINE_SIZE; break;
        case 3: trigger = s->va[2] + 20 * LINE_SIZE; target = s->va[2] + 25 * LINE_SIZE; break;
        case 4: train(s->va[2], 44); trigger = s->va[3]; target = s->va[3] + 5 * LINE_SIZE; break;
        case 5: trigger = s->va[3]; target = s->va[3] + 5 * LINE_SIZE; break;
        default: return UINT64_MAX;
        }
        stride_store(trigger);
        for (int i = 0; i < 100; i++) nop();
        sum += probe(target);
    }
    return sum / ROUNDS;
}

int main(void) {
    const char *names[6] = {"case0_same_page", "case1_cross_4k_request", "case2_same_64k", "case3_same_64k_no_trainer", "case4_cross_64k", "case5_cross_64k_no_trainer"};
    struct page_set set = {0};
    size_t bytes = (size_t)BUDDY_PAGES * PAGE_SIZE;
    if (STRIDE_LINES != 5 || TRAIN_ACCESSES != 4 || PAGE_LINES < 64) {
        fprintf(stderr, "requires stride=5, train-accesses=4, and 4KB pages\n");
        return 1;
    }
    pool = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (pool == MAP_FAILED) { perror("mmap page pool"); return 1; }
    pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) { perror("open /proc/self/pagemap"); return 2; }
    if (collect_pages() <= 0 || !find_page_set(&set)) {
        fprintf(stderr, "could not find A/B/C/D pages; increase BUDDY_PAGES or retry\n");
        return 3;
    }
    printf("# A/B/C share PA[33:16]; A/B adjacent; C/D adjacent across 64KB\n");
    for (int n = 0; n < 4; n++)
        printf("# page_%c va=%p pa=0x%016" PRIx64 "\n", 'A' + n, set.va[n], set.pa[n]);
    printf("case\tlatency\n");
    for (int id = 0; id < 6; id++) printf("%s\t%" PRIu64 "\n", names[id], run_case(&set, id));
    close(pagemap_fd);
    munmap(pool, bytes);
    free(pages);
    return 0;
}
