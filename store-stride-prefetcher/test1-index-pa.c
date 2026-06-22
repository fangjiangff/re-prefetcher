#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "until.h"

#define ITEMS 16384
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#ifndef TRAIN_STORES
#define TRAIN_STORES 5
#endif

#ifndef REPEAT
#define REPEAT 5
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef TRIGGER_MIN_LINE
#define TRIGGER_MIN_LINE 0
#endif

#ifndef TRIGGER_MAX_LINE
#define TRIGGER_MAX_LINE 128
#endif

#ifndef REF_TRIGGER_LINE
#define REF_TRIGGER_LINE 61
#endif

#ifndef HIT_THRESHOLD_NS
#define HIT_THRESHOLD_NS 120
#endif

#ifndef CPU_ID
#define CPU_ID -1
#endif

#ifndef MAX_PA_BIT
#define MAX_PA_BIT 47
#endif

#ifndef BUDDY_SCAN
#define BUDDY_SCAN 0
#endif

#ifndef BUDDY_PAGES
#define BUDDY_PAGES 16384
#endif

#ifndef BUDDY_CHUNK_PAGES
#define BUDDY_CHUNK_PAGES 16384
#endif

#ifndef ALIAS_SCAN
#define ALIAS_SCAN 0
#endif

#ifndef ALIAS_MIN_M
#define ALIAS_MIN_M 1
#endif

#ifndef ALIAS_MAX_M
#define ALIAS_MAX_M 20
#endif

uint8_t array2[ITEMS * LINE_SIZE] __attribute__((aligned(4096)));
uint8_t array1[100 * LINE_SIZE] = {0};

static uint8_t *dummy_buffer;
static int pagemap_fd = -1;
static size_t os_page_size;

struct case_result {
    int trigger_line;
    uint64_t trigger_va;
    uint64_t trigger_pa;
    uint64_t pa_xor_ref;
    uint64_t avg_ns;
    int prefetched;
    int direct_probe_hit;
    int pa_known;
};

struct bit_result {
    int pa_bit;
    uint8_t *trigger_addr;
    uint64_t trigger_va;
    uint64_t trigger_pa;
    uint64_t pa_xor_ref;
    uint64_t avg_ns;
    int prefetched;
    int candidate_found;
    const char *source;
};

static uint64_t run_trigger_addr_case(uint8_t *trigger_addr,
                                      int trigger_line,
                                      int do_trigger);

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void set_cpu_if_requested(void) {
#if CPU_ID >= 0
    cpu_set_t mask;

    CPU_ZERO(&mask);
    CPU_SET(CPU_ID, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        die("sched_setaffinity");
    }
#endif
}

static void dummy_accesses(void) {
    dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
}

static uint64_t line_offset(int line) {
    return (uint64_t)line * LINE_SIZE;
}

static int probe_line(void) {
    return TRAIN_STORES * STRIDE_LINES;
}

static int is_training_line(int line) {
    for (int step = 0; step < TRAIN_STORES; step++) {
        if (line == step * STRIDE_LINES) {
            return 1;
        }
    }
    return 0;
}

static void warm_lines(void) {
    for (int i = 0; i < ITEMS; i++) {
        mLoad(array2 + line_offset(i));
    }
}

static void flush_line_once(int line) {
    if (line >= 0 && line < ITEMS) {
        flush(array2 + line_offset(line));
    }
}

static void flush_experiment_lines(int trigger_line) {
    for (int step = 0; step < TRAIN_STORES; step++) {
        flush_line_once(step * STRIDE_LINES);
    }
    flush_line_once(trigger_line);
    flush_line_once(probe_line());
    mfence();
}

static void train_stream(void) {
    for (int step = 0; step < TRAIN_STORES; step++) {
        mStore_noinline(array2 + ((uint64_t)step * STRIDE_LINES * LINE_SIZE));
    }
}

static void delay_after_trigger(void) {
    volatile uint64_t dummy = 0;

    for (int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
    }
    for (int i = 0; i < 100; i++) {
        nop();
    }
    (void)dummy;
}

static uint64_t probe_latency(void) {
    volatile uint8_t *probe_addr = array2 + line_offset(probe_line());
    uint64_t time1;
    uint64_t time2;
    volatile unsigned int junk;

    time1 = timestamp();
    junk = *probe_addr;
    time2 = timestamp() - time1;
    (void)junk;
    return time2;
}

static uint64_t run_trigger_case(int trigger_line, int do_trigger) {
    return run_trigger_addr_case(array2 + line_offset(trigger_line),
                                 trigger_line,
                                 do_trigger);
}

static uint64_t run_trigger_addr_case(uint8_t *trigger_addr,
                                      int trigger_line,
                                      int do_trigger) {
    uint64_t latency_sum = 0;

    for (uint64_t round = 0; round < ROUNDS; round++) {
        dummy_accesses();
        flush_experiment_lines(trigger_line);
        flush(trigger_addr);
        mfence();

        for (int repeat = 0; repeat < REPEAT; repeat++) {
            train_stream();
            if (do_trigger) {
                mStore_noinline(trigger_addr);
            }
        }

        delay_after_trigger();
        latency_sum += probe_latency();
    }

    return latency_sum / ROUNDS;
}

static int open_pagemap(void) {
    pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) {
        return -1;
    }
    return 0;
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

    offset = (off_t)((va / os_page_size) * sizeof(entry));
    got = pread(pagemap_fd, &entry, sizeof(entry), offset);
    if (got != (ssize_t)sizeof(entry)) {
        return -1;
    }
    if (((entry >> 63) & 1) == 0) {
        return -1;
    }

    pfn = entry & ((1ULL << 55) - 1);
    if (pfn == 0) {
        return -1;
    }

    *pa = (pfn * os_page_size) + (va % os_page_size);
    return 0;
}

static void print_bit_list(uint64_t value) {
    int first = 1;

    if (value == 0) {
        printf("-");
        return;
    }

    for (int bit = 0; bit <= MAX_PA_BIT; bit++) {
        if ((value >> bit) & 1ULL) {
            if (!first) {
                printf(",");
            }
            printf("%d", bit);
            first = 0;
        }
    }
}

static int is_power_of_two_u64(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static int bit_index_u64(uint64_t value) {
    int bit = 0;

    while ((value & 1ULL) == 0) {
        value >>= 1;
        bit++;
    }
    return bit;
}

static void print_header(uint64_t no_trigger_avg,
                         uint64_t ref_avg,
                         int ref_pa_known,
                         uint64_t ref_pa) {
    printf("# arm64 A55 store-stride PA-index trigger sweep\n");
    printf("# train stores: array2 + {0,5,10,15,20} * LINE_SIZE when STRIDE_LINES=5\n");
    printf("# trigger: same mStore_noinline PC, variable target line\n");
    printf("# probe: array2 + %d * LINE_SIZE, expected prefetch target after stride-5 training\n",
           probe_line());
    printf("# STRIDE_LINES=%d TRAIN_STORES=%d REPEAT=%d ROUNDS=%d threshold_ns=%d CPU_ID=%d\n",
           STRIDE_LINES, TRAIN_STORES, REPEAT, ROUNDS, HIT_THRESHOLD_NS,
           CPU_ID);
    printf("# trigger_line_range=%d..%d ref_trigger_line=%d\n",
           TRIGGER_MIN_LINE, TRIGGER_MAX_LINE, REF_TRIGGER_LINE);
    printf("# no_trigger_avg_ns=%" PRIu64 " ref_trigger_avg_ns=%" PRIu64 "\n",
           no_trigger_avg, ref_avg);
    if (ref_pa_known) {
        printf("# ref_trigger_pa=0x%016" PRIx64 "\n", ref_pa);
    } else {
        printf("# ref_trigger_pa=unknown; high PA-bit inference is unavailable without /proc/self/pagemap PFNs\n");
    }
    printf("# prefetched=yes means avg_ns <= threshold and trigger line is not the probed line\n");
    printf("trigger_line\ttrigger_offset\ttrigger_va\ttrigger_pa\tpa_xor_ref\tchanged_pa_bits\tavg_ns\tprefetched\tnote\n");
}

static void print_case_result(const struct case_result *result) {
    printf("%d\t%" PRIu64 "\t0x%016" PRIx64 "\t",
           result->trigger_line,
           line_offset(result->trigger_line),
           result->trigger_va);

    if (result->pa_known) {
        printf("0x%016" PRIx64 "\t0x%016" PRIx64 "\t",
               result->trigger_pa, result->pa_xor_ref);
        print_bit_list(result->pa_xor_ref);
    } else {
        printf("unknown\tunknown\tunknown");
    }

    printf("\t%" PRIu64 "\t%s\t",
           result->avg_ns,
           result->prefetched ? "yes" : "no");

    if (result->direct_probe_hit) {
        printf("direct_probe_line");
    } else if (is_training_line(result->trigger_line)) {
        printf("training_line");
    } else if (result->trigger_line == REF_TRIGGER_LINE) {
        printf("reference");
    } else {
        printf("-");
    }
    printf("\n");
}

static void print_bit_summary(const struct case_result *results, int count) {
    int seen[MAX_PA_BIT + 1];
    int prefetched[MAX_PA_BIT + 1];

    memset(seen, 0, sizeof(seen));
    memset(prefetched, 0, sizeof(prefetched));

    for (int i = 0; i < count; i++) {
        if (!results[i].pa_known ||
            results[i].direct_probe_hit ||
            !is_power_of_two_u64(results[i].pa_xor_ref)) {
            continue;
        }

        int bit = bit_index_u64(results[i].pa_xor_ref);
        if (bit > MAX_PA_BIT) {
            continue;
        }
        seen[bit] = 1;
        if (results[i].prefetched) {
            prefetched[bit] = 1;
        }
    }

    printf("\n");
    printf("# single-bit PA verdicts relative to ref_trigger_line=%d\n",
           REF_TRIGGER_LINE);
    printf("# yes => this isolated PA-bit change did not break triggering, so this bit is not required by the tested index/match condition\n");
    printf("# no  => this isolated PA-bit change broke triggering, so this bit likely participates in the tested index/match condition\n");
    printf("pa_bit\tisolated_case\tverdict\n");

    for (int bit = 0; bit <= MAX_PA_BIT; bit++) {
        if (!seen[bit]) {
            printf("%d\tno\tinsufficient_case\n", bit);
        } else {
            printf("%d\tyes\t%s\n",
                   bit,
                   prefetched[bit] ? "not_participating" : "participating");
        }
    }
}

static int page_offset_bits(void) {
    int bits = 0;
    size_t value = os_page_size;

    while (value > 1) {
        value >>= 1;
        bits++;
    }
    return bits;
}

static int address_in_array2(uint8_t *addr) {
    return addr >= array2 && addr < array2 + sizeof(array2);
}

static uint8_t *find_addr_with_pa(uint8_t *base,
                                  size_t bytes,
                                  uint64_t target_pa) {
    uintptr_t page_mask = ~((uintptr_t)os_page_size - 1);
    uintptr_t target_offset = target_pa % os_page_size;

    for (size_t offset = 0; offset < bytes; offset += os_page_size) {
        uint8_t *page = base + offset;
        uint64_t page_pa;
        uint8_t *candidate;
        uint64_t candidate_pa;

        if (virt_to_phys(page, &page_pa) != 0) {
            continue;
        }

        page_pa &= (uint64_t)page_mask;
        if (page_pa != (target_pa & (uint64_t)page_mask)) {
            continue;
        }

        candidate = page + target_offset;
        if (virt_to_phys(candidate, &candidate_pa) == 0 &&
            candidate_pa == target_pa) {
            return candidate;
        }
    }

    return NULL;
}

static uint8_t *find_candidate_for_bit(int bit,
                                       uint8_t *ref_addr,
                                       uint64_t ref_pa,
                                       uint8_t *buddy_pool,
                                       size_t buddy_bytes,
                                       const char **source) {
    uint64_t target_pa = ref_pa ^ (1ULL << bit);
    int page_bits = page_offset_bits();

    *source = "none";

    if (bit < page_bits) {
        uintptr_t ref_va = (uintptr_t)ref_addr;
        uint8_t *candidate = (uint8_t *)(ref_va ^ (1ULL << bit));
        uint64_t candidate_pa;

        if (address_in_array2(candidate) &&
            virt_to_phys(candidate, &candidate_pa) == 0 &&
            candidate_pa == target_pa) {
            *source = "same_page_offset";
            return candidate;
        }
        return NULL;
    }

    {
        uint8_t *candidate = find_addr_with_pa(array2, sizeof(array2),
                                               target_pa);
        if (candidate != NULL) {
            *source = "array2_page";
            return candidate;
        }
    }

    if (buddy_pool != NULL) {
        uint8_t *candidate = find_addr_with_pa(buddy_pool, buddy_bytes,
                                               target_pa);
        if (candidate != NULL) {
            *source = "buddy_pool";
            return candidate;
        }
    }

    return NULL;
}

static void warm_buddy_pool(uint8_t *buddy_pool, size_t buddy_bytes) {
    if (buddy_pool == NULL) {
        return;
    }

    for (size_t offset = 0; offset < buddy_bytes; offset += os_page_size) {
        mLoad(buddy_pool + offset);
    }
}

static void print_pa_bit_result(int bit,
                                uint8_t *candidate,
                                uint64_t candidate_pa,
                                uint64_t ref_pa,
                                uint64_t avg_ns,
                                int prefetched,
                                const char *verdict,
                                const char *source) {
    if (candidate != NULL) {
        printf("%d\t0x%016" PRIx64 "\t0x%016" PRIx64 "\t0x%016" PRIx64 "\t%" PRIu64 "\t%s\t%s\t%s\n",
               bit,
               (uint64_t)(uintptr_t)candidate,
               candidate_pa,
               candidate_pa ^ ref_pa,
               avg_ns,
               prefetched ? "yes" : "no",
               verdict,
               source);
    } else {
        printf("%d\tunknown\tunknown\tunknown\t0\tno\t%s\t%s\n",
               bit, verdict, source);
    }
    fflush(stdout);
}

static int __attribute__((unused)) run_buddy_scan(void) {
    uint8_t *ref_addr = array2 + line_offset(REF_TRIGGER_LINE);
    uint64_t ref_pa = 0;
    size_t buddy_bytes = (size_t)BUDDY_PAGES * os_page_size;
    size_t chunk_pages = BUDDY_CHUNK_PAGES > 0 ? BUDDY_CHUNK_PAGES : BUDDY_PAGES;
    size_t chunk_bytes = chunk_pages * os_page_size;
    uint64_t no_trigger_avg;
    uint64_t ref_avg;
    int done[MAX_PA_BIT + 1];

    if (open_pagemap() != 0 ||
        virt_to_phys(ref_addr, &ref_pa) != 0) {
        printf("# arm64 A55 store-stride PA-bit buddy scan\n");
        printf("# ref_trigger_pa=unknown; run as root or with CAP_SYS_ADMIN to read /proc/self/pagemap PFNs\n");
        printf("pa_bit\tcandidate_va\tcandidate_pa\tpa_xor_ref\tavg_ns\tprefetched\tverdict\tsource\n");
        for (int bit = 0; bit <= MAX_PA_BIT; bit++) {
            printf("%d\tunknown\tunknown\tunknown\t0\tno\tinsufficient_case\tno_pagemap\n",
                   bit);
        }
        return 0;
    }

    no_trigger_avg = run_trigger_addr_case(ref_addr, REF_TRIGGER_LINE, 0);
    ref_avg = run_trigger_addr_case(ref_addr, REF_TRIGGER_LINE, 1);
    memset(done, 0, sizeof(done));

    printf("# arm64 A55 store-stride PA-bit buddy scan\n");
    printf("# train stores: array2 + {0,5,10,15,20} * LINE_SIZE when STRIDE_LINES=5\n");
    printf("# probe: array2 + %d * LINE_SIZE\n", probe_line());
    printf("# STRIDE_LINES=%d TRAIN_STORES=%d REPEAT=%d ROUNDS=%d threshold_ns=%d CPU_ID=%d\n",
           STRIDE_LINES, TRAIN_STORES, REPEAT, ROUNDS, HIT_THRESHOLD_NS,
           CPU_ID);
    printf("# ref_trigger_line=%d ref_trigger_va=%p ref_trigger_pa=0x%016" PRIx64 "\n",
           REF_TRIGGER_LINE, ref_addr, ref_pa);
    printf("# buddy_pages=%d buddy_bytes=%" PRIu64 " chunk_pages=%" PRIu64 " chunk_bytes=%" PRIu64 "\n",
           BUDDY_PAGES, (uint64_t)buddy_bytes,
           (uint64_t)chunk_pages, (uint64_t)chunk_bytes);
    printf("# no_trigger_avg_ns=%" PRIu64 " ref_trigger_avg_ns=%" PRIu64 "\n",
           no_trigger_avg, ref_avg);
    printf("# verdict is based on exact candidate_pa == ref_pa ^ (1 << pa_bit)\n");
    printf("pa_bit\tcandidate_va\tcandidate_pa\tpa_xor_ref\tavg_ns\tprefetched\tverdict\tsource\n");
    fflush(stdout);

    for (int bit = 0; bit <= MAX_PA_BIT; bit++) {
        const char *source = "none";
        uint8_t *candidate;
        uint64_t candidate_pa = 0;
        uint64_t avg_ns = 0;
        int prefetched = 0;
        const char *verdict = "insufficient_case";

        if (bit >= 64) {
            print_pa_bit_result(bit, NULL, 0, ref_pa, 0, 0, verdict, source);
            done[bit] = 1;
            continue;
        }

        candidate = find_candidate_for_bit(bit, ref_addr, ref_pa,
                                           NULL, 0, &source);
        if (candidate != NULL &&
            virt_to_phys(candidate, &candidate_pa) == 0 &&
            candidate_pa == (ref_pa ^ (1ULL << bit))) {
            avg_ns = run_trigger_addr_case(candidate, -1, 1);
            prefetched = avg_ns <= HIT_THRESHOLD_NS;
            verdict = prefetched ? "not_participating" : "participating";

            print_pa_bit_result(bit, candidate, candidate_pa, ref_pa, avg_ns,
                                prefetched, verdict, source);
            done[bit] = 1;
        }
    }

    for (size_t scanned = 0; scanned < (size_t)BUDDY_PAGES;) {
        size_t pages_this_chunk = chunk_pages;
        uint8_t *buddy_pool;
        int missing = 0;

        for (int bit = 0; bit <= MAX_PA_BIT; bit++) {
            if (!done[bit]) {
                missing++;
            }
        }
        if (missing == 0 || chunk_pages == 0) {
            break;
        }

        if (pages_this_chunk > (size_t)BUDDY_PAGES - scanned) {
            pages_this_chunk = (size_t)BUDDY_PAGES - scanned;
        }
        chunk_bytes = pages_this_chunk * os_page_size;

        buddy_pool = mmap(NULL, chunk_bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (buddy_pool == MAP_FAILED) {
            fprintf(stderr,
                    "mmap buddy_pool chunk scanned_pages=%" PRIu64 " pages=%" PRIu64 " bytes=%" PRIu64 " failed: %s\n",
                    (uint64_t)scanned, (uint64_t)pages_this_chunk,
                    (uint64_t)chunk_bytes, strerror(errno));
            break;
        }
        warm_buddy_pool(buddy_pool, chunk_bytes);

        for (int bit = 0; bit <= MAX_PA_BIT; bit++) {
            const char *source = "none";
            uint8_t *candidate;
            uint64_t candidate_pa = 0;
            uint64_t avg_ns;
            int prefetched;
            const char *verdict;

            if (done[bit]) {
                continue;
            }

            candidate = find_candidate_for_bit(bit, ref_addr, ref_pa,
                                               buddy_pool, chunk_bytes,
                                               &source);
            if (candidate == NULL ||
                virt_to_phys(candidate, &candidate_pa) != 0 ||
                candidate_pa != (ref_pa ^ (1ULL << bit))) {
                continue;
            }

            avg_ns = run_trigger_addr_case(candidate, -1, 1);
            prefetched = avg_ns <= HIT_THRESHOLD_NS;
            verdict = prefetched ? "not_participating" : "participating";
            print_pa_bit_result(bit, candidate, candidate_pa, ref_pa, avg_ns,
                                prefetched, verdict, source);
            done[bit] = 1;
        }

        munmap(buddy_pool, chunk_bytes);
        scanned += pages_this_chunk;
    }

    for (int bit = 0; bit <= MAX_PA_BIT; bit++) {
        if (!done[bit]) {
            print_pa_bit_result(bit, NULL, 0, ref_pa, 0, 0,
                                "insufficient_case", "not_found");
        }
    }
    return 0;
}

static uint64_t low_pfn_mask(int bits) {
    if (bits <= 0) {
        return 0;
    }
    if (bits >= 64) {
        return UINT64_MAX;
    }
    return (1ULL << bits) - 1ULL;
}

static uint8_t *find_alias_page(uint8_t *base,
                                size_t bytes,
                                uint64_t ref_page_pa,
                                int low_pfn_bits) {
    uint64_t mask = low_pfn_mask(low_pfn_bits);
    uint64_t ref_pfn_low = (ref_page_pa >> page_offset_bits()) & mask;

    for (size_t offset = 0; offset + os_page_size <= bytes;
         offset += os_page_size) {
        uint8_t *page = base + offset;
        uint64_t page_pa;
        uint64_t page_pfn_low;

        if (virt_to_phys(page, &page_pa) != 0) {
            continue;
        }
        if ((page_pa & ~((uint64_t)os_page_size - 1ULL)) ==
            (ref_page_pa & ~((uint64_t)os_page_size - 1ULL))) {
            continue;
        }

        page_pfn_low = (page_pa >> page_offset_bits()) & mask;
        if (page_pfn_low == ref_pfn_low) {
            return page;
        }
    }

    return NULL;
}

static void print_alias_result(int m,
                               uint8_t *candidate_page,
                               uint64_t candidate_page_pa,
                               uint64_t ref_page_pa,
                               uint64_t avg_ns,
                               int prefetched,
                               const char *verdict,
                               const char *source) {
    if (candidate_page != NULL) {
        printf("%d\t0x%016" PRIx64 "\t0x%016" PRIx64 "\t0x%016" PRIx64 "\t%" PRIu64 "\t%s\t%s\t%s\n",
               m,
               (uint64_t)(uintptr_t)candidate_page,
               candidate_page_pa,
               candidate_page_pa ^ ref_page_pa,
               avg_ns,
               prefetched ? "yes" : "no",
               verdict,
               source);
    } else {
        printf("%d\tunknown\tunknown\tunknown\t0\tno\t%s\t%s\n",
               m, verdict, source);
    }
    fflush(stdout);
}

static int __attribute__((unused)) run_alias_scan(void) {
    uint8_t *ref_page = array2;
    uint8_t *ref_trigger = array2 + line_offset(REF_TRIGGER_LINE);
    size_t trigger_page_offset = line_offset(REF_TRIGGER_LINE) % os_page_size;
    uint64_t ref_page_pa = 0;
    uint64_t no_trigger_avg;
    uint64_t ref_avg;
    size_t buddy_bytes = (size_t)BUDDY_PAGES * os_page_size;
    size_t chunk_pages = BUDDY_CHUNK_PAGES > 0 ? BUDDY_CHUNK_PAGES : BUDDY_PAGES;
    size_t chunk_bytes = chunk_pages * os_page_size;
    int done[ALIAS_MAX_M + 1];

    if (open_pagemap() != 0 ||
        virt_to_phys(ref_page, &ref_page_pa) != 0) {
        printf("# arm64 A55 store-stride low-PA alias scan\n");
        printf("# ref_page_pa=unknown; run as root or with CAP_SYS_ADMIN to read /proc/self/pagemap PFNs\n");
        printf("m_bits\tcandidate_page_va\tcandidate_page_pa\tpage_pa_xor_ref\tavg_ns\tprefetched\tverdict\tsource\n");
        for (int m = ALIAS_MIN_M; m <= ALIAS_MAX_M; m++) {
            print_alias_result(m, NULL, 0, 0, 0, 0,
                               "insufficient_case", "no_pagemap");
        }
        return 0;
    }

    memset(done, 0, sizeof(done));
    no_trigger_avg = run_trigger_addr_case(ref_trigger, REF_TRIGGER_LINE, 0);
    ref_avg = run_trigger_addr_case(ref_trigger, REF_TRIGGER_LINE, 1);

    printf("# arm64 A55 store-stride low-PA alias scan\n");
    printf("# train page: array2 page, train stores {0,5,10,15,20} lines when STRIDE_LINES=5\n");
    printf("# trigger candidate: candidate_page + ref_trigger_page_offset, probe remains array2 + %d * LINE_SIZE\n",
           probe_line());
    printf("# STRIDE_LINES=%d TRAIN_STORES=%d REPEAT=%d ROUNDS=%d threshold_ns=%d CPU_ID=%d\n",
           STRIDE_LINES, TRAIN_STORES, REPEAT, ROUNDS, HIT_THRESHOLD_NS,
           CPU_ID);
    printf("# ref_page_va=%p ref_page_pa=0x%016" PRIx64 " ref_trigger_offset=%" PRIu64 "\n",
           ref_page, ref_page_pa, (uint64_t)trigger_page_offset);
    printf("# alias condition for M: low M PFN bits equal, i.e. PA[%d..%d] match\n",
           page_offset_bits(), page_offset_bits() + ALIAS_MAX_M - 1);
    printf("# buddy_pages=%d buddy_bytes=%" PRIu64 " chunk_pages=%" PRIu64 " chunk_bytes=%" PRIu64 "\n",
           BUDDY_PAGES, (uint64_t)buddy_bytes,
           (uint64_t)chunk_pages, (uint64_t)chunk_bytes);
    printf("# no_trigger_avg_ns=%" PRIu64 " ref_trigger_avg_ns=%" PRIu64 "\n",
           no_trigger_avg, ref_avg);
    printf("m_bits\tcandidate_page_va\tcandidate_page_pa\tpage_pa_xor_ref\tavg_ns\tprefetched\tverdict\tsource\n");
    fflush(stdout);

    for (int m = ALIAS_MIN_M; m <= ALIAS_MAX_M; m++) {
        uint8_t *candidate_page = find_alias_page(array2, sizeof(array2),
                                                  ref_page_pa, m);
        uint64_t candidate_page_pa;

        if (candidate_page == NULL ||
            virt_to_phys(candidate_page, &candidate_page_pa) != 0) {
            continue;
        }

        {
            uint8_t *trigger_addr = candidate_page + trigger_page_offset;
            uint64_t avg_ns = run_trigger_addr_case(trigger_addr, -1, 1);
            int prefetched = avg_ns <= HIT_THRESHOLD_NS;
            print_alias_result(m, candidate_page, candidate_page_pa,
                               ref_page_pa, avg_ns, prefetched,
                               prefetched ? "alias_triggered" : "no_alias",
                               "array2_page");
        }
        done[m] = 1;
    }

    for (size_t scanned = 0; scanned < (size_t)BUDDY_PAGES;) {
        size_t pages_this_chunk = chunk_pages;
        uint8_t *buddy_pool;
        int missing = 0;

        for (int m = ALIAS_MIN_M; m <= ALIAS_MAX_M; m++) {
            if (!done[m]) {
                missing++;
            }
        }
        if (missing == 0 || chunk_pages == 0) {
            break;
        }

        if (pages_this_chunk > (size_t)BUDDY_PAGES - scanned) {
            pages_this_chunk = (size_t)BUDDY_PAGES - scanned;
        }
        chunk_bytes = pages_this_chunk * os_page_size;

        buddy_pool = mmap(NULL, chunk_bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (buddy_pool == MAP_FAILED) {
            fprintf(stderr,
                    "mmap alias buddy chunk scanned_pages=%" PRIu64 " pages=%" PRIu64 " bytes=%" PRIu64 " failed: %s\n",
                    (uint64_t)scanned, (uint64_t)pages_this_chunk,
                    (uint64_t)chunk_bytes, strerror(errno));
            break;
        }
        warm_buddy_pool(buddy_pool, chunk_bytes);

        for (int m = ALIAS_MIN_M; m <= ALIAS_MAX_M; m++) {
            uint8_t *candidate_page;
            uint64_t candidate_page_pa;
            uint8_t *trigger_addr;
            uint64_t avg_ns;
            int prefetched;

            if (done[m]) {
                continue;
            }

            candidate_page = find_alias_page(buddy_pool, chunk_bytes,
                                             ref_page_pa, m);
            if (candidate_page == NULL ||
                virt_to_phys(candidate_page, &candidate_page_pa) != 0) {
                continue;
            }

            trigger_addr = candidate_page + trigger_page_offset;
            avg_ns = run_trigger_addr_case(trigger_addr, -1, 1);
            prefetched = avg_ns <= HIT_THRESHOLD_NS;
            print_alias_result(m, candidate_page, candidate_page_pa,
                               ref_page_pa, avg_ns, prefetched,
                               prefetched ? "alias_triggered" : "no_alias",
                               "buddy_pool");
            done[m] = 1;
        }

        munmap(buddy_pool, chunk_bytes);
        scanned += pages_this_chunk;
    }

    for (int m = ALIAS_MIN_M; m <= ALIAS_MAX_M; m++) {
        if (!done[m]) {
            print_alias_result(m, NULL, 0, ref_page_pa, 0, 0,
                               "insufficient_case", "not_found");
        }
    }

    return 0;
}

int main(void) {
    uint64_t ref_pa = 0;
    int ref_pa_known = 0;
    uint64_t no_trigger_avg;
    uint64_t ref_avg;
    int case_count;
    struct case_result *results;
    long detected_page_size;

    if (STRIDE_LINES <= 0 || TRAIN_STORES <= 0 || REPEAT <= 0 || ROUNDS <= 0) {
        fprintf(stderr, "STRIDE_LINES, TRAIN_STORES, REPEAT, and ROUNDS must be positive\n");
        return 1;
    }
    if (probe_line() >= ITEMS) {
        fprintf(stderr, "probe line exceeds array2 size\n");
        return 1;
    }
    if (TRIGGER_MIN_LINE < 0 ||
        TRIGGER_MAX_LINE < TRIGGER_MIN_LINE ||
        TRIGGER_MAX_LINE >= ITEMS ||
        REF_TRIGGER_LINE < TRIGGER_MIN_LINE ||
        REF_TRIGGER_LINE > TRIGGER_MAX_LINE) {
        fprintf(stderr, "invalid trigger line range or reference trigger line\n");
        return 1;
    }

    detected_page_size = sysconf(_SC_PAGESIZE);
    if (detected_page_size <= 0) {
        die("sysconf(_SC_PAGESIZE)");
    }
    os_page_size = (size_t)detected_page_size;

    set_cpu_if_requested();

    memset(array2, -1, sizeof(array2));
    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        die("mmap dummy_buffer");
    }

    warm_lines();
    for (int line = 0; line < ITEMS; line++) {
        flush_line_once(line);
    }
    mfence();

#if BUDDY_SCAN
    return run_buddy_scan();
#endif

#if ALIAS_SCAN
    return run_alias_scan();
#endif

    if (open_pagemap() == 0) {
        ref_pa_known = virt_to_phys(array2 + line_offset(REF_TRIGGER_LINE),
                                    &ref_pa) == 0;
    }

    no_trigger_avg = run_trigger_case(REF_TRIGGER_LINE, 0);
    ref_avg = run_trigger_case(REF_TRIGGER_LINE, 1);

    case_count = TRIGGER_MAX_LINE - TRIGGER_MIN_LINE + 1;
    results = calloc((size_t)case_count, sizeof(*results));
    if (!results) {
        die("calloc results");
    }

    print_header(no_trigger_avg, ref_avg, ref_pa_known, ref_pa);

    for (int line = TRIGGER_MIN_LINE; line <= TRIGGER_MAX_LINE; line++) {
        int index = line - TRIGGER_MIN_LINE;
        struct case_result *result = &results[index];
        uint64_t pa = 0;

        result->trigger_line = line;
        result->trigger_va = (uint64_t)(uintptr_t)(array2 + line_offset(line));
        result->avg_ns = run_trigger_case(line, 1);
        result->direct_probe_hit = (line == probe_line());
        result->prefetched = !result->direct_probe_hit &&
                             result->avg_ns <= HIT_THRESHOLD_NS;

        if (ref_pa_known &&
            virt_to_phys(array2 + line_offset(line), &pa) == 0) {
            result->pa_known = 1;
            result->trigger_pa = pa;
            result->pa_xor_ref = pa ^ ref_pa;
        }

        print_case_result(result);
    }

    print_bit_summary(results, case_count);

    if (pagemap_fd >= 0) {
        close(pagemap_fd);
    }
    free(results);
    return 0;
}
