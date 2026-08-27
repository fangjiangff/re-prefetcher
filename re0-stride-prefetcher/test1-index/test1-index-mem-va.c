#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../../until.h"

#ifndef VA1_BASE
#define VA1_BASE 0x600000000ull
#endif

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

#ifndef DUMMY_BUFFER_PAGES
#define DUMMY_BUFFER_PAGES 10
#endif

#ifndef ENABLE_CPP_RCTX
#define ENABLE_CPP_RCTX 0
#endif

#ifndef ENABLE_PMU
#define ENABLE_PMU 0
#endif

#ifndef ENABLE_PROBE
#define ENABLE_PROBE 1
#endif

#ifndef STRIDE_ACCESS_LOAD
#define STRIDE_ACCESS_LOAD 0
#endif

#if STRIDE_ACCESS_LOAD
#define STRIDE_ACCESS_NAME "load"
#else
#define STRIDE_ACCESS_NAME "store"
#endif

#if ENABLE_PMU
#ifndef PMU_WINDOW_NAME
#define PMU_WINDOW_NAME "train-trigger-wait-per-round"
#endif
#ifndef PMU_CORE_X925
#define PMU_CORE_X925 ENABLE_CPP_RCTX
#endif
#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif
#include "../pmu.h"
static int index_mem_va_pmu_ready;
#endif

#define STRIDE_BYTES (STRIDE_LINES * LINE_SIZE)
#define TRAIN_ONLY_ACCESSES (STORE_ACCESSES - 1)
#define TRIGGER_POS (TRAIN_ONLY_ACCESSES * STRIDE_LINES)
#define PREDICTED_POS (STORE_ACCESSES * STRIDE_LINES)
#if ENABLE_PROBE
#define MAX_ALIAS_ACCESS_POS PREDICTED_POS
#else
#define MAX_ALIAS_ACCESS_POS TRIGGER_POS
#endif
#define ALIAS_REQUIRED_BYTES ((MAX_ALIAS_ACCESS_POS + 1) * LINE_SIZE)
#define ALIAS_SIZE \
    (((ALIAS_REQUIRED_BYTES + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE)
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t *dummy_buffer;

static int create_alias_fd(void) {
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "store-stride-va-alias", 0);
#else
    int fd = -1;
    errno = ENOSYS;
#endif
    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }
    if (ftruncate(fd, ALIAS_SIZE) != 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    return fd;
}

static uint8_t *map_alias(uintptr_t va, int fd, const char *name) {
    void *mapping = mmap((void *)va,
                         ALIAS_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_FIXED_NOREPLACE | MAP_SHARED | MAP_POPULATE,
                         fd,
                         0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr,
                "mmap %s at 0x%016lx failed: %s\n",
                name,
                (unsigned long)va,
                strerror(errno));
        return NULL;
    }
    if ((uintptr_t)mapping != va) {
        fprintf(stderr,
                "mmap %s returned wrong address: expected 0x%016lx got %p\n",
                name,
                (unsigned long)va,
                mapping);
        munmap(mapping, ALIAS_SIZE);
        return NULL;
    }
    return (uint8_t *)mapping;
}

// static void dummy_accesses(void) {
//     dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
// }

void dummyAccesses(void){
    // printf("dummySize %d\n", DUMMY_BUFFER_SIZE);
  // dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
    for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
        // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[j]) : "memory", "w0");
    }
}

static void flush_aliases(uint8_t *va1, uint8_t *va2) {
    for (size_t offset = 0; offset < ALIAS_SIZE; offset += LINE_SIZE) {
        flush(va1 + offset);
        flush(va2 + offset);
    }
}

static inline __attribute__((always_inline)) void stride_access(void *addr) {
#if STRIDE_ACCESS_LOAD
    mLoad_inline(addr);
#else
    mStore_inline(addr);
#endif
}

#if ENABLE_PROBE
static uint64_t probe_latency(uint8_t *addr) {
    uint64_t start = timestamp();
    stride_access(addr);
    return timestamp() - start;
}
#endif

__attribute__((noinline)) static void run_trainer(uint8_t *va1) {
    for (int access = 0; access < TRAIN_ONLY_ACCESSES; access++) {
        stride_access(va1 + (uint64_t)access * STRIDE_BYTES);
        nops();
    }
}

__attribute__((noinline)) static void run_trigger(uint8_t *va2) {
    stride_access(va2 + TRIGGER_POS * LINE_SIZE);
    nops();
}

static void wait_after_trigger_access(void) {
    nops();
    struct timespec prefetch_wait = {.tv_sec = 0, .tv_nsec = 100};
    nanosleep(&prefetch_wait, NULL);
}

static void run_case(const char *result_name,
                     const char *detail_name,
                     uint8_t *va1,
                     uint8_t *va2,
                     uint64_t rounds,
                     int enable_trainer,
                     int enable_trigger) {
#if ENABLE_PROBE
    uint64_t latency_sum[PROBE_POSITIONS] = {0};
    int probe_count[PROBE_POSITIONS] = {0};
#else
    (void)detail_name;
#endif

#if ENABLE_PMU
    if (index_mem_va_pmu_ready) {
        pmu_reset_accumulated();
    }
#endif

    for (uint64_t round = 0; round < rounds; round++) {
        dummyAccesses();
        occupy_store_prefetcher_entries(dummy_buffer, DUMMY_BUFFER_PAGES, 6);
        flush_aliases(va1, va2);
        mfence();

#if ENABLE_PMU
        int pmu_running = 0;
        if (index_mem_va_pmu_ready) {
            pmu_running = (pmu_start() == 0);
            if (!pmu_running) {
                printf("# PMU unavailable: counter group could not be started\n");
                index_mem_va_pmu_ready = 0;
            }
        }
#endif

#if ENABLE_CPP_RCTX
        cpp_rctx();
#endif

        if (enable_trainer) {
            run_trainer(va1);
        }
        if (enable_trigger) {
            run_trigger(va2);
        }
        wait_after_trigger_access();

#if ENABLE_PMU
        if (pmu_running) {
            pmu_stop_and_accumulate();
        }
#endif

#if ENABLE_PROBE
        int probe_pos = (int)(round % PROBE_POSITIONS);
        latency_sum[probe_pos] += probe_latency(va2 + probe_pos * LINE_SIZE);
        probe_count[probe_pos]++;
#endif
    }

#if ENABLE_PROBE
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
#else
    printf("%s\t-1\n", result_name);
#endif

#if ENABLE_PMU
    printf("# PMU_CASE %s\n", result_name);
    if (index_mem_va_pmu_ready) {
        pmu_print_accumulated(rounds);
    }
#endif
}

static void print_full_failure(int diff_bit) {
    printf("bit_%d_full\t-1\n", diff_bit);
}

static void run_bit(int diff_bit, uintptr_t va1_base, uint64_t rounds) {
    uintptr_t va2_base = va1_base ^ (1ull << diff_bit);
    char result_name[64];
    char detail_name[64];

    snprintf(result_name, sizeof(result_name), "bit_%d_full", diff_bit);
    snprintf(detail_name, sizeof(detail_name), "bit_%d_full", diff_bit);

    if ((va1_base % PAGE_SIZE) != 0 || (va2_base % PAGE_SIZE) != 0) {
        print_full_failure(diff_bit);
        fprintf(stderr,
                "bit_%d skipped: VA1/VA2 must both be page aligned (va1=0x%016lx va2=0x%016lx)\n",
                diff_bit,
                (unsigned long)va1_base,
                (unsigned long)va2_base);
        return;
    }

    int fd = create_alias_fd();
    if (fd < 0) {
        print_full_failure(diff_bit);
        return;
    }

    uint8_t *va1 = map_alias(va1_base, fd, "VA1");
    uint8_t *va2 = map_alias(va2_base, fd, "VA2");
    if (!va1 || !va2) {
        if (va1) {
            munmap(va1, ALIAS_SIZE);
        }
        if (va2) {
            munmap(va2, ALIAS_SIZE);
        }
        close(fd);
        print_full_failure(diff_bit);
        return;
    }

    memset(va1, -1, ALIAS_SIZE);
    // for (size_t offset = 0; offset < ALIAS_SIZE; offset += LINE_SIZE) {
    //     mLoad_inline(va1 + offset);
    //     mLoad_inline(va2 + offset);
    // }
    // flush_aliases(va1, va2);

    run_case(result_name, detail_name, va1, va2, rounds, 1, 1);

    munmap(va2, ALIAS_SIZE);
    munmap(va1, ALIAS_SIZE);
    close(fd);
}

static void run_same_va_baselines(uintptr_t va1_base, uint64_t rounds) {
    if ((va1_base % PAGE_SIZE) != 0) {
        printf("same_va_full\t-1\n");
        printf("same_va_no_trigger\t-1\n");
        printf("same_va_no_trainer\t-1\n");
        fprintf(stderr,
                "same_va skipped: VA1 must be page aligned (va1=0x%016lx)\n",
                (unsigned long)va1_base);
        return;
    }

    int fd = create_alias_fd();
    if (fd < 0) {
        printf("same_va_full\t-1\n");
        printf("same_va_no_trigger\t-1\n");
        printf("same_va_no_trainer\t-1\n");
        return;
    }

    uint8_t *va1 = map_alias(va1_base, fd, "same_va");
    if (!va1) {
        close(fd);
        printf("same_va_full\t-1\n");
        printf("same_va_no_trigger\t-1\n");
        printf("same_va_no_trainer\t-1\n");
        return;
    }

    memset(va1, -1, ALIAS_SIZE);
    // for (size_t offset = 0; offset < ALIAS_SIZE; offset += LINE_SIZE) {
    //     mLoad_inline(va1 + offset);
    // }
    // flush_aliases(va1, va1);

    run_case("same_va_full", "same_va_full", va1, va1, rounds, 1, 1);
    run_case("same_va_no_trigger", "same_va_no_trigger", va1, va1, rounds, 1, 0);
    run_case("same_va_no_trainer", "same_va_no_trainer", va1, va1, rounds, 0, 1);

    munmap(va1, ALIAS_SIZE);
    close(fd);
}

int main(void) {
    uintptr_t va1_base = VA1_BASE;

    if (STRIDE_LINES <= 0 || STORE_ACCESSES < 2 ||
        ROUNDS <= 0 || PROBE_POSITIONS <= 0) {
        fprintf(stderr,
                "invalid STRIDE_LINES/STORE_ACCESSES/ROUNDS/PROBE_POSITIONS\n");
        return 1;
    }
    if (MIN_DIFF_BIT < 12 || MAX_DIFF_BIT < MIN_DIFF_BIT || MAX_DIFF_BIT >= 48) {
        fprintf(stderr, "diff bit range must be within [12, 47]\n");
        return 1;
    }
    if ((1ull << MIN_DIFF_BIT) < ALIAS_SIZE) {
        fprintf(stderr,
                "MIN_DIFF_BIT=%d makes VA aliases overlap: alias_size=%lu\n",
                MIN_DIFF_BIT,
                (unsigned long)ALIAS_SIZE);
        return 1;
    }
    if ((TRIGGER_POS + 1) * LINE_SIZE > ALIAS_SIZE) {
        fprintf(stderr, "trigger position exceeds one alias page\n");
        return 1;
    }
#if ENABLE_PROBE
    if (PREDICTED_POS >= PROBE_POSITIONS) {
        fprintf(stderr,
                "predicted probe position %d must be inside PROBE_POSITIONS=%d\n",
                PREDICTED_POS,
                PROBE_POSITIONS);
        return 1;
    }
    if ((PREDICTED_POS + 1) * LINE_SIZE > ALIAS_SIZE) {
        fprintf(stderr, "predicted probe position exceeds one alias page\n");
        return 1;
    }
#endif

    dummy_buffer = (uint8_t *)mmap(NULL,
                                  DUMMY_BUFFER_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                  -1,
                                  0);
    if (dummy_buffer == MAP_FAILED) {
        perror("mmap dummy_buffer");
        return 1;
    }

    printf("# %s-stride VA-index contribution test\n", STRIDE_ACCESS_NAME);
    printf("# VA1_BASE=0x%016lx stride_lines=%d accesses=%d train_only_accesses=%d rounds=%d probe_positions=%d\n",
           (unsigned long)va1_base,
           STRIDE_LINES,
           STORE_ACCESSES,
           TRAIN_ONLY_ACCESSES,
           ROUNDS,
           PROBE_POSITIONS);
    printf("# VA2_BASE = VA1_BASE ^ (1 << diff_bit)\n");
    printf("# VA1 and VA2 are MAP_SHARED aliases of the same memfd page\n");
    printf("# access=%s alias_size=%lu ENABLE_PMU=%d ENABLE_PROBE=%d ENABLE_CPP_RCTX=%d\n",
           STRIDE_ACCESS_NAME, (unsigned long)ALIAS_SIZE,
           ENABLE_PMU, ENABLE_PROBE, ENABLE_CPP_RCTX);
#if !ENABLE_PROBE
    printf("# probe disabled: no cache-line probe accesses or latency details\n");
#endif
    printf("# trainer: VA1 + [0..%d]*%d*LINE_SIZE, trigger: VA2 + %d*LINE_SIZE, final: VA2 + %d*LINE_SIZE\n",
           TRAIN_ONLY_ACCESSES - 1,
           STRIDE_LINES,
           TRIGGER_POS,
           PREDICTED_POS);
    printf("case\tlatency_ns\n");

#if ENABLE_PMU
    index_mem_va_pmu_ready = (pmu_setup() == 0);
    if (!index_mem_va_pmu_ready) {
        printf("# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
    }
#endif

    run_same_va_baselines(va1_base, ROUNDS);

    for (int diff_bit = MIN_DIFF_BIT; diff_bit <= MAX_DIFF_BIT; diff_bit++) {
        run_bit(diff_bit, va1_base, ROUNDS);
    }

#if ENABLE_PMU
    pmu_cleanup();
#endif
    munmap(dummy_buffer, DUMMY_BUFFER_SIZE);
    return 0;
}
