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

#define PMU_WINDOW_NAME "test2-training-lines-341-361"
#include "../pmu.h"

#define Items 10240

#ifndef STRIDE_BYTES
#define STRIDE_BYTES 64
#endif

#ifndef TRAIN_STEP
#define TRAIN_STEP 10
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef OTHER_PAGES_N
#define OTHER_PAGES_N 0
#endif

#ifndef CANDIDATE_PAGES_PER_OTHER
#define CANDIDATE_PAGES_PER_OTHER 64
#endif

#ifndef NO_TRIGGER
#define NO_TRIGGER 0
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



static void print_test_header(int stride, int train_step, int active_pages,
                              uint64_t rounds) {
    printf("# %s store-stride other-page-count prefetch latency map\n",
#ifdef __x86_64__
           "x86_64"
#elif defined(__aarch64__)
           "arm64"
#else
           "unknown"
#endif
    );
    printf("# access mode: store (%s), same noinline PC for victim train and trigger\n",
#ifdef __x86_64__
           "movb store"
#else
           "strb"
#endif
    );
    printf("# stride_bytes=%d train_step=%d other_pages=%d rounds=%llu probe_positions=%d\n",
           stride, train_step, active_pages, (unsigned long long)rounds,
           PROBE_POSITIONS);
    printf("# timer: %s %s\n", TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME);
    printf("# position\toffset_bytes\tavg_%s\tprobes\n", TIMESTAMP_UNIT_NAME);
}

static void flush_victim_lines(void) {
    for (uint64_t offset = 0; offset < Items * LINE_SIZE; offset += LINE_SIZE) {
        flush(&array2[offset]);
    }
}

static void flush_other_pages(int active_pages) {
#if OTHER_PAGES_N > 0
    for (int page = 0; page < active_pages; page++) {
        uint8_t *base = other_pages[page];
        for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
            flush(base + offset);
        }
    }
#else
    (void)active_pages;
#endif
}

static void train_other_pages_range(int stride, int train_step,
                                    int first_page, int end_page) {
#if OTHER_PAGES_N > 0
    for (int page = first_page; page < end_page; page++) {
        uint8_t *base = other_pages[page];
        // for (int step = 0; step < train_step - 1; step++) {
        for (int step = 0; step < 1; step++) {
            mStore_inline(base + 3 * LINE_SIZE +
                          ((uint64_t)step * (uint64_t)stride));
            nops();
        }
    }
#else
    (void)stride;
    (void)train_step;
    (void)first_page;
    (void)end_page;
#endif
}

// void dummyAccesses(void){
//     for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
//         asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
//      }
// }


int main(void) {
    register uint64_t time1, time2;
    volatile uint8_t *probe_addr;

    memset(array2, -1, Items * LINE_SIZE * sizeof(uint8_t));

#if OTHER_PAGES_N > 0
    long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size != PAGE_SIZE) {
        fprintf(stderr, "system page size %ld does not match PAGE_SIZE %d\n",
                system_page_size, PAGE_SIZE);
        return 1;
    }
    if (allocate_unique_other_pages() != 0) {
        return 1;
    }
#endif

    uint64_t rounds = ROUNDS;
    int stride = STRIDE_BYTES;
    int train_step = TRAIN_STEP;

    if ((uint64_t)(train_step - 1) * (uint64_t)stride >= Items * LINE_SIZE) {
        fprintf(stderr, "training range exceeds array2 size\n");
        return 1;
    }
#if OTHER_PAGES_N > 0
    if (train_step > 1 &&
        3 * LINE_SIZE +
                (uint64_t)(train_step - 2) * (uint64_t)stride >=
            PAGE_SIZE) {
        fprintf(stderr, "other-page training range exceeds page size\n");
        return 1;
    }
#endif
    if ((uint64_t)train_step * (uint64_t)stride >= Items * LINE_SIZE) {
        fprintf(stderr, "predicted line exceeds array2 size\n");
        return 1;
    }

    int pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        printf("# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
    }

    for (int active_pages = 0; active_pages <= OTHER_PAGES_N;
         active_pages++) {
        memset(latency_sum, 0, sizeof(latency_sum));
        memset(probe_count, 0, sizeof(probe_count));
        uint64_t latency2_sum = 0;

        printf("# begin_other_pages=%d\n", active_pages);
        print_test_header(stride, train_step, active_pages, rounds);
        if (pmu_ready) {
            pmu_reset_accumulated();
        }

        for (uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
           
            // mfence();
            // dummyAccesses();
            flush_victim_lines();
            flush_other_pages(active_pages);
            // mfence();
            
            // mfence();

            // for (int step = 0; step < train_step - 1; step++) {
            //     stride_access(array2 + ((uint64_t)step * (uint64_t)stride));
            // }


            int pmu_running = 0;
            if (pmu_ready) {
                pmu_running = (pmu_start() == 0);
                if (!pmu_running) {
                    printf("# PMU unavailable: counter group could not be started\n");
                    pmu_ready = 0;
                }
            }
            cpp_rctx();

            
// #if OTHER_PAGES_N > 11
//             mLoad_inline(other_pages[0] + 7 * LINE_SIZE);
//             mLoad_inline(other_pages[1] + 3 * LINE_SIZE);
// #endif

            mStore_inline(array2 + (0 * (uint64_t)stride));
            nops();
            mStore_inline(array2 + (1 * (uint64_t)stride));
            nops(); 

            // int competitor_split = active_pages / 2;
            train_other_pages_range(stride, train_step,
                                    0, active_pages);
            // train_other_pages_range(stride, train_step,
            //                         0, 3);
            // nops();    
                     
            // train_other_pages_range(stride, train_step,
            //                     3, active_pages);
        //     // mfence();
        //     // uint8_t *base = other_pages;
        //     // stride_access(base + 3 * LINE_SIZE);
        //     // context_switch_before_trigger();

#if !NO_TRIGGER  
            mStore_inline(array2 + (2 * (uint64_t)stride));
            nops();
#endif



            if (pmu_running) {
                pmu_stop_and_accumulate();
            }

            int probe_pos = (int)(atkRound % PROBE_POSITIONS);
            probe_addr = array2 + ((uint64_t)probe_pos * LINE_SIZE);

            // probe_addr = array2 + ((uint64_t)(train_step) *
            //                       (uint64_t)stride);

            time1 = timestamp();
            mStore_inline((void *)probe_addr);
            time2 = timestamp() - time1;

            latency2_sum += time2;
            latency_sum[probe_pos] += time2;
            probe_count[probe_pos]++;
        }
        if (pmu_ready) {
            pmu_print_accumulated(rounds);
        }
        // printf("avg_latency=%llu\n",
        //        (unsigned long long)(latency2_sum / rounds));
        (void)latency2_sum;
        for (int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
            long long int avg = 0;
            if (probe_count[probe_pos] > 0) {
                avg = latency_sum[probe_pos] / probe_count[probe_pos];
            }
            printf("%3d\t%12d\t%10lld\t%5d\n",
                   probe_pos,
                   probe_pos * LINE_SIZE,
                   avg,
                   probe_count[probe_pos]);
        }
        printf("# end_other_pages=%d\n\n", active_pages);
    }

    pmu_cleanup();

#if OTHER_PAGES_N > 0
    free(other_pages);
    munmap(other_pages_mapping, other_pages_mapping_size);
#endif

    return 0;
}
