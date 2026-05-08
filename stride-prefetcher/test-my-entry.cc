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


#define CPU_ID 0
#define LINE_SIZE 64
// #define Items 256
#define Items 2048
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

uint8_t array1[100*64]={0};
uint8_t array3[100*1024]={0};
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

// static inline __attribute__((always_inline)) 
void mprefetch(void *p) {
    asm volatile(
        "PRFM PLDL1KEEP, [%0]\n\t"
        :
        : "r"(p)
        : "memory"
    );
}

static inline __attribute__((always_inline)) 
void mprefetch_inline(void *p) {
    asm volatile(
        "PRFM PLDL1KEEP, [%0]\n\t"
        :
        : "r"(p)
        : "memory"
    );
}

static inline __attribute__((always_inline))
uint64_t timestamp1(void)
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

static inline __attribute__((always_inline)) uint64_t timestamp() {
    uint64_t value;
    asm volatile("mrs %0, PMCCNTR_EL0" : "=r" (value));
    return value;
}
static inline __attribute__((always_inline)) uint64_t victim_probe(void *p) {
    register uint64_t start, end;
    mfence();
    start = timestamp();
    mfence();
    maccess(p);
    mfence();
    end = timestamp();
    mfence();  
    return end - start;
}



uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;
// uint8_t *array2;

long long int res2[100][100] = {0};


#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

static uint8_t* dummy_buffer;

void dummyAccesses(){
     for(uint64_t i = 0; i < DUMMY_BUFFER_SIZE; i += 64){
        maccess(&dummy_buffer[i]); 
     }
}

int main(){
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;

  // PinCore(CPU_ID);
  // victim_init();
  struct timespec const t_req{ .tv_sec = 0, .tv_nsec = 1500 /* 1µs */ };//秒0，纳秒1us
  struct timespec t_rem;

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

  // for(int stride = 64*1; stride <= 64*64; stride+=64){
    //   printf("Stride %d*64:\t%d\t\t",stride/64,stride);
    int stride = 10 * 64;
    int train_step = 5;
    for(int competitors=0;competitors<100;competitors++){
    for(int train_step2 = 0; train_step2 <= 20 ; train_step2++){
    // int train_step = 16;
      // int stride = 64*16;
      // int stride = 64*8;
      // int train_step = 19;
      printf("Stride %d*64:\t%d\ttrian_step %d\t",stride/64,stride, train_step2);
      uint64_t latency = 0;
      int res[1000]={0};
          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
              dummyAccesses();//for dummy accesses , reset the prefetcher state
                for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  // _mm_clflush(&array2[offset]);
                  flush(&array2[offset]);
                }
                for(int step = 0; step < train_step-1; step++){
                    // sw prefetch
                    mprefetch(array2 + (step * stride));
                    mfence();
                }
                
                for(int c =0;c<competitors;c++){
                    for(int tt=0;tt<train_step2;tt++){
                        mprefetch_inline(array3 + c*1024 + tt*stride);
                        mfence();
                    } 
                }
              //trigger 
              mprefetch(array2 + ((train_step - 1) * stride));
              
              uint64_t dummy = 0;
              for(int k =0;k<100;k++){
                dummy += array1[k*64];
                mfence();
              }

              for(int i=0;i<100;i++) nop(); mfence();

              probe_addr = array2 + ((train_step+15) * stride);
              // mfence();
              time1 = timestamp1();
              // mfence();
              // junk = *probe_addr; 
              maccess(array2 + ((train_step+15) * stride));
              // mfence();
              /* READ TIMER */
              time2 = timestamp1();
              // mfence();
              latency += (time2-time1);
          } 
        //   printf("%lld\n", latency/rounds);
          printf("competitors %d: latency %lld\n", competitors, latency/rounds);
            }
            printf("\n");
        }
      // printf("\n");
  // }
  return 0;
}
