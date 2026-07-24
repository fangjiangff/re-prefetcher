#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "../until.h"
// #include "victim.h" 

#define Items 10240

#ifndef TEST_STRIDE
#define TEST_STRIDE 320
#endif

#ifndef PROBES
#define PROBES 40
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif


#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef TRAIN_ACCESS_PREFETCH
#define TRAIN_ACCESS_PREFETCH 0
#endif

#ifndef ENABLE_CPP_RCTX
#define ENABLE_CPP_RCTX 0
#endif

#if TRAIN_ACCESS_LOAD && TRAIN_ACCESS_PREFETCH
#error "Only one train access mode can be enabled"
#endif

#ifndef MAX_STEP
#define MAX_STEP 20
#endif


uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

long long int latency_sum[MAX_STEP + 1][PROBES] = {{0}};

uint8_t array1[100*LINE_SIZE]={0};

#define DUMMY_BUFFER_PAGES 10
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t* dummy_buffer;

void dummyAccesses(void){
  // dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
       for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
        // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
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

static void delay_after_trigger(void) {
    nops();
    struct timespec prefetch_wait = {.tv_sec = 0, .tv_nsec = 100};
    nanosleep(&prefetch_wait, NULL);
}


int main(){
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;


  memset(array2,-1,Items*LINE_SIZE*sizeof(uint8_t));


  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }

  // for(int i=0; i< Items; i++){
  //   mLoad(&array2[i * LINE_SIZE]);
  // }


  // for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
  //   flush(&array2[offset]);
  // }
  // mfence();


  if (TEST_STRIDE <= 0 || TEST_STRIDE % LINE_SIZE != 0) {
    fprintf(stderr, "TEST_STRIDE must be a positive multiple of LINE_SIZE\n");
    return 1;
  }
  if (MAX_STEP > PROBES) {
    fprintf(stderr, "PROBES must be >= MAX_STEP to include all explicitly accessed positions\n");
    return 1;
  }
  if ((uint64_t)(PROBES - 1) * (uint64_t)TEST_STRIDE >= Items * LINE_SIZE) {
    fprintf(stderr, "probe range exceeds array2 size\n");
    return 1;
  }

  printf("# %s %s-stride degree sweep\n",
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
  printf("# stride_bytes=%d stride_lines=%d max_step=%d probes=%d rounds=%d\n",
         TEST_STRIDE, TEST_STRIDE / LINE_SIZE, MAX_STEP, PROBES, ROUNDS);
  printf("# timer: %s %s\n", TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME);
  printf("# train_step probe_pos offset_bytes role avg_%s\n", TIMESTAMP_UNIT_NAME);

  int stride = TEST_STRIDE;
      for(int train_step = 1; train_step <= MAX_STEP ; train_step++){
        for(int pos = 0; pos < PROBES; pos++){//test one position
          for(uint64_t atkRound = 0; atkRound < ROUNDS; ++atkRound) {
            
#if ENABLE_CPP_RCTX
            cpp_rctx();
#endif
            mfence();
            dummyAccesses();//for dummy accesses , reset the prefetcher state
            mfence();

            for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);
            }
            occupy_store_prefetcher_entries(dummy_buffer, DUMMY_BUFFER_PAGES, 6);
              
              for(int step = 0; step < train_step -1; step++){
                  stride_access(array2 + (step * stride));
              }
              // trigger.
              stride_access(array2 + ((train_step -1) * stride));
              
              delay_after_trigger();
              
              probe_addr = array2 + (pos*stride);
              
              time1 = timestamp();
              junk = *probe_addr;
              time2 = timestamp() - time1;
              latency_sum[train_step][pos] += time2;
          } 
          printf("%d\t%d\t%d\t%s\t%lld\n",
                 train_step,
                 pos,
                 pos * stride,
                 pos < train_step ? "accessed" : "candidate",
                 latency_sum[train_step][pos]/ROUNDS);
        }
      }
  // }

  (void)junk;
  return 0;
}
