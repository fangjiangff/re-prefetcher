#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../../until.h"
// #include "victim.h"

#define Items 10240

#ifndef STRIDE_BYTES
#define STRIDE_BYTES 64
#endif

#ifndef TRAIN_START_LINE
#define TRAIN_START_LINE 3
#endif
#if TRAIN_START_LINE < 0 || TRAIN_START_LINE >= Items
#error "TRAIN_START_LINE must be inside array2"
#endif

#ifndef TRAIN_STEP
#define TRAIN_STEP 10
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef ENABLE_CPP_RCTX
#define ENABLE_CPP_RCTX 0
#endif

/* test0-exist.py passes explicit PMU_CORE_* flags for known Arm cores. */
#ifndef PMU_CORE_X925
#define PMU_CORE_X925 ENABLE_CPP_RCTX
#endif
#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif
#ifndef ENABLE_PMU
#define ENABLE_PMU 1
#endif
#ifndef CALIBRATE_PROBE
#define CALIBRATE_PROBE 0
#endif
#ifndef PMU_CORE_KP920
#define PMU_CORE_KP920 0
#endif
#if PMU_CORE_KP920
#define PMU_WINDOW_NAME "register-only-training+nops+barrier-separate-pass"
#endif
#if ENABLE_PMU
#include "../pmu.h"
#endif


#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef SINGLE_PROBE
#define SINGLE_PROBE 0
#endif

#ifndef SINGLE_PROBE_POSITION
#define SINGLE_PROBE_POSITION (TRAIN_START_LINE + TRAIN_STEP * (STRIDE_BYTES / LINE_SIZE))
#endif

#ifndef ENABLE_PROBE
#define ENABLE_PROBE 1
#endif

#if SINGLE_PROBE && (SINGLE_PROBE_POSITION < 0 || SINGLE_PROBE_POSITION >= PROBE_POSITIONS)
#error "SINGLE_PROBE_POSITION must be inside PROBE_POSITIONS"
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef TRAIN_ACCESS_PREFETCH
#define TRAIN_ACCESS_PREFETCH 0
#endif

#ifndef DUMMY_BUFFER_PAGES
#define DUMMY_BUFFER_PAGES 64
#endif

#ifndef ENABLE_DUMMY_ACCESSES
#define ENABLE_DUMMY_ACCESSES 1
#endif

#ifndef ENABLE_SCHED_YIELD
#define ENABLE_SCHED_YIELD 2
#endif

#ifndef ENABLE_GETPID_SYSCALL
#define ENABLE_GETPID_SYSCALL 0
#endif

#ifndef ENABLE_INVALID_SYSCALL
#define ENABLE_INVALID_SYSCALL 0
#endif

/*
 * Enter EL1 through the normal Linux arm64 syscall path without executing a
 * real syscall body.  -1 is outside the arm64 syscall-number range, so Linux
 * returns -ENOSYS in x0 after the common syscall entry/exit path.
 */
static inline long invalid_syscall(void)
{
    register long x0 asm("x0");
    register long x8 asm("x8") = -1L;

    asm volatile("svc #0"
                 : "=r"(x0)
                 : "r"(x8)
                 : "memory", "cc");
    return x0;
}

#if TRAIN_ACCESS_LOAD && TRAIN_ACCESS_PREFETCH
#error "Only one train access mode can be enabled"
#endif

#ifndef NO_TRIGGER
#define NO_TRIGGER 0
#endif


#define ARRAY2_SIZE (Items * LINE_SIZE * sizeof(uint8_t))

static uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};


#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)
#define PREFETCHER_FILL_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t* dummy_buffer;
static uint8_t* prefetcher_fill_buffer;

void dummyAccesses(void){
    for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
     }
}

static inline __attribute__((always_inline)) void stride_access(void *addr) {
#if TRAIN_ACCESS_PREFETCH
    mPrefetch_noinline(addr);
#elif TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
    mStore_noinline(addr);
#endif
}

/* Copied from test-prefetcher/stride_probe.c for controlled ablation. */
#define KP_POOL_SIZE (512UL * 1024 * 1024)
#define POLLUTION_SIZE (64UL * 1024 * 1024)
#define POLLUTION_STREAMS 512
#define POLLUTION_STEPS 8
#define POLLUTION_MAX_STRIDE_LINES 32
#define POLLUTION_PCS 8
typedef enum { ACCESS_LOAD, ACCESS_STORE } access_type_t;

static inline uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}


/* ============================================================
 * Multiple PCs for prefetcher pollution
 * ============================================================ */

typedef uint8_t (*load_pc_fn_t)(volatile uint8_t *);
typedef void (*store_pc_fn_t)(volatile uint8_t *, uint8_t);

__attribute__((noinline, noclone)) static uint8_t load_pc0(volatile uint8_t *p) { return *p; }
__attribute__((noinline, noclone)) static uint8_t load_pc1(volatile uint8_t *p) { return *p; }
__attribute__((noinline, noclone)) static uint8_t load_pc2(volatile uint8_t *p) { return *p; }
__attribute__((noinline, noclone)) static uint8_t load_pc3(volatile uint8_t *p) { return *p; }
__attribute__((noinline, noclone)) static uint8_t load_pc4(volatile uint8_t *p) { return *p; }
__attribute__((noinline, noclone)) static uint8_t load_pc5(volatile uint8_t *p) { return *p; }
__attribute__((noinline, noclone)) static uint8_t load_pc6(volatile uint8_t *p) { return *p; }
__attribute__((noinline, noclone)) static uint8_t load_pc7(volatile uint8_t *p) { return *p; }

__attribute__((noinline, noclone)) static void store_pc0(volatile uint8_t *p, uint8_t v) { *p = v; }
__attribute__((noinline, noclone)) static void store_pc1(volatile uint8_t *p, uint8_t v) { *p = v; }
__attribute__((noinline, noclone)) static void store_pc2(volatile uint8_t *p, uint8_t v) { *p = v; }
__attribute__((noinline, noclone)) static void store_pc3(volatile uint8_t *p, uint8_t v) { *p = v; }
__attribute__((noinline, noclone)) static void store_pc4(volatile uint8_t *p, uint8_t v) { *p = v; }
__attribute__((noinline, noclone)) static void store_pc5(volatile uint8_t *p, uint8_t v) { *p = v; }
__attribute__((noinline, noclone)) static void store_pc6(volatile uint8_t *p, uint8_t v) { *p = v; }
__attribute__((noinline, noclone)) static void store_pc7(volatile uint8_t *p, uint8_t v) { *p = v; }

static load_pc_fn_t load_pcs[POLLUTION_PCS] = {
    load_pc0, load_pc1, load_pc2, load_pc3,
    load_pc4, load_pc5, load_pc6, load_pc7
};

static store_pc_fn_t store_pcs[POLLUTION_PCS] = {
    store_pc0, store_pc1, store_pc2, store_pc3,
    store_pc4, store_pc5, store_pc6, store_pc7
};


/* ============================================================
 * Prefetcher pollution
 *
 * 512 streams × 8 accesses = 4096 explicit accesses.
 * If all cache lines are unique:
 *
 * 4096 × 64 B = 256 KiB ≈ 4 × 64 KiB L1D.
 *
 * Each stream uses:
 *   - random base
 *   - random stride from 1~32 cache lines
 *   - one of 8 different access PCs
 *
 * This is state pollution/scrubbing, NOT architectural reset.
 * ============================================================ */

static void pollute_prefetcher(uint8_t *buf, size_t buf_size, access_type_t type, uint64_t *rng)
{
    volatile uint8_t sink = 0;
    const size_t max_span = POLLUTION_STEPS * POLLUTION_MAX_STRIDE_LINES * LINE_SIZE;

    for (int stream = 0; stream < POLLUTION_STREAMS; stream++) {
        size_t stride_lines = 1 + (xorshift64(rng) % POLLUTION_MAX_STRIDE_LINES);
        size_t stride = stride_lines * LINE_SIZE;
        size_t usable = buf_size - max_span;
        size_t offset = xorshift64(rng) % usable;
        offset &= ~(LINE_SIZE - 1);

        uint8_t *base = buf + offset;
        int pc = stream % POLLUTION_PCS;

        for (int i = 0; i < POLLUTION_STEPS; i++) {
            volatile uint8_t *addr = base + (size_t)i * stride;

            if (type == ACCESS_LOAD) {
                sink ^= load_pcs[pc](addr);
            } else {
                store_pcs[pc](addr, (uint8_t)(stream + i));
            }
        }
    }

    mfence();
    (void)sink;
}


static void shuffle_regions(size_t *regions, size_t count, uint64_t *rng)
{
    if (count <= 1) return;

    for (size_t i = count - 1; i > 0; i--) {
        size_t j = xorshift64(rng) % (i + 1);
        size_t tmp = regions[i];
        regions[i] = regions[j];
        regions[j] = tmp;
    }
}



static int kp_env_int(const char *name, int fallback, int maximum) {
    const char *value = getenv(name);
    if (!value) return fallback;
    char *end;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end || parsed < 0 || parsed > maximum) {
        fprintf(stderr, "Invalid %s=%s\n", name, value);
        exit(EXIT_FAILURE);
    }
    return (int)parsed;
}

/* Match stride_probe's training_access, including its volatile load sink. */
static inline void kp_training_access(volatile uint8_t *addr, uint8_t value,
                                      volatile uint8_t *sink) {
#if TRAIN_ACCESS_LOAD
    *sink ^= *addr;
#elif TRAIN_ACCESS_PREFETCH
    mPrefetch_inline((void *)addr);
#else
    *addr = value;
#endif
}

#if ENABLE_PMU && PMU_CORE_KP920 && defined(__aarch64__)
/* All argument loads and register saves precede ENABLE. No stack/global
 * access occurs between successful ENABLE and DISABLE. Both latency and
 * PMU passes execute this function's same training PCs (fd=-1 for latency).
 * ioctl request numbers come from the Linux perf_event UAPI. */
_Static_assert(PERF_EVENT_IOC_ENABLE == 0x2400, "Unexpected ENABLE ioctl");
_Static_assert(PERF_EVENT_IOC_DISABLE == 0x2401, "Unexpected DISABLE ioctl");
_Static_assert(__NR_ioctl == 29, "Unexpected arm64 ioctl syscall");
#if TRAIN_ACCESS_LOAD
#define KP_CALL_ACCESS "bl mLoad_noinline\n"
#define KP_INLINE_ACCESS "ldrb w9, [x19]\neor w25, w25, w9\n"
#elif TRAIN_ACCESS_PREFETCH
#define KP_CALL_ACCESS "bl mPrefetch_noinline\n"
#define KP_INLINE_ACCESS "prfm pldl1keep, [x19]\n"
#else
#define KP_CALL_ACCESS "bl mStore_noinline\n"
#define KP_INLINE_ACCESS "strb w24, [x19]\n"
#endif

long kp_register_window(void *base, size_t stride, int count, int stage, int fd);
__asm__(
    ".text\n.align 2\n.global kp_register_window\n"
    ".type kp_register_window, %function\n"
    "kp_register_window:\n"
    "stp x29, x30, [sp, #-96]!\n"
    "stp x19, x20, [sp, #16]\n"
    "stp x21, x22, [sp, #32]\n"
    "stp x23, x24, [sp, #48]\n"
    "stp x25, x26, [sp, #64]\n"
    "mov x29, sp\n"
    "mov x19, x0\nmov x20, x1\nmov w21, w2\nmov w22, w3\n"
    "sxtw x23, w4\nmov w24, #1\nmov w25, #0\n"
    "dsb sy\nisb\n"
    "tbnz x23, #63, 1f\n" /* fd=-1: latency-only */
    "mov x0, x23\nmov x1, #0x2400\nmov x2, #0\nmov x8, #29\n"
    "svc #0\n"
    ".global kp_window_enabled\nkp_window_enabled:\n"
    "cbnz x0, 9f\n"
    "1:\ncbz w21, 4f\ncmp w22, #3\nb.ge 3f\n"
    "2:\nmov x0, x19\n"
    KP_CALL_ACCESS
    "add x19, x19, x20\n"
    "cmp w24, w21\nadd w24, w24, #1\nb.lt 2b\nb 4f\n"
    "3:\n"
    KP_INLINE_ACCESS
    "add x19, x19, x20\n"
    "cmp w24, w21\nadd w24, w24, #1\nb.lt 3b\n"
    "4:\nmov w9, #100\n"
    "5:\nnop\nsubs w9, w9, #1\nb.ne 5b\n"
    "dsb sy\nisb\n"
    "tbnz x23, #63, 8f\n"
    "mov x0, x23\nmov x1, #0x2401\nmov x2, #0\nmov x8, #29\n"
    ".global kp_window_disabling\nkp_window_disabling:\n"
    "svc #0\nb 9f\n"
    "8:\nmov x0, #0\n"
    "9:\nldp x25, x26, [sp, #64]\nldp x23, x24, [sp, #48]\n"
    "ldp x21, x22, [sp, #32]\nldp x19, x20, [sp, #16]\n"
    "ldp x29, x30, [sp], #96\nret\n"
    ".size kp_register_window, .-kp_register_window\n"
);

static void kp_read_disabled_counter(void) {
    struct pmu_read_value result;
    if (read(pmu_counters[0].fd, &result, sizeof(result)) != sizeof(result)) {
        perror("PMU: read disabled PRF_REQ");
        exit(EXIT_FAILURE);
    }
    if (!result.time_running || result.time_running != result.time_enabled) {
        fprintf(stderr, "PMU: PRF_REQ not fully scheduled; counts discarded\n");
        exit(EXIT_FAILURE);
    }
    pmu_counters[0].accumulated_value += result.value;
    pmu_counters[0].accumulated_time_enabled += result.time_enabled;
    pmu_counters[0].accumulated_time_running += result.time_running;
}
#endif

#if CALIBRATE_PROBE
/* Match the experiment's store probe, including timestamp barriers. */
static int calibrate_probe(void) {
    static uint8_t line[LINE_SIZE] __attribute__((aligned(4096)));
    uint64_t hits[201], misses[201];
    for (int i = 0; i < 201; i++) {
        mStore_inline(line);
        uint64_t start = timestamp();
        mStore_inline(line);
        hits[i] = timestamp() - start;
        flush(line);
        mfence();
        start = timestamp();
        mStore_inline(line);
        misses[i] = timestamp() - start;
    }
    /* Medians reduce sensitivity to scheduling interrupts. */
    for (int i = 1; i < 201; i++) {
        uint64_t h = hits[i], m = misses[i];
        int j = i;
        while (j > 0 && hits[j - 1] > h) { hits[j] = hits[j - 1]; j--; }
        hits[j] = h;
        j = i;
        while (j > 0 && misses[j - 1] > m) { misses[j] = misses[j - 1]; j--; }
        misses[j] = m;
    }
    printf("# CACHE_PROBE threshold=%llu hit=%llu miss=%llu unit=%s\n",
           (unsigned long long)(hits[100] + (misses[100] > hits[100] ?
               (misses[100] - hits[100]) / 2 : 0)),
           (unsigned long long)hits[100], (unsigned long long)misses[100],
           TIMESTAMP_UNIT_NAME);
    if (misses[100] <= hits[100]) {
        fprintf(stderr, "Cache probe calibration cannot separate hits and misses; specify --hit-threshold-ns using a measured threshold\n");
        return -1;
    }
    return 0;
}
#endif

int main(){
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;


  memset(array2, -1, ARRAY2_SIZE);


  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }
  // mfence();

  prefetcher_fill_buffer = (uint8_t*)mmap(NULL, PREFETCHER_FILL_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(prefetcher_fill_buffer == MAP_FAILED) {
      printf("failed to map prefetcher fill buffer!\n");
      exit(1);
  }


  uint64_t rounds = ROUNDS;

    int stride = STRIDE_BYTES;
    int train_step = kp_env_int("KP_TRAIN_STEP", TRAIN_STEP, Items);
    int kp_stage = kp_env_int("KP_STAGE", 0, 3);
#if TRAIN_ACCESS_PREFETCH
    if (kp_stage >= 2) {
        fprintf(stderr, "Random pollution supports load/store only\n");
        return 1;
    }
#endif
    const size_t train_start_offset = (size_t)TRAIN_START_LINE * LINE_SIZE;
    if (train_step < 0 ||
        (train_step > 0 &&
         train_start_offset + (uint64_t)(train_step - 1) * (uint64_t)stride >= Items * LINE_SIZE)) {
      fprintf(stderr, "training range exceeds array2 size\n");
      return 1;
    }

    /* Allocate/prefault identically in every stage. Flush remains ARRAY2_SIZE
     * in all stages; the tail padding allows that range at the last region.
     * Maintenance ranges can overlap, but training/probe regions never do. */
    size_t span = train_start_offset + (size_t)train_step * stride + LINE_SIZE;
    if (span < PROBE_POSITIONS * LINE_SIZE) span = PROBE_POSITIONS * LINE_SIZE;
    size_t region_size = ((span + 4096 + 4095) / 4096) * 4096;
    size_t num_regions = KP_POOL_SIZE / region_size;
    size_t required_regions = (ENABLE_PMU && PMU_CORE_KP920 ? 2 : 1) * rounds;
    if (kp_stage >= 1 && num_regions < required_regions) {
        fprintf(stderr, "Insufficient independent regions: %zu < %zu\n",
                num_regions, required_regions);
        return 1;
    }
    uint8_t *kp_pool = mmap(NULL, KP_POOL_SIZE + ARRAY2_SIZE,
                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                           -1, 0);
    uint8_t *kp_pollution = mmap(NULL, POLLUTION_SIZE,
                                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                                -1, 0);
    size_t *regions = malloc(num_regions * sizeof(*regions));
    if (kp_pool == MAP_FAILED || kp_pollution == MAP_FAILED || !regions) {
        perror("KP ablation allocation");
        return 1;
    }
    for (size_t i = 0; i < KP_POOL_SIZE + ARRAY2_SIZE; i += 4096) kp_pool[i] = 1;
    for (size_t i = 0; i < POLLUTION_SIZE; i += 4096) kp_pollution[i] = 1;
    for (size_t i = 0; i < num_regions; ++i) regions[i] = i;
    uint64_t region_rng = 0x8f3c7a21d5b964e1ULL;
    shuffle_regions(regions, num_regions, &region_rng);
    uint64_t pollution_rng = region_rng;
    printf("# KP_ABLATION stage=%d train_step=%d region_size=%zu "
           "flush_bytes=%zu rounds=%llu seed=0x8f3c7a21d5b964e1\n",
           kp_stage, train_step, region_size, (size_t)ARRAY2_SIZE,
           (unsigned long long)rounds);

    printf("# TRAIN_START_LINE=%d offset_bytes=%zu\n", TRAIN_START_LINE, train_start_offset);
    printf("# ENABLE_CPP_RCTX=%d PMU_CORE_X925=%d PMU_CORE_A55=%d "
           "ENABLE_DUMMY_ACCESSES=%d "
           "ENABLE_SCHED_YIELD=%d ENABLE_GETPID_SYSCALL=%d "
           "ENABLE_INVALID_SYSCALL=%d "
           "DUMMY_BUFFER_PAGES=%d "
           "DUMMY_BUFFER_SIZE=%u\n",
           ENABLE_CPP_RCTX, PMU_CORE_X925, PMU_CORE_A55,
           ENABLE_DUMMY_ACCESSES,
           ENABLE_SCHED_YIELD, ENABLE_GETPID_SYSCALL,
           ENABLE_INVALID_SYSCALL,
           DUMMY_BUFFER_PAGES, DUMMY_BUFFER_SIZE);

#if CALIBRATE_PROBE
          if (calibrate_probe() != 0) return 1;
#endif
#if ENABLE_PMU
          int pmu_ready = 0;
#if !PMU_CORE_KP920
          pmu_ready = (pmu_setup() == 0);
          if (!pmu_ready) {
              printf("# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
          }
#endif

#else
          printf("# PMU disabled: cache probe only\n");
#endif

          /* KP920 repeats the same loop/PCs with probes disabled for PMU.
           * No PMU syscall intervenes between training and latency probing. */
          const int passes = (ENABLE_PMU && PMU_CORE_KP920) ? 2 : 1;
          for (int pass = 0; pass < passes; ++pass) {
#if ENABLE_PMU && PMU_CORE_KP920
            if (pass == 1) {
                pmu_ready = (pmu_setup() == 0);
                if (!pmu_ready) {
                    fprintf(stderr, "PMU: KP920 PRF_REQ setup failed\n");
                    pmu_cleanup();
                    return 1;
                }
            }
#endif
          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {

            uint8_t *round_base = array2;
            if (kp_stage >= 1) {
                size_t region_index = (size_t)pass * rounds + atkRound;
                round_base = kp_pool + regions[region_index] * region_size;
            }
            volatile uint8_t train_sink = 0; /* Outside PMU window. */
            if (kp_stage >= 2) {
                pollute_prefetcher(kp_pollution, POLLUTION_SIZE,
                                  TRAIN_ACCESS_LOAD ? ACCESS_LOAD : ACCESS_STORE,
                                  &pollution_rng);
            } else {
#if ENABLE_DUMMY_ACCESSES
                dummyAccesses();
#endif
                occupy_store_prefetcher_entries(prefetcher_fill_buffer,
                                                DUMMY_BUFFER_PAGES, 1);
            }
            for (uint64_t offset = 0; offset < ARRAY2_SIZE; offset += LINE_SIZE) {
                flush(round_base + offset);
            }

            mfence();

   
#if ENABLE_PMU && PMU_CORE_KP920
            int window_fd = pmu_ready ? pmu_counters[0].fd : -1;
            if (window_fd >= 0 && ioctl(window_fd, PERF_EVENT_IOC_RESET, 0) != 0) {
                perror("PMU: reset PRF_REQ");
                return 1;
            }
            long window_status = kp_register_window(round_base + train_start_offset,
                                                     stride, train_step, kp_stage,
                                                     window_fd);
            if (window_status != 0) {
                fprintf(stderr, "PMU: raw ioctl boundary failed: %ld\n", window_status);
                return 1;
            }
            if (window_fd >= 0) kp_read_disabled_counter();
#else
#if ENABLE_PMU
            int pmu_running = 0;
            if (pmu_ready) {
                pmu_running = (pmu_start() == 0);
                if (!pmu_running) {
                    printf("# PMU unavailable: counter group could not be started\n");
                    pmu_ready = 0;
#if PMU_CORE_KP920
                    pmu_cleanup();
                    return 1;
#endif
                }
            }
#endif

#if ENABLE_CPP_RCTX
            cpp_rctx();
#endif

            if (kp_stage >= 3) {
                for (int step = 0; step < train_step; ++step) {
                    volatile uint8_t *addr = round_base + train_start_offset +
                                             (size_t)step * stride;
                    kp_training_access(addr, (uint8_t)(step + 1), &train_sink);
                }
            } else {
                for (int step = 0; step < train_step; ++step) {
                    stride_access(round_base + train_start_offset +
                                  (size_t)step * stride);
                }
            }

#if ENABLE_SCHED_YIELD
        for(int i=0;i<10;i++){
            sched_yield();
        }        
#endif


#if ENABLE_GETPID_SYSCALL
            (void)syscall(SYS_getpid);
#endif

#if ENABLE_INVALID_SYSCALL
            (void)invalid_syscall();
#endif


// #if !NO_TRIGGER
//             if (train_step > 0) {
//                 stride_access(array2 + train_start_offset + ((train_step - 1) * stride));
//                 // nops();
//             }
// #endif

            nops();

#if ENABLE_PMU
            if (pmu_running) {
#if PMU_CORE_KP920
                mfence();
#endif
                pmu_stop_and_accumulate();
            }

#endif
#endif
            // mfence();

#if ENABLE_PROBE
            if (pass == 0) {
#if SINGLE_PROBE
            int probe_pos = SINGLE_PROBE_POSITION;
#else
            int probe_pos = (atkRound) % PROBE_POSITIONS;//test one position each round
#endif
            probe_addr = round_base + (probe_pos * LINE_SIZE);

            time1 = timestamp();
            mStore_inline((void*)probe_addr);
            time2 = timestamp() - time1;

            latency_sum[probe_pos] += time2;
            probe_count[probe_pos]++;
            }
#endif
          }
          } /* passes */
#if ENABLE_PMU
          if (pmu_ready) {
              pmu_print_accumulated(rounds);
          }
          pmu_cleanup();
#endif
#if ENABLE_PROBE
#if SINGLE_PROBE
          int first_probe_pos = SINGLE_PROBE_POSITION;
          int last_probe_pos = SINGLE_PROBE_POSITION + 1;
#else
          int first_probe_pos = 0;
          int last_probe_pos = PROBE_POSITIONS;
#endif
          for(int probe_pos = first_probe_pos; probe_pos < last_probe_pos; probe_pos++) {
              long long int avg_ns = 0;
              if(probe_count[probe_pos] > 0) {
                  avg_ns = latency_sum[probe_pos] / probe_count[probe_pos];
              }
              printf("%3d\t%12d\t%10lld\t%5d\n",
                     probe_pos,
                     probe_pos * LINE_SIZE,
                     avg_ns,
                     probe_count[probe_pos]);
          }
#endif
    //   }
      printf("\n");
//   }

  free(regions);
  munmap(kp_pool, KP_POOL_SIZE + ARRAY2_SIZE);
  munmap(kp_pollution, POLLUTION_SIZE);
  munmap(dummy_buffer, DUMMY_BUFFER_SIZE);
  munmap(prefetcher_fill_buffer, PREFETCHER_FILL_BUFFER_SIZE);
  return 0;
}
