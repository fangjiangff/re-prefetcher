#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../until.h"

#define PMU_WINDOW_NAME "test2-entry-replacement"
#include "../pmu.h"

#define Items 10240
#define OTHER_PAGES_N 20
#define INITIALIZED_ENTRIES 16
#define INITIAL_ACCESSES 2
#define TRIGGER_STEP 2
#define PREDICTED_STEP 3

#ifndef STRIDE_BYTES
#define STRIDE_BYTES 320
#endif

#ifndef ROUNDS
#define ROUNDS 40000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef CANDIDATE_PAGES_PER_OTHER
#define CANDIDATE_PAGES_PER_OTHER 64
#endif

#ifndef NO_TRIGGER
#define NO_TRIGGER 0
#endif

#ifndef TEST_PAGE
#define TEST_PAGE 0
#endif


uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));
long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

#if OTHER_PAGES_N > 0
static uint8_t *other_pages_mapping;
static size_t other_pages_mapping_size;
static uint8_t **other_pages;

#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define PREFETCH_PA_FIELD_MASK ((1ULL << 18) - 1)
#define PREFETCH_HASH_MASK ((1U << 6) - 1)


static int physical_page_info(int pagemap_fd, const void *addr,
                              uint64_t *physical_addr, uint8_t *hash) {
    uintptr_t va = (uintptr_t)addr;
    uint64_t entry;
    off_t offset = (off_t)((va / PAGE_SIZE) * sizeof(entry));
    ssize_t bytes = pread(pagemap_fd, &entry, sizeof(entry), offset);

    if (bytes != (ssize_t)sizeof(entry)) {
        fprintf(stderr, "failed to read /proc/self/pagemap: %s\n",
                bytes < 0 ? strerror(errno) : "short read");
        return -1;
    }
    if (!(entry & PAGEMAP_PRESENT)) {
        fprintf(stderr, "candidate page is not resident\n");
        return -1;
    }

    uint64_t pfn = entry & PAGEMAP_PFN_MASK;
    if (pfn == 0) {
        fprintf(stderr,
                "pagemap did not expose a PFN; run with CAP_SYS_ADMIN/root\n");
        return -1;
    }

    uint64_t pa = pfn * (uint64_t)PAGE_SIZE + (va & (PAGE_SIZE - 1));
    *physical_addr = pa;
    *hash = (uint8_t)(((pa >> 16) ^ (pa >> 22) ^ (pa >> 28)) &
                      PREFETCH_HASH_MASK);
    return 0;
}

static int allocate_unique_other_pages(void) {
    if (OTHER_PAGES_N > PREFETCH_HASH_MASK) {
        fprintf(stderr,
                "OTHER_PAGES_N=%d exceeds the 63 hashes available after "
                "excluding the victim hash\n",
                OTHER_PAGES_N);
        return -1;
    }
    if (CANDIDATE_PAGES_PER_OTHER <= 0 ||
        (size_t)CANDIDATE_PAGES_PER_OTHER > SIZE_MAX / PAGE_SIZE ||
        (size_t)OTHER_PAGES_N >
            SIZE_MAX / ((size_t)CANDIDATE_PAGES_PER_OTHER * PAGE_SIZE)) {
        fprintf(stderr, "invalid candidate-pool size\n");
        return -1;
    }

    size_t candidate_count =
        (size_t)OTHER_PAGES_N * CANDIDATE_PAGES_PER_OTHER;
    other_pages_mapping_size = candidate_count * PAGE_SIZE;
    other_pages_mapping = mmap(NULL, other_pages_mapping_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                               -1, 0);
    if (other_pages_mapping == MAP_FAILED) {
        fprintf(stderr, "failed to map candidate pool: %s\n", strerror(errno));
        other_pages_mapping = NULL;
        return -1;
    }

    /* Fault in every candidate before reading its pagemap entry. */
    memset(other_pages_mapping, -1, other_pages_mapping_size);
    other_pages = calloc(OTHER_PAGES_N, sizeof(*other_pages));
    uint8_t *hashes = calloc(OTHER_PAGES_N, sizeof(*hashes));
    uint64_t *physical_addrs =
        calloc(OTHER_PAGES_N, sizeof(*physical_addrs));
    if (!other_pages || !hashes || !physical_addrs) {
        fprintf(stderr, "failed to allocate other-page metadata\n");
        free(other_pages);
        free(hashes);
        free(physical_addrs);
        return -1;
    }

    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) {
        fprintf(stderr, "failed to open /proc/self/pagemap: %s\n",
                strerror(errno));
        free(hashes);
        free(physical_addrs);
        return -1;
    }

    uint64_t victim_pa;
    uint8_t victim_hash;
    if (physical_page_info(pagemap_fd, array2, &victim_pa,
                           &victim_hash) != 0) {
        close(pagemap_fd);
        free(hashes);
        free(physical_addrs);
        return -1;
    }

    size_t selected = 0;
    for (size_t candidate = 0;
         candidate < candidate_count && selected < OTHER_PAGES_N;
        candidate++) {
        uint8_t *page = other_pages_mapping + candidate * PAGE_SIZE;
        uint64_t physical_addr;
        uint8_t hash;
        if (physical_page_info(pagemap_fd, page, &physical_addr, &hash) != 0) {
            close(pagemap_fd);
            free(hashes);
            free(physical_addrs);
            return -1;
        }

        int duplicate = hash == victim_hash;
        for (size_t i = 0; i < selected; i++) {
            if (hashes[i] == hash) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            other_pages[selected] = page;
            hashes[selected] = hash;
            physical_addrs[selected] = physical_addr;
            selected++;
        }
    }
    close(pagemap_fd);

    if (selected != OTHER_PAGES_N) {
        fprintf(stderr,
                "candidate pool supplied only %zu unique non-victim "
                "physical-address hashes for %d pages; increase "
                "CANDIDATE_PAGES_PER_OTHER\n",
                selected, OTHER_PAGES_N);
        free(hashes);
        free(physical_addrs);
        return -1;
    }

    printf("# victim_page va=%p pa=0x%llx pa16_33=0x%05llx "
           "hash=0x%02x\n",
           (void *)array2, (unsigned long long)victim_pa,
           (unsigned long long)((victim_pa >> 16) & PREFETCH_PA_FIELD_MASK),
           victim_hash);
    for (size_t i = 0; i < selected; i++) {
        printf("# other_page[%zu] va=%p pa=0x%llx pa16_33=0x%05llx "
               "hash=0x%02x\n",
               i, (void *)other_pages[i],
               (unsigned long long)physical_addrs[i],
               (unsigned long long)((physical_addrs[i] >> 16) &
                                    PREFETCH_PA_FIELD_MASK),
               hashes[i]);
    }

    free(hashes);
    free(physical_addrs);
    return 0;
}
#endif



static void print_test_header(int stride, int test_page, uint64_t rounds) {
    printf("# %s store-stride entry-replacement probe\n",
#ifdef __x86_64__
           "x86_64"
#elif defined(__aarch64__)
           "arm64"
#else
           "unknown"
#endif
    );
    printf("# access mode: store (%s), per-entry access sites\n",
#ifdef __x86_64__
           "movb store"
#else
           "strb"
#endif
    );
    printf("# stride_bytes=%d fixed_pages=%d initialized_entries=%d "
           "initial_accesses=%d test_page=%d trigger=%d rounds=%llu "
           "probe_positions=%d\n",
           stride, OTHER_PAGES_N, INITIALIZED_ENTRIES, INITIAL_ACCESSES,
           test_page, !NO_TRIGGER, (unsigned long long)rounds, PROBE_POSITIONS);
    printf("# timer: %s %s\n", TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME);
    printf("# position\toffset_bytes\tavg_%s\tprobes\n", TIMESTAMP_UNIT_NAME);
}

static void flush_other_pages(void) {
    for (int page = 0; page < OTHER_PAGES_N; page++) {
        uint8_t *base = other_pages[page];
        for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
            flush(base + offset);
        }
    }
}

static void initialize_entries_1(int stride) {
    /* Entry 0. */
    mStore_inline(other_pages[0] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[0] + 1 * (uint64_t)stride);
    nops();

    /* Entry 1. */
    mStore_inline(other_pages[1] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[1] + 1 * (uint64_t)stride);
    nops();

    /* Entry 2. */
    mStore_inline(other_pages[2] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[2] + 1 * (uint64_t)stride);
    nops();

    /* Entry 3. */
    mStore_inline(other_pages[3] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[3] + 1 * (uint64_t)stride);
    nops();

    /* Entry 4. */
    mStore_inline(other_pages[4] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[4] + 1 * (uint64_t)stride);
    nops();

    /* Entry 5. */
    mStore_inline(other_pages[5] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[5] + 1 * (uint64_t)stride);
    nops();

    /* Entry 6. */
    mStore_inline(other_pages[6] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[6] + 1 * (uint64_t)stride);
    nops();



    /* Entry 7. */
    mStore_inline(other_pages[7] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[7] + 1 * (uint64_t)stride);
    nops();

    /* Entry 8. */
    mStore_inline(other_pages[8] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[8] + 1 * (uint64_t)stride);
    nops();

    /* Entry 9. */
    mStore_inline(other_pages[9] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[9] + 1 * (uint64_t)stride);
    nops();

    /* Entry 10. */
    mStore_inline(other_pages[10] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[10] + 1 * (uint64_t)stride);
    nops();

    /* Entry 11. */
    mStore_inline(other_pages[11] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[11] + 1 * (uint64_t)stride);
    nops();

    // /* Entry 12. */
    // mStore_inline(other_pages[12] + 0 * (uint64_t)stride);
    // nops();
    // mStore_inline(other_pages[12] + 1 * (uint64_t)stride);
    // nops();

    // /* Entry 13. */
    // mStore_inline(other_pages[13] + 0 * (uint64_t)stride);
    // nops();
    // mStore_inline(other_pages[13] + 1 * (uint64_t)stride);
    // nops();

    // /* Entry 14. */
    // mStore_inline(other_pages[14] + 0 * (uint64_t)stride);
    // nops();
    // mStore_inline(other_pages[14] + 1 * (uint64_t)stride);
    // nops();

    // /* Entry 15. */
    // mStore_inline(other_pages[15] + 0 * (uint64_t)stride);
    // nops();
    // mStore_inline(other_pages[15] + 1 * (uint64_t)stride);
    // nops();
}

static void initialize_entries_2(int stride) {
    /* Entry 0. */
    mStore_inline(other_pages[0] + 0 * (uint64_t)stride);
    nops();

  
    /* Entry 1. */
    mStore_inline(other_pages[1] + 0 * (uint64_t)stride);
    nops();


    /* Entry 2. */
    mStore_inline(other_pages[2] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[2] + 1 * (uint64_t)stride);
    nops();

    /* Entry 3. */
    mStore_inline(other_pages[3] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[3] + 1 * (uint64_t)stride);
    nops();

    /* Entry 4. */
    mStore_inline(other_pages[4] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[4] + 1 * (uint64_t)stride);
    nops();



    /* Entry 5. */
    mStore_inline(other_pages[5] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[5] + 1 * (uint64_t)stride);
    nops();

    mStore_inline(other_pages[0] + 1 * (uint64_t)stride);
    nops();

    mStore_inline(other_pages[1] + 1 * (uint64_t)stride);
    nops();

    /* Entry 6. */
    mStore_inline(other_pages[6] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[6] + 1 * (uint64_t)stride);
    nops();


    /* Entry 7. */
    mStore_inline(other_pages[7] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[7] + 1 * (uint64_t)stride);
    nops();

    /* Entry 8. */
    mStore_inline(other_pages[8] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[8] + 1 * (uint64_t)stride);
    nops();

    /* Entry 9. */
    mStore_inline(other_pages[9] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[9] + 1 * (uint64_t)stride);
    nops();


    /* Entry 10. */
    mStore_inline(other_pages[10] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[10] + 1 * (uint64_t)stride);
    nops();

    /* Entry 11. */
    mStore_inline(other_pages[11] + 0 * (uint64_t)stride);
    nops();
    mStore_inline(other_pages[11] + 1 * (uint64_t)stride);
    nops();

    // /* Entry 12. */
    // mStore_inline(other_pages[12] + 0 * (uint64_t)stride);
    // nops();
    // mStore_inline(other_pages[12] + 1 * (uint64_t)stride);
    // nops();

    // /* Entry 13. */
    // mStore_inline(other_pages[13] + 0 * (uint64_t)stride);
    // nops();
    // mStore_inline(other_pages[13] + 1 * (uint64_t)stride);
    // nops();

    // /* Entry 14. */
    // mStore_inline(other_pages[14] + 0 * (uint64_t)stride);
    // nops();
    // mStore_inline(other_pages[14] + 1 * (uint64_t)stride);
    // nops();

    // /* Entry 15. */
    // mStore_inline(other_pages[15] + 0 * (uint64_t)stride);
    // nops();
    // mStore_inline(other_pages[15] + 1 * (uint64_t)stride);
    // nops();
}
// void dummyAccesses(void){
//     for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
//         asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
//      }
// }


int main(void) {
    register uint64_t time1, time2;
    volatile uint8_t *probe_addr;
    const uint64_t rounds = ROUNDS;
    const int stride = STRIDE_BYTES;
    const int test_page = TEST_PAGE;
    const int predicted_pos = (PREDICTED_STEP * stride) / LINE_SIZE;

    memset(array2, -1, sizeof(array2));

    long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size != PAGE_SIZE) {
        fprintf(stderr, "system page size %ld does not match PAGE_SIZE %d\n",
                system_page_size, PAGE_SIZE);
        return 1;
    }
    if (test_page < 0 || test_page >= INITIALIZED_ENTRIES) {
        fprintf(stderr, "TEST_PAGE=%d must be in [0, %d]\n",
                test_page, INITIALIZED_ENTRIES - 1);
        return 1;
    }
    if (stride <= 0 || stride % LINE_SIZE != 0) {
        fprintf(stderr, "STRIDE_BYTES=%d must be a positive multiple of %d\n",
                stride, LINE_SIZE);
        return 1;
    }
    if ((uint64_t)PREDICTED_STEP * (uint64_t)stride >= PAGE_SIZE) {
        fprintf(stderr, "predicted address exceeds the tested page\n");
        return 1;
    }
    if (predicted_pos < 0 || predicted_pos >= PROBE_POSITIONS) {
        fprintf(stderr,
                "predicted position %d exceeds PROBE_POSITIONS=%d\n",
                predicted_pos, PROBE_POSITIONS);
        return 1;
    }
    if (allocate_unique_other_pages() != 0) {
        return 1;
    }

    memset(latency_sum, 0, sizeof(latency_sum));
    memset(probe_count, 0, sizeof(probe_count));
    printf("# begin_test_page=%d\n", test_page);
    print_test_header(stride, test_page, rounds);

    int pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        printf("# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
    } else {
        pmu_reset_accumulated();
    }

    for (uint64_t atk_round = 0; atk_round < rounds; ++atk_round) {
        flush_other_pages();
        mfence();

        int pmu_running = 0;
        if (pmu_ready) {
            pmu_running = (pmu_start() == 0);
            if (!pmu_running) {
                printf("# PMU unavailable: counter group could not be started\n");
                pmu_ready = 0;
            }
        }

        cpp_rctx();
        initialize_entries_1(stride);
#if !NO_TRIGGER
        mStore_inline(other_pages[test_page] +
                      ((uint64_t)TRIGGER_STEP * (uint64_t)stride));
        nops();
#endif

        if (pmu_running) {
            pmu_stop_and_accumulate();
        }

        probe_addr = other_pages[test_page] +
                     ((uint64_t)PREDICTED_STEP * (uint64_t)stride);
        time1 = timestamp();
        mStore_inline((void *)probe_addr);
        time2 = timestamp() - time1;
        latency_sum[predicted_pos] += time2;
        probe_count[predicted_pos]++;
    }

    if (pmu_ready) {
        pmu_print_accumulated(rounds);
    }
    printf("%3d\t%12d\t%10lld\t%5d\n",
           predicted_pos, PREDICTED_STEP * stride,
           latency_sum[predicted_pos] / probe_count[predicted_pos],
           probe_count[predicted_pos]);
    printf("# end_test_page=%d\n", test_page);

    pmu_cleanup();
    free(other_pages);
    munmap(other_pages_mapping, other_pages_mapping_size);
    return 0;
}
