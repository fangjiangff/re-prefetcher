#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "../until.h"
// #include "victim.h"

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

#ifndef ENABLE_CPP_RCTX
#define ENABLE_CPP_RCTX 0
#endif

/* test0-exist.py enables ENABLE_CPP_RCTX only for Cortex-X925. */
#ifndef PMU_CORE_X925
#define PMU_CORE_X925 ENABLE_CPP_RCTX
#endif
#include "../pmu.h"


#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef SINGLE_PROBE
#define SINGLE_PROBE 0
#endif

#ifndef SINGLE_PROBE_POSITION
#define SINGLE_PROBE_POSITION (TRAIN_STEP * (STRIDE_BYTES / LINE_SIZE))
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
#define DUMMY_BUFFER_PAGES 10
#endif

#ifndef ENABLE_DUMMY_ACCESSES
#define ENABLE_DUMMY_ACCESSES 1
#endif

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

static uint8_t* dummy_buffer;

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
    mStore_inline(addr);
#endif
}

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


  uint64_t rounds = ROUNDS;

    int stride = STRIDE_BYTES;
    int train_step = TRAIN_STEP;
    if (train_step < 0 ||
        (train_step > 0 &&
         (uint64_t)(train_step - 1) * (uint64_t)stride >= Items * LINE_SIZE)) {
      fprintf(stderr, "training range exceeds array2 size\n");
      return 1;
    }

    printf("# ENABLE_CPP_RCTX=%d ENABLE_DUMMY_ACCESSES=%d "
           "DUMMY_BUFFER_PAGES=%d "
           "DUMMY_BUFFER_SIZE=%u\n",
           ENABLE_CPP_RCTX, ENABLE_DUMMY_ACCESSES,
           DUMMY_BUFFER_PAGES, DUMMY_BUFFER_SIZE);

          int pmu_ready = (pmu_setup() == 0);
          int pmu_running = 0;
          if (!pmu_ready) {
              printf("# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
          }

          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {

#if ENABLE_DUMMY_ACCESSES
            dummyAccesses();
#endif

            for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);
            }

            // reset prefetcher state
#if ENABLE_CPP_RCTX
            cpp_rctx();
#endif

            // begin to trainer the store stride prefetch
            if (pmu_ready && atkRound == 0) {
                pmu_running = (pmu_start() == 0);
                if (!pmu_running) {
                    printf("# PMU unavailable: counter group could not be started\n");
                }
            }

            for(int step = 0; step < train_step-1; step++){
                stride_access(array2 + (step * stride));
                // mfence();
                // nops();
            }
#if !NO_TRIGGER
            if (train_step > 0) {
                stride_access(array2 + ((train_step - 1) * stride));
                // mfence();
                // nops();
            }
#endif
            if (pmu_running && atkRound + 1 == rounds) {
                // Allow the final prefetch requests to complete before reading PMU counters.
                // struct timespec prefetch_wait = {.tv_sec = 0, .tv_nsec = 1000};
                // nanosleep(&prefetch_wait, NULL);
                pmu_stop_and_print(rounds);
                pmu_running = 0;
            }

            mfence();

#if SINGLE_PROBE
            int probe_pos = SINGLE_PROBE_POSITION;
#else
            int probe_pos = (atkRound) % PROBE_POSITIONS;//test one position each round
#endif
            probe_addr = array2 + (probe_pos * LINE_SIZE);

            time1 = timestamp();
            mStore_inline((void*)probe_addr);
            time2 = timestamp() - time1;

            latency_sum[probe_pos] += time2;
            probe_count[probe_pos]++;
            // printf("%llu\n", (unsigned long long)time2);
          }
          pmu_cleanup();
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
    //   }
      printf("\n");
//   }

  return 0;
}
