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

/* Test hypothesis: PA[12:15] is a 16-set index and PA[16:33] is the tag. */
#ifndef MAX_COMPETITORS
#define MAX_COMPETITORS 32
#endif
#ifndef ROUNDS
#define ROUNDS 4000
#endif
#ifndef CANDIDATE_PAGES
#define CANDIDATE_PAGES 8192
#endif
#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif
#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 4
#endif
#ifndef TRIGGER_ACCESSES
#define TRIGGER_ACCESSES 1
#endif

#define INDEX_SHIFT 12
#define INDEX_MASK 0xfU
#define TAG_SHIFT 16
#define TAG_MASK ((1ULL << 18) - 1)
#define PRESENT (1ULL << 63)
#define PFN_MASK ((1ULL << 55) - 1)
#define PREDICTED_LINE ((TRAIN_ACCESSES + TRIGGER_ACCESSES) * STRIDE_LINES)
#define LINES_PER_PAGE (PAGE_SIZE / LINE_SIZE)

struct page {
    uint8_t *va;
    uint64_t pa;
    uint64_t tag;
    unsigned index;
};

static uint8_t *pool;
static size_t pool_size;
static struct page victim;
static struct page *same_set, *different_set;
static int same_count, different_count;

static int virt_to_phys(int fd, void *address, uint64_t *pa) {
    uintptr_t va = (uintptr_t)address;
    uint64_t entry;
    off_t off = (off_t)((va / PAGE_SIZE) * sizeof(entry));
    if (pread(fd, &entry, sizeof(entry), off) != (ssize_t)sizeof(entry) ||
        !(entry & PRESENT) || !(entry & PFN_MASK))
        return -1;
    *pa = (entry & PFN_MASK) * PAGE_SIZE + (va & (PAGE_SIZE - 1));
    return 0;
}

static struct page page_info(uint8_t *va, uint64_t pa) {
    struct page p = {va, pa, (pa >> TAG_SHIFT) & TAG_MASK,
                     (unsigned)((pa >> INDEX_SHIFT) & INDEX_MASK)};
    return p;
}

static int has_tag(struct page *pages, int count, uint64_t tag) {
    for (int i = 0; i < count; i++)
        if (pages[i].tag == tag) return 1;
    return 0;
}

static int select_pages(int wanted) {
    int fd = open("/proc/self/pagemap", O_RDONLY);
    uint64_t pa;
    if (fd < 0) {
        fprintf(stderr, "open pagemap: %s\n", strerror(errno));
        return -1;
    }
    if (virt_to_phys(fd, pool, &pa)) {
        fprintf(stderr, "PFN unavailable; run as root or with CAP_SYS_ADMIN\n");
        close(fd);
        return -1;
    }
    victim = page_info(pool, pa);
    same_set = calloc((size_t)wanted, sizeof(*same_set));
    different_set = calloc((size_t)wanted, sizeof(*different_set));
    if (!same_set || !different_set) {
        perror("calloc");
        close(fd);
        return -1;
    }

    for (int i = 1; i < CANDIDATE_PAGES &&
         (same_count < wanted || different_count < wanted); i++) {
        uint8_t *va = pool + (size_t)i * PAGE_SIZE;
        if (virt_to_phys(fd, va, &pa)) continue;
        struct page p = page_info(va, pa);
        if (p.tag == victim.tag) continue;
        if (p.index == victim.index && same_count < wanted &&
            !has_tag(same_set, same_count, p.tag))
            same_set[same_count++] = p;
        else if (p.index != victim.index && different_count < wanted &&
                 !has_tag(different_set, different_count, p.tag))
            different_set[different_count++] = p;
    }
    close(fd);
    if (same_count < wanted || different_count < wanted) {
        fprintf(stderr, "found same=%d different=%d, need=%d; increase CANDIDATE_PAGES\n",
                same_count, different_count, wanted);
        return -1;
    }
    return 0;
}


static void flush_page_stream(struct page *p) {
    for (int i = 0; i < LINES_PER_PAGE; i++)
        flush(p->va + (size_t)i * LINE_SIZE);
}

static __attribute__((noinline)) void victim_store(void *addr) {
    mStore_inline(addr);
}

static __attribute__((noinline)) void competitor_store(void *addr) {
    mStore_inline(addr);
}

static void train_victim(struct page *p) {
    for (int i = 0; i < TRAIN_ACCESSES; i++)
        victim_store(p->va + (size_t)i * STRIDE_LINES * LINE_SIZE);
}

static void train_competitor(struct page *p) {
    for (int i = 0; i < TRAIN_ACCESSES; i++)
        competitor_store(p->va + (size_t)i * STRIDE_LINES * LINE_SIZE);
}

static uint64_t one_round(struct page *competitors, int count, int trigger) {
    flush_page_stream(&victim);

    for (int i = 0; i < count; i++)
        flush_page_stream(&competitors[i]);
    mfence();

#ifdef __aarch64__
    cpp_rctx();
#endif

    // trainer sequence
    // train_victim(&victim);
        mStore_inline(victim.va + (0 * STRIDE_LINES * LINE_SIZE));
        mStore_inline(victim.va + (1 * STRIDE_LINES * LINE_SIZE));
        mStore_inline(victim.va + (2 * STRIDE_LINES * LINE_SIZE));
         mStore_inline(victim.va + (3 * STRIDE_LINES * LINE_SIZE));

    // co
    for (int i = 0; i < count; i++) train_competitor(&competitors[i]);
    
    // trigger sequence
    if (trigger) {
        // for (int i = 0; i < TRIGGER_ACCESSES; i++) {
        //     int pos = TRAIN_ACCESSES + i;
        //     mStore_inline(victim.va + (size_t)pos * STRIDE_LINES * LINE_SIZE);
        //     // nops();
        // }
       
        mStore_inline(victim.va + (4 * STRIDE_LINES * LINE_SIZE));
        mStore_inline(victim.va + (5 * STRIDE_LINES * LINE_SIZE));
    }
    //probe
    // uint8_t *probe = victim.va + (size_t)PREDICTED_LINE * LINE_SIZE;
     uint8_t *probe = victim.va + (6 * STRIDE_LINES * LINE_SIZE);
    uint64_t start = timestamp();
    mStore_inline(probe);
    return timestamp() - start;
}

static void measure(const char *mode, struct page *pages, int count,
                    int rounds, int trigger) {
    uint64_t sum = 0;
    for (int i = 0; i < rounds; i++) sum += one_round(pages, count, trigger);
    printf("%s\t%d\t%" PRIu64 "\t%d\n", mode, count,
           sum / (uint64_t)rounds, rounds);
}

static void print_page(const char *kind, int n, const struct page *p) {
    printf("# %s[%d] va=%p pa=0x%016" PRIx64
           " index12_15=0x%x tag16_33=0x%05" PRIx64 "\n",
           kind, n, (void *)p->va, p->pa, p->index, p->tag);
}

int main(int argc, char **argv) {
    int maximum = argc > 1 ? atoi(argv[1]) : MAX_COMPETITORS;
    int rounds = argc > 2 ? atoi(argv[2]) : ROUNDS;
    if (argc > 3 || maximum < 1 || maximum > MAX_COMPETITORS || rounds < 1) {
        fprintf(stderr, "usage: %s [max_competitors<=%d [rounds]]\n",
                argv[0], MAX_COMPETITORS);
        return 1;
    }
    if (sysconf(_SC_PAGESIZE) != PAGE_SIZE) {
        fprintf(stderr, "requires %d-byte pages\n", PAGE_SIZE);
        return 1;
    }
    if (PREDICTED_LINE >= PAGE_SIZE / LINE_SIZE) {
        fprintf(stderr, "test stream crosses a 4KB page\n");
        return 1;
    }

    pool_size = (size_t)CANDIDATE_PAGES * PAGE_SIZE;
    pool = mmap(NULL, pool_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (pool == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        return 1;
    }
    memset(pool, -1, pool_size);
    if (select_pages(maximum)) return 1;

    printf("# store-stride associativity test\n");
    printf("# hypothesis: PA[12:15]=index, PA[16:33]=tag\n");
    printf("# same_set isolates associativity; different_set is capacity/cache control\n");
    printf("# stride_lines=%d train=%d trigger=%d predicted_line=%d rounds=%d\n",
           STRIDE_LINES, TRAIN_ACCESSES, TRIGGER_ACCESSES, PREDICTED_LINE, rounds);
    printf("# timer=%s unit=%s\n", TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME);
    print_page("victim", 0, &victim);
    for (int i = 0; i < maximum; i++) print_page("same", i, &same_set[i]);
    for (int i = 0; i < maximum; i++) print_page("different", i, &different_set[i]);
    printf("mode\tcompetitors\tavg_%s\tprobes\n", TIMESTAMP_UNIT_NAME);

    measure("no_trigger", same_set, 0, rounds, 0);
    for (int n = 0; n <= maximum; n++) {
        measure("same_set", same_set, n, rounds, 1);
        measure("different_set", different_set, n, rounds, 1);
    }

    free(same_set);
    free(different_set);
    munmap(pool, pool_size);
    return 0;
}
