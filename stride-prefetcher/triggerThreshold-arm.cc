#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include "cacheutils.hh"
#include "utils.hh"
#include <random>
#include <sys/mman.h>
// #include <x86intrin.h>
#include "time.h"


#ifndef CPU_ID
#define CPU_ID 0
#endif
#define LINE_SIZE 64
// #define Items 256
#define Items 8000
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

#ifndef PROBE_POSITION
#define PROBE_POSITION 1
#endif

#if TEST_ON_ST == 1
    /*
     * Store: 向 addr 写 1 字节
     * 使用 w 寄存器（低 8 bit 会被使用）
     */
    #define _maccess(pre, addr) \
        asm volatile( \
            pre "strb w0, [%0]\n\t" \
            :: "r" (addr) \
            : "memory", "w0")

#else
    /*
     * Load: 从 addr 读 1 字节
     */
    #define _maccess(pre, addr) \
        asm volatile( \
            pre "ldrb w0, [%0]\n\t" \
            :: "r" (addr) \
            : "memory", "w0")

#endif


#define REG_ARG_1 "x0"

#define mfence() asm volatile("DMB SY\nISB")

#define flush(addr) asm volatile("DC CIVAC, %0" :: "r" (addr))

#define return_asm() "ret"

#define nop() asm volatile("nop")

#define VIRTUAL_ADDRESS_BITS 48

void maccess(void *p) {
    _maccess("", p);
    // volatile uint32_t value;
    // asm volatile("LDR %0, [%1]\n\t" : "=r"(value) : "r"(p));
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

static inline __attribute__((always_inline)) uint64_t victim_probe(void *p) {
    register uint64_t start, end;
    // DEBUG("probing victim buffer at offset %zu\n", offset);
    mfence();
    // DEBUG("accessing victim buffer at offset %zu\n", offset);
    start = timestamp();
    // DEBUG("probing victim buffer at offset %zu\n", offset);
    mfence();
    maccess(p);
    mfence();
    end = timestamp();
    mfence();  
    return end - start;
}


uint8_t array1[100*LINE_SIZE]={0};

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;
// uint8_t *array2;

long long int res2[100][100] = {0};


#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

static uint8_t* dummy_buffer;

// void dummyAccesses(){
//      for(uint64_t i = 0; i < DUMMY_BUFFER_SIZE; i += 64){
//         maccess(&dummy_buffer[i]); 
//      }
// }

void dummyAccesses(){
     for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
        uint64_t i = j;
        asm volatile("PRFM PLDL3KEEP, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
     }
}

int main(){
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;

  PinCore(CPU_ID);
  // victim_init();
  struct timespec const t_req{ .tv_sec = 0, .tv_nsec = 1 /* 1µs */ };//秒0，纳秒1us
  struct timespec t_rem;

  if (PROBE_POSITION < 1) {
    fprintf(stderr, "PROBE_POSITION must be >= 1\n");
    return 1;
  }

  if ((20 + PROBE_POSITION - 1) * 4096 >= Items * LINE_SIZE) {
    fprintf(stderr, "PROBE_POSITION=%d is out of array2 bounds for max stride\n", PROBE_POSITION);
    return 1;
  }

  // printf("test_on_hit %d, test_on_sw %d\n", TEST_ON_HIT, TEST_ON_SW);

  memset(array2,-1,Items*LINE_SIZE*sizeof(uint8_t));
  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }

  // EnableStride(CPU_ID);
  int i;    
  for(int i=0; i< Items; i++){
    maccess(&array2[i * 64]);
  }

  if(TEST_ON_HIT){
      // printf("Test on Hit:\n");
  }else{
    // printf("Test on Miss:\n");
    // victim_flush_buffer();
    for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
      flush(&array2[offset]);
    }
  }

  int rounds = 1000;

  for(int stride = 64; stride <= 4096; stride+=64){
      // printf("Stride %d*64:\t%d\t\t",stride/64,stride);
      for(int train_step = 1; train_step <= 40 ; train_step++){
          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
              dummyAccesses();//for dummy accesses , reset the prefetcher state

              if(TEST_ON_HIT){
                // _mm_clflush(&array2[train_step * stride]);
                for(int step = 0; step < train_step; step++){
                  array2[step * stride] = 1;//access to bring into cache
                }
                flush(&array2[train_step * stride]);
                mfence();
              }else{
                for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  // _mm_clflush(&array2[offset]);
                  flush(&array2[offset]);
                }
              }
              // with stride prefetcher training
              for(int repeat = 0; repeat < 5; repeat ++) {

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
                //trigger..
                //   
                // mfence();
                if(TEST_ON_SW){
                  // sw prefetch
                  mprefetch(array2 + ((train_step -1) * stride));
                  mfence();
                }else{
                  maccess(array2 + ((train_step -1) * stride)); 
                  mfence();
                }
              }
              // nanosleep(&t_req, &t_rem);
              uint64_t dummy = 0;
              for(int k =0;k<100;k++){//wait for prefetch done.
                dummy += array1[k*64];
                mfence();
              }
              for(int i=0;i<100;i++) nop(); mfence();
              // test the Nth expected prefetched address.
              probe_addr = array2 + ((train_step + PROBE_POSITION - 1) * stride);
              /* READ TIMER */
              time1 = timestamp();
              /* MEMORY ACCESS TO TIME */
              junk = *probe_addr;
              /* READ TIMER */
              time2 = timestamp() - time1;

              // time2 = victim_probe(array2 + ((train_step) * stride));
              res2[stride/64][train_step] += time2;
          } 
          printf("%lld\t", res2[stride/64][train_step]/rounds);
      }
      printf("\n");
  }
  return 0;
}
