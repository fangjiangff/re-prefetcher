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

// 64 cache lines.
#ifndef MAX_STRIDE
#define MAX_STRIDE 4096
#endif


#ifndef MAX_STEP
#define MAX_STEP 20
#endif


uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

#define MAX_STRIDE_LINES (MAX_STRIDE / LINE_SIZE)

long long int latency_sum[MAX_STRIDE_LINES + 1][MAX_STEP + 1] = {{0}};

uint8_t array1[100*LINE_SIZE]={0};

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)
#define PREFETCHER_FILL_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t* dummy_buffer;
static uint8_t* prefetcher_fill_buffer;

static uint32_t shuffle_state = 0x9e3779b9u;

static uint32_t next_shuffle_rand(void) {
    shuffle_state = shuffle_state * 1664525u + 1013904223u;
    return shuffle_state;
}

static void shuffle_ints(int *values, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = (int)(next_shuffle_rand() % (uint32_t)(i + 1));
        int tmp = values[i];

        values[i] = values[j];
        values[j] = tmp;
    }
}

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


  if (MAX_STRIDE % LINE_SIZE != 0) {
    fprintf(stderr, "MAX_STRIDE must be a multiple of LINE_SIZE\n");
    return 1;
  }
  if ((uint64_t)MAX_STEP * (uint64_t)MAX_STRIDE >= Items * LINE_SIZE) {
    fprintf(stderr, "probe range exceeds array2 size\n");
    return 1;
  }

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
         "DUMMY_BUFFER_PAGES=%d DUMMY_BUFFER_SIZE=%u\n",
         ENABLE_CPP_RCTX, ENABLE_DUMMY_ACCESSES,
         DUMMY_BUFFER_PAGES, DUMMY_BUFFER_SIZE);
  printf("# stride_lines train_step avg_%s\n", TIMESTAMP_UNIT_NAME);

  int stride_order[MAX_STRIDE_LINES];
  int train_step_order[MAX_STEP];

  for (int i = 0; i < MAX_STRIDE_LINES; i++) {
      stride_order[i] = i + 1;
  }
  for (int i = 0; i < MAX_STEP; i++) {
      train_step_order[i] = i + 1;
  }

  shuffle_ints(stride_order, MAX_STRIDE_LINES);
  shuffle_ints(train_step_order, MAX_STEP);


  for(int stride_idx = 0; stride_idx < MAX_STRIDE_LINES; stride_idx++){
  // for(int stride_idx = 4; stride_idx < 5; stride_idx++){
      // int stride_lines = stride_order[stride_idx];
    int stride_lines = stride_idx + 1;//stride = 5
      int stride = stride_lines * LINE_SIZE;
      for(int train_step_idx = 0; train_step_idx < MAX_STEP; train_step_idx++){
      //  for(int train_step_idx = 5; train_step_idx < 6; train_step_idx++){
        //   int train_step = train_step_order[train_step_idx];
          int train_step = train_step_idx + 1;//trian=6
          for(uint64_t atkRound = 0; atkRound < ROUNDS; ++atkRound) {
            uint64_t probe_offset = (uint64_t)train_step * (uint64_t)stride;

            for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);
            }

// #if ENABLE_DUMMY_ACCESSES && !ENABLE_CPP_RCTX
            occupy_store_prefetcher_entries(prefetcher_fill_buffer,
                                            DUMMY_BUFFER_PAGES, 6);
// #endif
            mfence();

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

            //   delay_after_trigger();
              //probe
              
              
              probe_addr = array2 + probe_offset;
              
              time1 = timestamp();
            //   junk = *probe_addr;
              mStore_inline((void*)probe_addr);
              time2 = timestamp() - time1;
              latency_sum[stride_lines][train_step] += time2;
          }
          printf("%d\t%d\t%lld\n",
                 stride_lines,
                 train_step,
                 latency_sum[stride_lines][train_step]/ROUNDS);
      }
  }

  (void)junk;
  return 0;
}
