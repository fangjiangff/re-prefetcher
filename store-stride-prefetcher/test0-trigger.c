#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "until.h"
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


#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
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

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

static uint8_t* dummy_buffer;

void dummyAccesses(void){
  dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
}

static inline __attribute__((always_inline)) void stride_access(void *addr) {
#if TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
    mStore_noinline(addr);
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

  for(int i=0; i< Items; i++){
    mLoad(&array2[i * 64]);
  }


  for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
    flush(&array2[offset]);
  }
  mfence();


  if (MAX_STRIDE % LINE_SIZE != 0) {
    fprintf(stderr, "MAX_STRIDE must be a multiple of LINE_SIZE\n");
    return 1;
  }
  if ((uint64_t)MAX_STEP * (uint64_t)MAX_STRIDE >= Items * LINE_SIZE) {
    fprintf(stderr, "probe range exceeds array2 size\n");
    return 1;
  }

  printf("# arm64 %s-stride trigger sweep\n",
#if TRAIN_ACCESS_LOAD
         "load"
#else
         "store"
#endif
  );
  printf("# stride_lines train_step avg_ns\n");

  for(int stride = 64; stride <= MAX_STRIDE; stride+=64){
      int stride_lines = stride / LINE_SIZE;
      for(int train_step = 1; train_step <= MAX_STEP ; train_step++){
          for(uint64_t atkRound = 0; atkRound < ROUNDS; ++atkRound) {

            dummyAccesses();//for dummy accesses , reset the prefetcher state
            
            for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);
            }
              
              for(int step = 0; step < train_step -1; step++){
                  stride_access(array2 + (step * stride));
              }
              // trigger.
              stride_access(array2 + ((train_step -1) * stride));
              
              // delay_after_trigger();

              //probe
              uint64_t probe_offset = (uint64_t)train_step * (uint64_t)stride;
              
              probe_addr = array2 + probe_offset;
              
              time1 = timestamp();
              junk = *probe_addr;
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
