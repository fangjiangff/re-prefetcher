#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include "cacheutils.hh"
#include "utils.hh"
#include <random>
#include <sys/mman.h>
#include <x86intrin.h>
#include "time.h"
// #include "victim.h" 

#ifndef CPU_ID
#define CPU_ID 0
#endif
#define LINE_SIZE 64
// #define Items 256
#define Items 4096
#define nop() asm volatile("nop")

#ifndef STRIDE_BYTES
#define STRIDE_BYTES 64
#endif

#ifndef TRAIN_STEP
#define TRAIN_STEP 5
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 100
#endif

#ifndef TEST_ON_SW
#define TEST_ON_SW 0
#endif

#define _maccess(pre, addr) asm volatile(pre "mov (%0), %%al" :: "r" (addr) : "rax")//load

#define _mprefetch(pre, addr)  asm volatile(pre "prefetcht2 (%0)" :: "r" (addr))

void maccess(void* p){
    _maccess("", p);
}

void mprefetch(void* p){
    _mprefetch("", p);
}

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

uint8_t array1[100*LINE_SIZE]={0};

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

static uint8_t* dummy_buffer;

void dummyAccesses(){
    uint64_t tmp = 0;
     for(uint64_t i = 0; i < DUMMY_BUFFER_SIZE; i += 64){
        tmp += dummy_buffer[i]; 
     }
    (void)tmp;
}

static void print_test_header(int stride, int train_step, int rounds) {
    printf("# x86 stride prefetch latency map\n");
    printf("# access mode: %s\n", TEST_ON_SW ? "software prefetch (prefetcht0)" : "load");
    printf("# stride_bytes=%d train_step=%d rounds=%d probe_positions=%d\n",
           stride, train_step, rounds, PROBE_POSITIONS);
    printf("# position\toffset_bytes\tavg_cycles\tprobes\n");
}



// TIME STAMP END
uint64_t accessLatency(uint8_t* addr)
{
  uint64_t start, end;
  mfence();
  start = rdtsc();
  
  maccess(addr);

  end = rdtsc();
  mfence();
  return end - start;
}


int main(){
  // printf("test_on_hit %d, test_on_st %d, test_on_sw %d\n", TEST_ON_HIT, TEST_ON_ST, TEST_ON_SW);
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;


  struct timespec const t_req{ .tv_sec = 0, .tv_nsec = 15000 /* 1µs */ };//秒0，纳秒1us
  struct timespec t_rem;

  memset(array2,-1,Items*LINE_SIZE*sizeof(uint8_t));


  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }

  for(int i=0; i< Items; i++){
    maccess(&array2[i * 64]);
  }


  for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
    _mm_clflush(&array2[offset]);
  }


  int rounds = ROUNDS;

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
    print_test_header(stride, train_step, rounds);
      // for(int train_step = 1; train_step <= 32 ; train_step++){
          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
              for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  _mm_clflush(&array2[offset]);
              }
              dummyAccesses();//for dummy accesses , reset the prefetcher state

              // with stride prefetcher training
              for(int repeat = 0; repeat < 5; repeat ++) {

                for(int step = 0; step < train_step -1; step++){
                    if(TEST_ON_SW){
                      mprefetch(array2 + (step * stride));
                    }
                    else{
                      maccess(array2 + (step * stride));
                    }
                    mfence();
                }
                //trigger
                if(TEST_ON_SW){
                  mprefetch(array2 + ((train_step -1) * stride));
                }
                else{
                  maccess(array2 + ((train_step -1) * stride));
                }
                mfence();
              }
              
                // nanosleep(&t_req, &t_rem);
              uint64_t dummy = 0;
              for(int k =0;k<100;k++){//wait for prefetch done.
                dummy += array1[k*64];
                mfence();
              }
              for(int i=0;i<100;i++) nop(); mfence();

              // nanosleep(&t_req, &t_rem);
              int probe_pos = atkRound % PROBE_POSITIONS;//test one position each round
              probe_addr = array2 + (probe_pos * LINE_SIZE);

              time1 = __rdtscp( & junk); /* READ TIMER */
              // mfence();
              junk = * probe_addr; /* MEMORY ACCESS TO TIME */
              // mfence();
              time2 = __rdtscp( & junk) - time1; 

              latency_sum[probe_pos] += time2;
              probe_count[probe_pos]++;
          } 
          for(int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
              long long int avg_cycles = 0;
              if(probe_count[probe_pos] > 0) {
                  avg_cycles = latency_sum[probe_pos] / probe_count[probe_pos];
              }
              printf("%3d\t%12d\t%10lld\t%5d\n",
                     probe_pos,
                     probe_pos * LINE_SIZE,
                     avg_cycles,
                     probe_count[probe_pos]);
          }
      // }
      printf("\n");
  // }

  return 0;
}
