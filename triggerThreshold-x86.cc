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

void maccess(void* p)
{
  // printf("maccess %x\n",p);
  asm volatile ("movq (%0), %%rax\n"
    :
    : "c" (p)
    : "rax");
}

#define CPU_ID 0
#define LINE_SIZE 64
// #define Items 256
#define Items 2048
#define Prefetch_Threshold 120
#define nop() asm volatile("nop")

// #define TEST_ON_HIT 1
#ifndef TEST_ON_HIT
#define TEST_ON_HIT 1
#endif

#ifndef TEST_ON_SW
#define TEST_ON_SW 0
#endif

#ifndef TEST_ON_ST
#define TEST_ON_ST 0
#endif

// 0 Load 1 Store 2 Prefetch
// if TEST_ON_ST
//   #define _maccess(pre, addr) asm volatile(pre "mov %%al, (%0)" :: "r" (addr) : "memory") //store
// else
//   #define _maccess(pre, addr) asm volatile(pre "mov (%0), %%al" :: "r" (addr) : "rax")//load

#if TEST_ON_ST == 1
    /* * 情况 1: TEST_ON_ST 为 1 -> 执行 Store (写入)
     * 注意：Clobber list 使用 "memory"，告知编译器内存已被修改
     */
    #define _maccess(pre, addr) asm volatile(pre "mov %%al, (%0)" :: "r" (addr) : "memory")//store

#else
    /* * 情况 2: TEST_ON_ST 为 0 (或未定义) -> 执行 Load (读取)
     * 注意：Clobber list 使用 "rax"，因为修改了 al 寄存器
     */
    #define _maccess(pre, addr) asm volatile(pre "mov (%0), %%al" :: "r" (addr) : "rax")//load

#endif

#define _mprefetch(pre, addr)  asm volatile(pre "prefetcht0 (%0)" :: "r" (addr))

#define return_asm() "ret"
#define REG_ARG_1 "rdi"

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;
// static uint8_t* array2 = NULL;
// uint8_t *array2;

long long int res2[100][100] = {0};


#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

static uint8_t* dummy_buffer;

void dummyAccesses(){
     for(uint64_t i = 0; i < DUMMY_BUFFER_SIZE; i += 64){
        maccess(&dummy_buffer[i]); 
     }
}

static __attribute__((noinline, naked)) void maccess_gadget(uint64_t offset) {
    register uintptr_t r __asm__(REG_ARG_1) = (uintptr_t) &array2[offset];
    _maccess(
        ".global _victim_gadget\n"
        "_victim_gadget:\n",
        r
    );
    asm volatile(return_asm());
}

static __attribute__((noinline, naked)) void prefetch_gadget(uint64_t offset) {
    register uintptr_t r __asm__(REG_ARG_1) = (uintptr_t) &array2[offset];
    _mprefetch(
        ".global _victim_pf_gadget\n"
        "_victim_pf_gadget:\n",
        r
    );
    asm volatile(return_asm());
}

// TIME STAMP END
uint64_t accessLatency(uint8_t* addr)
{
  uint64_t start, end;
  mfence();
  start = rdtsc();
  // mfence();
  maccess(addr);
  // mfence();
  end = rdtsc();
  mfence();
  return end - start;
}


int main(){
  // printf("test_on_hit %d, test_on_st %d, test_on_sw %d\n", TEST_ON_HIT, TEST_ON_ST, TEST_ON_SW);
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;

  PinCore(CPU_ID);
  // victim_init();
  struct timespec const t_req{ .tv_sec = 0, .tv_nsec = 15000 /* 1µs */ };//秒0，纳秒1us
  struct timespec t_rem;

  memset(array2,-1,Items*LINE_SIZE*sizeof(uint8_t));

  // array2 = (uint8_t*)mmap(NULL, Items*LINE_SIZE*sizeof(uint8_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);
  // if(array2 == MAP_FAILED) {
  //       printf("failed to map victim buffer!\n");
  //       return -1;
  // }

  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }

  EnableStride(CPU_ID);
  int i;    
  for(int i=0; i< Items; i++){
    maccess(&array2[i * 64]);
  }

  if(TEST_ON_HIT){
      // printf("Test on Hit:\n");
  }else{
    // printf("Test on Miss:\n");
    for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
      _mm_clflush(&array2[offset]);
    }
  }

  int rounds = 100;

  for(int stride = 64; stride <= 1024; stride+=64){
      // printf("Stride %d*64:\t",stride/64);
      for(int train_step = 1; train_step <= 32 ; train_step++){
          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {

              for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  _mm_clflush(&array2[offset]);
              }

              dummyAccesses();//for dummy accesses , reset the prefetcher state

              // with stride prefetcher training
              for(int repeat = 0; repeat < 5; repeat ++) {
                 if(TEST_ON_HIT){
                  for(int step = 0; step < train_step; step++){
                    int mix_step = ((step * 7) + 41) % train_step;
                    maccess_gadget(mix_step * stride);//make sure it is loaded into cache
                    mfence();
                  }
                  _mm_clflush(&array2[train_step * stride]);
                }

                for(int step = 0; step < train_step -1; step++){
                    if(TEST_ON_SW){
                      prefetch_gadget(step * stride);
                    }
                    else{
                      maccess_gadget(step * stride);
                    }
                    mfence();
                }
                //trigger..
                if(TEST_ON_SW){
                  prefetch_gadget((train_step -1) * stride);
                }
                else{
                  maccess_gadget((train_step -1) * stride);
                }
                mfence();
              }
              // int dummy = array2[0];
              
              nanosleep(&t_req, &t_rem);
              probe_addr = array2 + (train_step * stride);
              time1 = __rdtscp( & junk); /* READ TIMER */
              // mfence();
              junk = * probe_addr; /* MEMORY ACCESS TO TIME */
              // mfence();
              time2 = __rdtscp( & junk) - time1; 
              // time2 = accessLatency((uint8_t*)probe_addr);
              // if(time2 < Prefetch_Threshold) res2[stride/64][train_step] ++;
               res2[stride/64][train_step] += time2;
          } 
          printf("%lld\t", res2[stride/64][train_step]/rounds);
      }
      printf("\n");
  }
  EnableALL(CPU_ID);

  return 0;
}
