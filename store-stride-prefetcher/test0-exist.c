#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
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

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef NO_TRIGGER
#define NO_TRIGGER 0
#endif

#ifndef CONTEXT_SWITCH_BEFORE_TRIGGER
#define CONTEXT_SWITCH_BEFORE_TRIGGER 0
#endif

#ifndef CONTEXT_SWITCH_YIELDS
#define CONTEXT_SWITCH_YIELDS 1
#endif

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

uint8_t array1[100*LINE_SIZE]={0};

uint8_t array3[Items * LINE_SIZE] __attribute__((aligned(4096)));;

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

static void context_switch_before_trigger(void) {
#if CONTEXT_SWITCH_BEFORE_TRIGGER
    for (int i = 0; i < CONTEXT_SWITCH_YIELDS; i++) {
        sched_yield();
    }
#endif
}

static void print_test_header(int stride, int train_step, uint64_t rounds) {
    printf("# arm64 %s-stride prefetch latency map\n",
#if TRAIN_ACCESS_LOAD
           "load"
#else
           "store"
#endif
    );
    printf("# access mode: %s (%s), same noinline PC for train and trigger\n",
#if TRAIN_ACCESS_LOAD
           "load", "ldrb"
#else
           "store", "strb"
#endif
    );
    printf("# stride_bytes=%d train_step=%d rounds=%llu probe_positions=%d\n",
           stride, train_step, (unsigned long long)rounds, PROBE_POSITIONS);
    printf("# timer: clock_gettime(CLOCK_MONOTONIC) ns\n");
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
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


  uint64_t rounds = ROUNDS;

  // for(int stride = 64; stride <= 1024; stride+=64){
    int stride = STRIDE_BYTES;
    int train_step = TRAIN_STEP;
    if (train_step <= 0) {
      fprintf(stderr, "TRAIN_STEP must be positive\n");
      return 1;
    }
    if (stride <= 0) {
      fprintf(stderr, "STRIDE_BYTES must be positive\n");
      return 1;
    }
    if ((uint64_t)(train_step - 1) * (uint64_t)stride >= Items * LINE_SIZE) {
      fprintf(stderr, "training range exceeds array2 size\n");
      return 1;
    }
    print_test_header(stride, train_step, rounds);
      // for(int train_step = 1; train_step <= 32 ; train_step++){
          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
            dummyAccesses();//for dummy accesses , reset the prefetcher state


            for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);
              }
            //   mfence();
             

              for(int step = 0; step < train_step -1; step++){
                  stride_access(array2 + (step * stride));
              }

              context_switch_before_trigger();
                //trigger
#if !NO_TRIGGER
                stride_access(array2 + ((train_step -1) * stride));
#endif

              // }
              uint64_t dummy = 0;
              for(int k =0;k<100;k++){//wait for prefetch done.
                dummy += array1[k*64];
                // mfence();
              }
              for(int i=0;i<100;i++) {
                nop();
              }
              // mfence();

              int probe_pos = atkRound % PROBE_POSITIONS;//test one position each round
              
              probe_addr = array2 + (probe_pos * LINE_SIZE);
              
            //   mfence();
              time1 = timestamp();
              junk = *probe_addr;
              time2 = timestamp() - time1;

              latency_sum[probe_pos] += time2;
              probe_count[probe_pos]++;
          } 
          for(int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
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
      // }
      printf("\n");
  // }

  (void)junk;
  return 0;
}
