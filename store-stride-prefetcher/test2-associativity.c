#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include "until.h"

#define DUMMY_BUFFER_PAGES 10

#define DEFAULT_STORE_PC 0x500000120ull
#define DEFAULT_POOL_MIB 16384
#define DEFAULT_ROUNDS 1000
#define DEFAULT_MATCH_BITS 4
#define DEFAULT_GUESSES "4,8,16,32,64"
#define MAX_GUESSES 32
#define MAX_MATCH_BITS 16

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#define DEFAULT_STRIDE (STRIDE_LINES * LINE_SIZE)

#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 4
#endif

#ifndef COMPETITOR_ACCESSES
#define COMPETITOR_ACCESSES (TRAIN_ACCESSES)
#endif

#define VICTIM_TRIGGER0_LINE (TRAIN_ACCESSES * STRIDE_LINES)
#define VICTIM_TRIGGER1_LINE ((TRAIN_ACCESSES + 1) * STRIDE_LINES)
#define VICTIM_PROBE_LINE ((TRAIN_ACCESSES + 2) * STRIDE_LINES)

typedef void (*store_gadget_f)(void *);

static uint8_t array1[100 * LINE_SIZE] = {0};
static uint8_t *dummy_buffer;
static size_t page_size;
static size_t dummy_buffer_size;

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

struct selected_page {
    uint8_t *va;
    uint64_t pa;
};

static uintptr_t line_base(uintptr_t address) {
    return address & ~((uintptr_t)LINE_SIZE - 1);
}

static void flush_line_addr(void *addr) {
    flush((void *)line_base((uintptr_t)addr));
}

static void dummy_accesses(void) {
    dummyAccess(dummy_buffer, dummy_buffer_size);
}

static void delay_after_trigger(void) {
    uint64_t dummy = 0;

    for (int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
    }
    for (int i = 0; i < 100; i++) {
        nop();
    }

    (void)dummy;
}

static uint64_t probe_latency(uint8_t *addr) {
    uint64_t time1 = timestamp();
    maccess(addr);
    uint64_t time2 = timestamp();

    return time2 - time1;
}

static void flush_page_stride_lines(uint8_t *page, int stride) {
    for (int step = 0; step <= TRAIN_ACCESSES + 2; step++) {
        flush_line_addr(page + ((size_t)step * (size_t)stride));
    }
}

static void train_page(store_gadget_f store_gadget,
                       uint8_t *page,
                       int accesses,
                       int stride) {
    for (int step = 0; step < accesses; step++) {
        store_gadget(page + ((size_t)step * (size_t)stride));
    }
}

static void trigger_victim(store_gadget_f store_gadget,
                           uint8_t *victim,
                           int stride) {
    store_gadget(victim + ((size_t)VICTIM_TRIGGER0_LINE * LINE_SIZE));
    store_gadget(victim + ((size_t)VICTIM_TRIGGER1_LINE * LINE_SIZE));
}

static store_gadget_f map_store_gadget(uintptr_t address) {
    size_t gadget_size =
        (size_t)(_store_gadget_asm_end - _store_gadget_asm_start);

    uintptr_t base = address - (address % page_size);
    size_t map_size = ((address + gadget_size) - base);
    if (map_size % page_size) {
        map_size += page_size - (map_size % page_size);
    }

    uint8_t *code = mmap((void *)base,
                         map_size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_FIXED_NOREPLACE | MAP_ANONYMOUS |
                             MAP_PRIVATE | MAP_POPULATE,
                         -1,
                         0);
    if (code == MAP_FAILED) {
        fprintf(stderr,
                "mmap store gadget at 0x%016lx size 0x%lx failed: %s\n",
                (unsigned long)base,
                (unsigned long)map_size,
                strerror(errno));
        return NULL;
    }

    memcpy((void *)address, _store_gadget_asm_start, gadget_size);
    __builtin___clear_cache((char *)address,
                            (char *)(address + gadget_size));

    return (store_gadget_f)(void *)address;
}

static int read_phys_addr(int pagemap_fd, uintptr_t va, uint64_t *pa) {
    uint64_t entry = 0;
    off_t offset = (off_t)((va / page_size) * sizeof(entry));
    ssize_t got = pread(pagemap_fd, &entry, sizeof(entry), offset);

    if (got != (ssize_t)sizeof(entry)) {
        return -1;
    }
    if ((entry & (1ull << 63)) == 0) {
        return -1;
    }

    uint64_t pfn = entry & ((1ull << 55) - 1);
    if (pfn == 0) {
        errno = EPERM;
        return -1;
    }

    *pa = (pfn * page_size) | (va & (page_size - 1));
    return 0;
}

static int parse_guesses(const char *text, int *guesses, int *count) {
    char buffer[256];
    char *save = NULL;
    char *token = NULL;

    if (strlen(text) >= sizeof(buffer)) {
        return -1;
    }

    strcpy(buffer, text);
    *count = 0;

    for (token = strtok_r(buffer, ",", &save);
         token;
         token = strtok_r(NULL, ",", &save)) {
        if (*count >= MAX_GUESSES) {
            return -1;
        }

        int value = atoi(token);
        if (value < 0) {
            return -1;
        }

        guesses[*count] = value;
        (*count)++;
    }

    return *count > 0 ? 0 : -1;
}

static int select_same_color_pages(uint8_t *pool,
                                   size_t pool_pages,
                                   int match_bits,
                                   int needed,
                                   struct selected_page *selected,
                                   uint64_t *selected_color) {
    int colors = 1 << match_bits;
    int *counts = calloc((size_t)colors, sizeof(*counts));
    struct selected_page *buckets =
        calloc((size_t)colors * (size_t)needed, sizeof(*buckets));

    if (!counts || !buckets) {
        fprintf(stderr, "failed to allocate page-color buckets\n");
        free(counts);
        free(buckets);
        return -1;
    }

    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) {
        fprintf(stderr, "open /proc/self/pagemap failed: %s\n", strerror(errno));
        free(counts);
        free(buckets);
        return -1;
    }

    for (size_t i = 0; i < pool_pages; i++) {
        uint8_t *va = pool + (i * page_size);
        uint64_t pa = 0;

        if (read_phys_addr(pagemap_fd, (uintptr_t)va, &pa) != 0) {
            fprintf(stderr,
                    "failed to read PFN for %p: %s. "
                    "This test needs readable PFNs, usually root/CAP_SYS_ADMIN.\n",
                    va,
                    strerror(errno));
            close(pagemap_fd);
            free(counts);
            free(buckets);
            return -1;
        }

        uint64_t color = (pa >> 12) & ((1ull << match_bits) - 1);
        int idx = (int)color;

        if (counts[idx] < needed) {
            size_t slot = ((size_t)idx * (size_t)needed) + (size_t)counts[idx];
            buckets[slot].va = va;
            buckets[slot].pa = pa;
            counts[idx]++;

            if (counts[idx] == needed) {
                for (int k = 0; k < needed; k++) {
                    selected[k] = buckets[((size_t)idx * (size_t)needed) +
                                          (size_t)k];
                }
                *selected_color = color;
                close(pagemap_fd);
                free(counts);
                free(buckets);
                return 0;
            }
        }
    }

    int best_color = 0;
    for (int i = 1; i < colors; i++) {
        if (counts[i] > counts[best_color]) {
            best_color = i;
        }
    }

    fprintf(stderr,
            "no PA color had %d pages for match_bits=%d; best color 0x%x had %d pages\n",
            needed,
            match_bits,
            best_color,
            counts[best_color]);

    close(pagemap_fd);
    free(counts);
    free(buckets);
    return -1;
}

static uint64_t run_one_round(store_gadget_f store_gadget,
                              struct selected_page *pages,
                              int competitors,
                              int stride) {
    uint8_t *victim = pages[0].va;
    uint8_t *probe_addr =
        victim + ((size_t)VICTIM_PROBE_LINE * (size_t)LINE_SIZE);

    dummy_accesses();

    flush_page_stride_lines(victim, stride);
    for (int i = 1; i <= competitors; i++) {
        flush_page_stride_lines(pages[i].va, stride);
    }

    train_page(store_gadget, victim, TRAIN_ACCESSES, stride);
    for (int i = 1; i <= competitors; i++) {
        train_page(store_gadget, pages[i].va, COMPETITOR_ACCESSES, stride);
    }

    flush_page_stride_lines(victim, stride);
    trigger_victim(store_gadget, victim, stride);
    delay_after_trigger();

    return probe_latency(probe_addr);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [store_pc pool_mib rounds match_bits [guesses_csv]]\n"
            "default: store_pc=0x%lx pool_mib=%d rounds=%d match_bits=%d guesses=%s\n",
            prog,
            (unsigned long)DEFAULT_STORE_PC,
            DEFAULT_POOL_MIB,
            DEFAULT_ROUNDS,
            DEFAULT_MATCH_BITS,
            DEFAULT_GUESSES);
}

int main(int argc, char **argv) {
    uintptr_t store_pc = DEFAULT_STORE_PC;
    int pool_mib = DEFAULT_POOL_MIB;
    int rounds = DEFAULT_ROUNDS;
    int match_bits = DEFAULT_MATCH_BITS;
    const char *guess_text = DEFAULT_GUESSES;
    int stride = DEFAULT_STRIDE;
    int guesses[MAX_GUESSES];
    int guess_count = 0;

    long detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        fprintf(stderr, "failed to detect OS page size\n");
        return 1;
    }
    page_size = (size_t)detected_page_size;

    if (argc != 1 && argc != 5 && argc != 6) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc >= 5) {
        store_pc = strtoull(argv[1], NULL, 0);
        pool_mib = atoi(argv[2]);
        rounds = atoi(argv[3]);
        match_bits = atoi(argv[4]);
    }
    if (argc == 6) {
        guess_text = argv[5];
    }

    if (pool_mib <= 0 || rounds <= 0 ||
        match_bits < 1 || match_bits > MAX_MATCH_BITS ||
        parse_guesses(guess_text, guesses, &guess_count) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    size_t predicted_offset =
        (size_t)VICTIM_PROBE_LINE * (size_t)LINE_SIZE;
    if (predicted_offset + LINE_SIZE > page_size) {
        fprintf(stderr,
                "victim predicted line exceeds one page: train_accesses=%d stride_lines=%d\n",
                TRAIN_ACCESSES,
                STRIDE_LINES);
        return 1;
    }

    int max_guess = 0;
    for (int i = 0; i < guess_count; i++) {
        if (guesses[i] > max_guess) {
            max_guess = guesses[i];
        }
    }
    int needed_pages = max_guess + 1;

    size_t pool_size = (size_t)pool_mib * 1024ull * 1024ull;
    size_t pool_pages = pool_size / page_size;
    uint8_t *pool = mmap(NULL,
                         pool_size,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1,
                         0);
    if (pool == MAP_FAILED) {
        fprintf(stderr,
                "failed to map %d MiB pool: %s\n",
                pool_mib,
                strerror(errno));
        return 1;
    }

    for (size_t i = 0; i < pool_pages; i++) {
        pool[i * page_size] = (uint8_t)i;
    }

    dummy_buffer_size = page_size * DUMMY_BUFFER_PAGES;
    dummy_buffer = mmap(NULL,
                        dummy_buffer_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1,
                        0);
    if (dummy_buffer == MAP_FAILED) {
        fprintf(stderr, "failed to map dummy buffer: %s\n", strerror(errno));
        return 1;
    }
    memset(dummy_buffer, -1, dummy_buffer_size);

    store_gadget_f store_gadget = map_store_gadget(store_pc);
    if (!store_gadget) {
        return 1;
    }

    struct selected_page *pages =
        calloc((size_t)needed_pages, sizeof(*pages));
    if (!pages) {
        fprintf(stderr, "failed to allocate selected page list\n");
        return 1;
    }

    uint64_t color = 0;
    if (select_same_color_pages(pool,
                                pool_pages,
                                match_bits,
                                needed_pages,
                                pages,
                                &color) != 0) {
        return 1;
    }

    printf("# store-stride associativity test\n");
    printf("# selection: same PA bits [12:%d], match_bits=%d color=0x%lx\n",
           12 + match_bits - 1,
           match_bits,
           (unsigned long)color);
    printf("# pool_mib=%d pool_pages=%lu selected_pages=%d rounds=%d\n",
           pool_mib,
           (unsigned long)pool_pages,
           needed_pages,
           rounds);
    printf("# store_pc=0x%016lx page_size=%lu stride_lines=%d train_accesses=%d competitor_accesses=%d\n",
           (unsigned long)store_pc,
           (unsigned long)page_size,
           STRIDE_LINES,
           TRAIN_ACCESSES,
           COMPETITOR_ACCESSES);
    printf("# victim_pa=0x%016lx victim_va=%p predicted_line=%d\n",
           (unsigned long)pages[0].pa,
           pages[0].va,
           VICTIM_PROBE_LINE);
    printf("# lower avg_ns means victim predicted line was still prefetched\n");
    printf("guess_ways\tcompetitors\tentries_same_color\tcolor\tavg_ns\tprobes\n");

    for (int g = 0; g < guess_count; g++) {
        int competitors = guesses[g];
        uint64_t latency = 0;

        for (int r = 0; r < rounds; r++) {
            latency += run_one_round(store_gadget,
                                     pages,
                                     competitors,
                                     stride);
        }

        printf("%d\t%d\t%d\t0x%lx\t%lu\t%lu\n",
               guesses[g],
               competitors,
               competitors + 1,
               (unsigned long)color,
               (unsigned long)(latency / (uint64_t)rounds),
               (unsigned long)rounds);
    }

    return 0;
}
