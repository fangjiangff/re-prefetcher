#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
// #include "victim.h" 

#ifndef __aarch64__
#error "store_stride_poc uses AArch64 DC CIVAC, DSB/ISB, and STRB/LDRB."
#endif

#ifndef CPU_ID
#define CPU_ID 0
#endif
#define LINE_SIZE 64
#define PAGE_SIZE 4096
// #define Items 256
#define Items 10240
static inline void nop(void) {
    asm volatile("nop");
}

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


static inline void mfence(void) {
    asm volatile("DSB SY\nISB" ::: "memory");
}

static inline void flush(void *addr) {
    asm volatile("DC CIVAC, %0" :: "r" (addr) : "memory");
}


#define _mStore(addr, val)                 \
asm volatile(                                      \
    "strb %w[value], [%[address]]\n\t"          \
    :                                              \
    : [address] "r" (addr), [value] "r" ((uint32_t)(val)) \
    : "memory")

#define _mLoad(addr) \
    asm volatile( \
        "ldrb w0, [%0]\n\t" \
        :: "r" (addr) \
        : "memory", "w0")

void mLoad(void* p){
    _mLoad(p);
}

void mStore(void* p, uint8_t val){
    _mStore(p, val);
}


static inline __attribute__((always_inline))
uint64_t timestamp(void)
{
    struct timespec t1;
    asm volatile("DSB SY" ::: "memory");
    asm volatile("ISB" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    asm volatile("ISB" ::: "memory");
    asm volatile("DSB SY" ::: "memory");
    return t1.tv_sec * 1000ULL * 1000ULL * 1000ULL + t1.tv_nsec;
}



uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

uint8_t array1[100*LINE_SIZE]={0};

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

static uint8_t* dummy_buffer;

void dummyAccesses(void){
    for(uint64_t i = 0; i < DUMMY_BUFFER_SIZE; i += LINE_SIZE){
        // mStore(dummy_buffer + i, random() & 0xFF);
        asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
    }
    mfence();
}

static void print_test_header(int stride, int train_step, uint64_t rounds) {
    printf("# arm64 store-stride prefetch latency map\n");
    printf("# access mode: store (strb)\n");
    printf("# stride_bytes=%d train_step=%d rounds=%llu probe_positions=%d\n",
           stride, train_step, (unsigned long long)rounds, PROBE_POSITIONS);
    printf("# timer: clock_gettime(CLOCK_MONOTONIC) ns\n");
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
}



// TIME STAMP END
uint64_t accessLatency(uint8_t* addr)
{
  uint64_t start, end;
  mfence();
  start = timestamp();
  mLoad(addr);
  end = timestamp();
  mfence();
  return end - start;
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
              for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);
              }
            //   mfence();
              dummyAccesses();//for dummy accesses , reset the prefetcher state

              // with stride prefetcher training
              for(int repeat = 0; repeat < 5; repeat ++) {

                for(int step = 0; step < train_step -1; step++){
                    mStore(array2 + (step * stride), 1);
                    // mLoad(array2 + (step * stride));
                    // Prefetch streams end when A DSB is executed.
                    // mfence();
                }
                mStore(array2 + ((train_step -1) * stride), 1);
                // mLoad(array2 + ((train_step -1) * stride));
                // mfence();
              }
              
              uint64_t dummy = 0;
              for(int k =0;k<100;k++){//wait for prefetch done.
                dummy += array1[k*64];
                // mfence();
              }
              for(int i=0;i<1000;i++) {
                nop();
              }
            //   mfence();

              int probe_pos = atkRound % PROBE_POSITIONS;//test one position each round
              
              probe_addr = array2 + (probe_pos * LINE_SIZE);

            //   mfence();
              time1 = timestamp();
              junk = *probe_addr;
            //   mfence();
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
