#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "../../until.h"
// #include "victim.h" 

#define Items 262144

#ifndef STRIDE_BYTES
#define STRIDE_BYTES 64
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

#ifndef ENABLE_PMU
#define ENABLE_PMU 0
#endif

#if ENABLE_PMU
#ifndef PMU_CORE_X925
#define PMU_CORE_X925 ENABLE_CPP_RCTX
#endif
#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif
#ifndef PMU_WINDOW_NAME
#define PMU_WINDOW_NAME "train-trigger-per-round"
#endif
#include "../pmu.h"

#if PMU_CORE_X925
#define SWEEP_PMU_EVENT_NAME "l2d_cache_hwprf"
#elif PMU_CORE_A55
#define SWEEP_PMU_EVENT_NAME "l2d_cache_refill_prefetch"
#else
#define SWEEP_PMU_EVENT_NAME "l2d_cache_prf"
#endif
#endif


#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef TRAIN_ACCESS_PREFETCH
#define TRAIN_ACCESS_PREFETCH 0
#endif

#ifndef DUMMY_BUFFER_PAGES
#define DUMMY_BUFFER_PAGES 10
#endif

#ifndef ENABLE_DUMMY_ACCESSES
#define ENABLE_DUMMY_ACCESSES 1
#endif

#if TRAIN_ACCESS_LOAD && TRAIN_ACCESS_PREFETCH
#error "Only one train access mode can be enabled"
#endif

#ifndef MIN_STRIDE
#define MIN_STRIDE 0
#endif

// Stride macros use cache-line units; byte addresses multiply by LINE_SIZE.
#ifndef MAX_STRIDE
#define MAX_STRIDE 64
#endif


#ifndef MIN_STEP
#define MIN_STEP 1
#endif

#ifndef MAX_STEP
#define MAX_STEP 20
#endif

#ifndef PROBE_CANDIDATES
#define PROBE_CANDIDATES 10
#endif

#if !ENABLE_PMU && PROBE_CANDIDATES < 10
#error "PROBE_CANDIDATES must be at least 10"
#endif

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

uint8_t array1[100*LINE_SIZE]={0};

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)
#define PREFETCHER_FILL_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t* dummy_buffer;
static uint8_t* prefetcher_fill_buffer;


void dummyAccesses(void){
  // dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
    for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
        // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[j]) : "memory", "w0");
        // asm volatile("STR wzr, [%0]\n\t" :: "r"(&dummy_buffer[j]) : "memory");

     }
}

static inline __attribute__((always_inline)) void stride_access(void *addr) {
#if TRAIN_ACCESS_PREFETCH
    mPrefetch_noinline(addr);
#elif TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
    mStore_inline(addr);
#endif
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

#if ENABLE_PMU
static int find_sweep_pmu_counter(void) {
    for (int i = 0; i < PMU_COUNTER_COUNT; i++) {
        if (strcmp(pmu_counters[i].name, SWEEP_PMU_EVENT_NAME) == 0) {
            return i;
        }
    }
    return -1;
}

static double sweep_pmu_per_round(int counter_index, uint64_t rounds) {
    const struct pmu_counter *counter = &pmu_counters[counter_index];
    double scaled = (double)counter->accumulated_value;

    if (counter->accumulated_time_running != 0 &&
        counter->accumulated_time_running < counter->accumulated_time_enabled) {
        scaled *= (double)counter->accumulated_time_enabled /
                  (double)counter->accumulated_time_running;
    }
    return rounds != 0 ? scaled / (double)rounds : 0.0;
}
#endif


int main(){
#if !ENABLE_PMU
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;
#endif


  memset(array2,-1,Items*LINE_SIZE*sizeof(uint8_t));


  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }

  prefetcher_fill_buffer = (uint8_t*)mmap(NULL, PREFETCHER_FILL_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(prefetcher_fill_buffer == MAP_FAILED) {
      printf("failed to map prefetcher fill buffer!\n");
      exit(1);
  }

//   for(int i=0; i< Items; i++){
//     mLoad(&array2[i * 64]);
//   }


//   for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
//     flush(&array2[offset]);
//   }
//   mfence();


  if (MAX_STRIDE < 1) {
    fprintf(stderr, "MAX_STRIDE must be >= 1 cache line\n");
    return 1;
  }
  if (MIN_STRIDE < 0 || MIN_STRIDE > MAX_STRIDE) {
    fprintf(stderr, "MIN_STRIDE must be >= 0 and <= MAX_STRIDE\n");
    return 1;
  }
  if (MIN_STEP < 1 || MIN_STEP > MAX_STEP) {
    fprintf(stderr, "MIN_STEP must be >= 1 and <= MAX_STEP\n");
    return 1;
  }
#if ENABLE_PMU
  if ((uint64_t)(MAX_STEP - 1) * (uint64_t)MAX_STRIDE >= Items) {
    fprintf(stderr, "training range exceeds array2 size\n");
    return 1;
  }
#else
  if ((uint64_t)(MAX_STEP + PROBE_CANDIDATES - 1) *
          (uint64_t)MAX_STRIDE >= Items) {
    fprintf(stderr, "probe range exceeds array2 size\n");
    return 1;
  }
#endif

  printf("# %s %s-stride trigger sweep\n",
#ifdef __x86_64__
         "x86_64",
#elif defined(__aarch64__)
         "arm64",
#else
         "unknown",
#endif
#if TRAIN_ACCESS_PREFETCH
         "prefetch"
#elif TRAIN_ACCESS_LOAD
         "load"
#else
         "store"
#endif
  );
  printf("# timer: %s %s\n", TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME);
  printf("# ENABLE_CPP_RCTX=%d ENABLE_DUMMY_ACCESSES=%d "
         "ENABLE_PMU=%d DUMMY_BUFFER_PAGES=%d DUMMY_BUFFER_SIZE=%u\n",
         ENABLE_CPP_RCTX, ENABLE_DUMMY_ACCESSES, ENABLE_PMU,
         DUMMY_BUFFER_PAGES, DUMMY_BUFFER_SIZE);
  printf("# MIN_STRIDE=%d MAX_STRIDE=%d MIN_STEP=%d\n",
         MIN_STRIDE, MAX_STRIDE, MIN_STEP);
#if ENABLE_PMU
  printf("# stride_lines train_step avg_%s\n", SWEEP_PMU_EVENT_NAME);
#else
  printf("# latency mode probes %d candidate positions after training\n",
         PROBE_CANDIDATES);
  printf("# stride_lines train_step best_probe_distance min_avg_%s"
         " probe_avg_%s_d1...d%d\n",
         TIMESTAMP_UNIT_NAME, TIMESTAMP_UNIT_NAME, PROBE_CANDIDATES);
#endif

#if ENABLE_PMU
  if (pmu_setup() != 0) {
      fprintf(stderr, "PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
      return 1;
  }
  int sweep_pmu_counter = find_sweep_pmu_counter();
  if (sweep_pmu_counter < 0 || pmu_counters[sweep_pmu_counter].fd < 0) {
      fprintf(stderr, "PMU event %s is unavailable\n", SWEEP_PMU_EVENT_NAME);
      pmu_cleanup();
      return 1;
  }
#endif

  for(int stride_idx = MIN_STRIDE; stride_idx <= MAX_STRIDE; stride_idx++){
  // for(int stride_idx = 4; stride_idx < 5; stride_idx++){
    int stride_lines = stride_idx;
      uint64_t stride = (uint64_t)stride_lines * LINE_SIZE;
      for(int train_step_idx = MIN_STEP; train_step_idx <= MAX_STEP; train_step_idx++){
      //  for(int train_step_idx = 5; train_step_idx < 6; train_step_idx++){
          int train_step = train_step_idx;
#if ENABLE_PMU
          pmu_reset_accumulated();
#else
          long long int probe_latency_sum[PROBE_CANDIDATES] = {0};
          uint64_t probe_count[PROBE_CANDIDATES] = {0};
#endif
          for(uint64_t atkRound = 0; atkRound < ROUNDS; ++atkRound) {
#if !ENABLE_PMU
            int probe_index = (int)(atkRound % PROBE_CANDIDATES);
            uint64_t probe_offset =
                (uint64_t)(train_step + probe_index) * (uint64_t)stride;
#endif

            int flush_step_count = train_step;
#if !ENABLE_PMU
            flush_step_count += PROBE_CANDIDATES;
#endif
            for (int flush_step = 0; flush_step < flush_step_count;
                 flush_step++) {
                  flush(array2 + ((uint64_t)flush_step * (uint64_t)stride));
            }

// #if ENABLE_DUMMY_ACCESSES && !ENABLE_CPP_RCTX
            occupy_store_prefetcher_entries(prefetcher_fill_buffer,
                                            DUMMY_BUFFER_PAGES, 6);
// #endif
            mfence();
#if ENABLE_PMU
            if (pmu_start() != 0 || pmu_counters[sweep_pmu_counter].fd < 0) {
                fprintf(stderr, "PMU event %s could not be started\n",
                        SWEEP_PMU_EVENT_NAME);
                pmu_cleanup();
                return 1;
            }
#endif

            // reset prefetcher state
#if ENABLE_CPP_RCTX
            cpp_rctx();
#endif
            
              // for(int repeat = 0; repeat < 5; repeat ++) {
              for(int step = 0; step < train_step -1; step++){
                  stride_access(array2 + (step * stride));
                  nops();
                  // mfence();
              }
              // }
              // trigger.
              stride_access(array2 + ((train_step -1) * stride));
              nops();
              // mfence();
              
              nops();
              struct timespec prefetch_wait = {.tv_sec = 0, .tv_nsec = 100};
              nanosleep(&prefetch_wait, NULL);
#if ENABLE_PMU
              pmu_stop_and_accumulate();
#else

            //   delay_after_trigger();
              //probe
              probe_addr = array2 + probe_offset;
              
              time1 = timestamp();
            //   junk = *probe_addr;
              mStore_inline((void*)probe_addr);
              time2 = timestamp() - time1;
              probe_latency_sum[probe_index] += time2;
              probe_count[probe_index]++;
#endif
          }
#if ENABLE_PMU
          printf("%d\t%d\t%.2f\n",
                 stride_lines,
                 train_step,
                 sweep_pmu_per_round(sweep_pmu_counter, ROUNDS));
#else
          long long int probe_avg[PROBE_CANDIDATES];
          long long int min_avg = -1;
          int best_probe_distance = 0;

          for (int probe_index = 0; probe_index < PROBE_CANDIDATES;
               probe_index++) {
              probe_avg[probe_index] =
                  probe_count[probe_index] != 0
                      ? probe_latency_sum[probe_index] /
                            (long long int)probe_count[probe_index]
                      : -1;
              if (probe_avg[probe_index] >= 0 &&
                  (min_avg < 0 || probe_avg[probe_index] < min_avg)) {
                  min_avg = probe_avg[probe_index];
                  best_probe_distance = probe_index + 1;
              }
          }

          printf("%d\t%d\t%d\t%lld",
                 stride_lines, train_step, best_probe_distance, min_avg);
          for (int probe_index = 0; probe_index < PROBE_CANDIDATES;
               probe_index++) {
              printf("\t%lld", probe_avg[probe_index]);
          }
          printf("\n");
#endif
      }
  }

#if ENABLE_PMU
  pmu_cleanup();
#endif
#if !ENABLE_PMU
  (void)junk;
#endif
  return 0;
}
