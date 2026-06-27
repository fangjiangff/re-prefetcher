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

#ifndef TRAIN_ACCESS_PREFETCH
#define TRAIN_ACCESS_PREFETCH 0
#endif

#if TRAIN_ACCESS_LOAD && TRAIN_ACCESS_PREFETCH
#error "Only one train access mode can be enabled"
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

#ifndef USER_MEMORY_PRESSURE_BEFORE_TRIGGER
#define USER_MEMORY_PRESSURE_BEFORE_TRIGGER 0
#endif

#ifndef USER_MEMORY_PRESSURE_ITERS
#define USER_MEMORY_PRESSURE_ITERS 256
#endif

#ifndef BUSY_WAIT_BEFORE_TRIGGER
#define BUSY_WAIT_BEFORE_TRIGGER 0
#endif

#ifndef BUSY_WAIT_NS
#define BUSY_WAIT_NS 10960
#endif

static volatile long user_memory_pressure_sink;
static uint64_t user_memory_pressure_time_sum_ns;
static uint64_t user_memory_pressure_time_min_ns = UINT64_MAX;
static uint64_t user_memory_pressure_time_max_ns;
static uint64_t user_memory_pressure_time_count;
static uint64_t busy_wait_time_sum_ns;
static uint64_t busy_wait_time_min_ns = UINT64_MAX;
static uint64_t busy_wait_time_max_ns;
static uint64_t busy_wait_time_count;

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

uint8_t array1[100*LINE_SIZE]={0};

uint8_t array3[Items * LINE_SIZE] __attribute__((aligned(4096)));;

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

static uint8_t* dummy_buffer;

void dummyAccesses(void){
//   dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
   for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
        uint64_t i = j;
        // asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
        // mLoad_noinline(&dummy_buffer[i]);
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

static void context_switch_before_trigger(void) {
#if CONTEXT_SWITCH_BEFORE_TRIGGER
    for (int i = 0; i < CONTEXT_SWITCH_YIELDS; i++) {
        sched_yield();
    }
#endif
}

static inline __attribute__((always_inline)) uint64_t read_cntvct(void) {
    uint64_t value;

    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static inline __attribute__((always_inline)) uint64_t read_cntfrq(void) {
    uint64_t value;

    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

static void user_memory_pressure_before_trigger(void) {
#if USER_MEMORY_PRESSURE_BEFORE_TRIGGER
    uint64_t start = timestamp();
    long sum = 0;

    for (int i = 0; i < USER_MEMORY_PRESSURE_ITERS; i++) {
        uint8_t *addr = array3 + ((i * 97) % Items) * LINE_SIZE;
        uint32_t value = (uint32_t)i;

        asm volatile("strb %w0, [%1]\n\t"
                     :
                     : "r"(value), "r"(addr)
                     : "memory");
        sum += value;
    }

    user_memory_pressure_sink += sum;

    uint64_t elapsed = timestamp() - start;

    user_memory_pressure_time_sum_ns += elapsed;
    if (elapsed < user_memory_pressure_time_min_ns) {
        user_memory_pressure_time_min_ns = elapsed;
    }
    if (elapsed > user_memory_pressure_time_max_ns) {
        user_memory_pressure_time_max_ns = elapsed;
    }
    user_memory_pressure_time_count++;
#endif
}

static void busy_wait_before_trigger(void) {
#if BUSY_WAIT_BEFORE_TRIGGER
    uint64_t start_ns = timestamp();
    uint64_t freq = read_cntfrq();
    uint64_t ticks = (freq * (uint64_t)BUSY_WAIT_NS + 999999999ULL) / 1000000000ULL;
    uint64_t end = read_cntvct() + (ticks ? ticks : 1);

    while (read_cntvct() < end) {
        nop();
    }

    uint64_t elapsed = timestamp() - start_ns;

    busy_wait_time_sum_ns += elapsed;
    if (elapsed < busy_wait_time_min_ns) {
        busy_wait_time_min_ns = elapsed;
    }
    if (elapsed > busy_wait_time_max_ns) {
        busy_wait_time_max_ns = elapsed;
    }
    busy_wait_time_count++;
#endif
}

static void print_busy_wait_time_stats(void) {
#if BUSY_WAIT_BEFORE_TRIGGER
    uint64_t avg = 0;

    if (busy_wait_time_count > 0) {
        avg = busy_wait_time_sum_ns / busy_wait_time_count;
    }

    printf("# busy_wait_time_ns count=%llu avg=%llu min=%llu max=%llu target=%d\n",
           (unsigned long long)busy_wait_time_count,
           (unsigned long long)avg,
           (unsigned long long)busy_wait_time_min_ns,
           (unsigned long long)busy_wait_time_max_ns,
           BUSY_WAIT_NS);
#endif
}

static void print_user_memory_pressure_time_stats(void) {
#if USER_MEMORY_PRESSURE_BEFORE_TRIGGER
    uint64_t avg = 0;

    if (user_memory_pressure_time_count > 0) {
        avg = user_memory_pressure_time_sum_ns / user_memory_pressure_time_count;
    }

    printf("# user_memory_pressure_time_ns count=%llu avg=%llu min=%llu max=%llu iters=%d\n",
           (unsigned long long)user_memory_pressure_time_count,
           (unsigned long long)avg,
           (unsigned long long)user_memory_pressure_time_min_ns,
           (unsigned long long)user_memory_pressure_time_max_ns,
           USER_MEMORY_PRESSURE_ITERS);
#endif
}

static void print_test_header(int stride, int train_step, uint64_t rounds) {
    printf("# arm64 %s-stride prefetch latency map\n",
#if TRAIN_ACCESS_PREFETCH
           "prefetch"
#elif TRAIN_ACCESS_LOAD
           "load"
#else
           "store"
#endif
    );
    printf("# access mode: %s (%s), same noinline PC for train and trigger\n",
#if TRAIN_ACCESS_PREFETCH
           "prefetch", "PRFM PLDL1KEEP"
#elif TRAIN_ACCESS_LOAD
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

              for(int step = 0; step < train_step-1; step++){
                  stride_access(array2 + (step * stride));
                //   mfence();
              }

            //   for(int k=0;k<100;k++){nop();}
            //   mfence();
              context_switch_before_trigger();
              
  
                //trigger
#if !NO_TRIGGER
                stride_access(array2 + ((train_step -1) * stride));
#endif

                // busy_wait_before_trigger();
              // }
              uint64_t dummy = 0;
              for(int k =0;k<100;k++){//wait for prefetch done.
                dummy += array1[k*64];
                // mfence();
              }
              for(int i=0;i<100;i++) {
                nop();
              }

            //   mfence();

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
          print_user_memory_pressure_time_stats();
          print_busy_wait_time_stats();
      // }
      printf("\n");
  // }

  (void)junk;
  return 0;
}
