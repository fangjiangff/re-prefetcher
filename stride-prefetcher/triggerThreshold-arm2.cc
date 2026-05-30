#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include "cacheutils.hh"
#include "utils.hh"
#include <algorithm>
#include <random>
#include <sys/mman.h>
// #include <x86intrin.h>
#include "time.h"


#define CPU_ID 0
#define LINE_SIZE 64
// #define Items 256
#ifndef Items
#define Items 2048
#endif
#define Prefetch_Threshold 200

#ifndef TEST_ON_HIT
#define TEST_ON_HIT 0
#endif

#ifndef TEST_ON_SW
#define TEST_ON_SW 1
#endif

#ifndef TEST_ON_ST
#define TEST_ON_ST 0
#endif

#ifndef STRIDE
#define STRIDE 30
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 60
#endif


#define _maccess(pre, addr) \
    asm volatile( \
        pre "ldrb w0, [%0]\n\t" \
        :: "r" (addr) \
        : "memory", "w0")

#define REG_ARG_1 "x0"

#define mfence() asm volatile("DMB SY\nISB")

#define flush(addr) asm volatile("DC CIVAC, %0" :: "r" (addr))

#define return_asm() "ret"

#define nop() asm volatile("nop")

#define VIRTUAL_ADDRESS_BITS 48

void maccess(void *p) {
    _maccess("", p);
}

void mprefetch(void *p) {
    asm volatile(
        "PRFM PLDL1KEEP, [%0]\n\t"
        :
        : "r"(p)
        : "memory"
    );
}

static inline __attribute__((always_inline))
uint64_t timestamp(void)
{
    asm volatile("DSB SY");
    asm volatile("ISB");
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t res = t1.tv_sec * 1000 * 1000 * 1000ULL + t1.tv_nsec;
    asm volatile("ISB");
    asm volatile("DSB SY");
    return res;
}


// static inline __attribute__((always_inline)) uint64_t timestamp() {
//     uint64_t value;
//     asm volatile("mrs %0, PMCCNTR_EL0" : "=r" (value));
//     return value;
// }

// static inline __attribute__((always_inline)) uint64_t timestamp() {
//     uint64_t value;
//     asm volatile("DSB SY\nISB\n"
//                  "MRS %0, PMCCNTR_EL0\n"
//                  "ISB\n"
//                  : "=r" (value));
//     return value;
// }

// static inline __attribute__((always_inline)) uint64_t victim_probe(void *p) {
//     register uint64_t start, end;
//     // DEBUG("probing victim buffer at offset %zu\n", offset);
//     mfence();
//     // DEBUG("accessing victim buffer at offset %zu\n", offset);
//     start = timestamp();
//     // DEBUG("probing victim buffer at offset %zu\n", offset);
//     mfence();
//     maccess(p);
//     mfence();
//     end = timestamp();
//     mfence();  
//     return end - start;
// }


uint8_t array1[100*LINE_SIZE]={0};

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;
// uint8_t *array2;

long long int res2[100][Items] = {0};


#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)
#define DUMMY_LINES (DUMMY_BUFFER_SIZE / LINE_SIZE)

static uint8_t* dummy_buffer;
static uint32_t dummy_order[DUMMY_LINES];

void initDummyAccesses(){
    for(uint32_t i = 0; i < DUMMY_LINES; i++){
        dummy_order[i] = i;
    }

    std::mt19937 rng(0x5eed1234);
    std::shuffle(dummy_order, dummy_order + DUMMY_LINES, rng);
}

__attribute__((always_inline)) static inline void maccess_inline(void *p) {
    volatile uint32_t value;
    asm volatile("LDR %0, [%1]\n\t" : "=r"(value) : "r"(p));
}

__attribute__((always_inline)) static inline void prefetchch_inline(void *p) {
    // volatile uint32_t value;
    asm volatile("PRFM PLDL1KEEP, [%0]\n\t" : : "r"(p));
}

void dummyAccesses(){
     for(uint32_t j = 0; j < DUMMY_LINES; j++){
        uint64_t i = dummy_order[j] * LINE_SIZE;
        asm volatile("PRFM PLDL3KEEP, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
     }
}

int main(){
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;

  // PinCore(CPU_ID);
  // victim_init();
  struct timespec const t_req{ .tv_sec = 0, .tv_nsec = 1 /* 1µs */ };//秒0，纳秒1us
  struct timespec t_rem;

  // printf("test_on_hit %d, test_on_sw %d\n", TEST_ON_HIT, TEST_ON_SW);

  memset(array2,-1,Items*LINE_SIZE*sizeof(uint8_t));
  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }
  initDummyAccesses();

  // EnableStride(CPU_ID);
  int i;    
  for(int i=0; i< Items; i++){
    maccess(&array2[i * 64]);
  }

  for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
    flush(&array2[offset]);
  }


  int rounds = 100;

  // for(int stride = 64; stride <= 4096; stride+=64){
    int stride = STRIDE * LINE_SIZE;
      // printf("Stride %d*64:\t%d\t\t",stride/64,stride);
      for(int train_step = 1; train_step <= 40 ; train_step++){
        
        for(int pos = 0;pos < PROBE_POSITIONS; pos++){//test one position
            // dummyAccesses();
            for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
              for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                // _mm_clflush(&array2[offset]);
                flush(&array2[offset]);
              }

              // dummyAccesses();       
              
              for(int n=0;n<100;n++) nop();
              // with stride prefetcher training
              for(int repeat = 0; repeat < 5; repeat ++) {
                //train the prefetcher..
                for(int step = 0; step < train_step -1; step++){
                    if(TEST_ON_SW){
                      // sw prefetch
                      mprefetch(array2 + (step * stride));
                      mfence();
                    }else{
                      maccess(array2 + (step * stride));
                      mfence();
                    }
                }
                if(TEST_ON_SW){
                  mprefetch(array2 + ((train_step -1) * stride));
                  mfence();
                }else{
                  maccess(array2 + ((train_step -1) * stride)); 
                  mfence();
                }
              }

              //wait for prefetch done.
              // uint64_t dummy = 0;
              // for(int k =0;k<100;k++){
              //   dummy += array1[k*64];
              //   mfence();
              // }
              // for(int i=0;i<100;i++) nop(); mfence();

              //test the different position.
              probe_addr = array2 + (pos * stride);
              // // // /* READ TIMER */
              time1 = timestamp();
              mfence();
              /* MEMORY ACCESS TO TIME */
              junk = *probe_addr;
              mfence();
              /* READ TIMER */
              time2 = timestamp() - time1;
              res2[train_step][pos] += time2;

              // time2 = victim_probe(array2 + (pos * stride));
              // res2[train_step][pos] += time2;

              if(pos < train_step) res2[train_step][pos] = 0;//these positions are all cache hit, no need to test.
          } 
          printf("%lld\t", res2[train_step][pos]/rounds);
          // printf("train_step %d pos %d:\t,latency %lld\n", train_step, pos, res2[train_step][pos]/rounds);
        }
        printf("\n");
      }
  // }
  return 0;
}
