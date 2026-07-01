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

#ifndef DUMMY_BUFFER_PAGES
#define DUMMY_BUFFER_PAGES 10
#endif

#ifndef DUMMY_ACCESS_LOAD
#define DUMMY_ACCESS_LOAD 0
#endif

#ifndef DUMMY_ACCESS_STORE
#define DUMMY_ACCESS_STORE 0
#endif

#ifndef DUMMY_ACCESS_PERMUTED
#define DUMMY_ACCESS_PERMUTED 0
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
#define BUSY_WAIT_NS 2000
#endif

#ifndef PREFETCH_WAIT_ITERS
#define PREFETCH_WAIT_ITERS 100
#endif

#define MIX_LOAD_NONE 0
#define MIX_LOAD_ARRAY3 1
#define MIX_LOAD_ARRAY2 2

#ifndef MIX_LOAD_TARGET
#define MIX_LOAD_TARGET MIX_LOAD_NONE
#endif

#ifndef MIX_LOAD_NUM
#define MIX_LOAD_NUM 0
#endif

#if MIX_LOAD_TARGET < MIX_LOAD_NONE || MIX_LOAD_TARGET > MIX_LOAD_ARRAY2
#error "Unknown MIX_LOAD_TARGET"
#endif

static volatile long user_memory_pressure_sink;
static uint64_t user_memory_pressure_time_sum_ns;
static uint64_t user_memory_pressure_time_min_ns = UINT64_MAX;
static uint64_t user_memory_pressure_time_max_ns;
static uint64_t user_memory_pressure_time_count;

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

uint8_t array1[100*LINE_SIZE]={0};

uint8_t array3[Items * LINE_SIZE] __attribute__((aligned(4096)));;

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t* dummy_buffer;

// void dummyAccesses(void){
//    size_t lines = DUMMY_BUFFER_SIZE / LINE_SIZE;

//    for(size_t n = 0; n < lines; n++){
// #if DUMMY_ACCESS_PERMUTED
//         size_t line = (n * 97) % lines;
// #else
//         size_t line = n;
// #endif
//         void *addr = dummy_buffer + line * LINE_SIZE;

// #if DUMMY_ACCESS_LOAD
//         mLoad_inline(addr);
// #elif DUMMY_ACCESS_STORE
//         mStore_inline(addr);
// #else
//         mPrefetch_inline(addr);
// #endif
//    }
// }

void dummyAccesses(void){
  dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
    //    for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
    //     // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
    //     asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
    //     // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
    //  }
}

// void dummyAccesses(){
//     uint64_t tmp = 0;
//      for(uint64_t i = 0; i < DUMMY_BUFFER_SIZE; i += 64){
//         tmp += dummy_buffer[i];
//      }
//     (void)tmp;
// }

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

#ifdef __aarch64__
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
#endif

static void user_memory_pressure_before_trigger(void) {
#if USER_MEMORY_PRESSURE_BEFORE_TRIGGER
    uint64_t start = timestamp();
    long sum = 0;

    for (int i = 0; i < USER_MEMORY_PRESSURE_ITERS; i++) {
        uint8_t *addr = array3 + ((i * 97) % Items) * LINE_SIZE;
        uint32_t value = (uint32_t)i;

#ifdef __aarch64__
        asm volatile("strb %w0, [%1]\n\t"
                     :
                     : "r"(value), "r"(addr)
                     : "memory");
#else
        mStore_inline(addr);
#endif
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
#ifndef __aarch64__
#error "BUSY_WAIT_BEFORE_TRIGGER currently supports aarch64 only"
#endif
    asm volatile(
        "mrs x9, cntfrq_el0\n\t"
        "mul x9, x9, %[target_ns]\n\t"
        "add x9, x9, %[ceil_ns]\n\t"
        "udiv x9, x9, %[ns_per_sec]\n\t"
        "cmp x9, #0\n\t"
        "cinc x9, x9, eq\n\t"
        "mrs x10, cntvct_el0\n\t"
        "add x9, x9, x10\n"
        "1:\n\t"
        "mrs x10, cntvct_el0\n\t"
        "cmp x10, x9\n\t"
        "b.hs 2f\n\t"
        "nop\n\t"
        "b 1b\n"
        "2:\n\t"
        :
        : [target_ns] "r"((uint64_t)BUSY_WAIT_NS),
          [ceil_ns] "r"(999999999ULL),
          [ns_per_sec] "r"(1000000000ULL)
        : "x9", "x10", "cc");
#endif
}

static void print_busy_wait_time_stats(void) {
#if BUSY_WAIT_BEFORE_TRIGGER
    printf("# busy_wait target_ns=%d source=cntvct_el0\n", BUSY_WAIT_NS);
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

static void mix_load_before_trigger(void) {
#if MIX_LOAD_NUM > 0 && MIX_LOAD_TARGET != MIX_LOAD_NONE
    for (int r = 0; r < MIX_LOAD_NUM; r++) {
        int mix_r = (int)(random() % 64);
#if MIX_LOAD_TARGET == MIX_LOAD_ARRAY3
        mLoad_noinline(array3 + (mix_r * LINE_SIZE));
#elif MIX_LOAD_TARGET == MIX_LOAD_ARRAY2
        mLoad_noinline(array2 + (mix_r * LINE_SIZE));
#endif
    }
#endif
}

static void print_mix_load_config(void) {
#if MIX_LOAD_TARGET == MIX_LOAD_ARRAY3
    printf("# mix_load target=array3 count=%d range_lines=64\n", MIX_LOAD_NUM);
#elif MIX_LOAD_TARGET == MIX_LOAD_ARRAY2
    printf("# mix_load target=array2 count=%d range_lines=64\n", MIX_LOAD_NUM);
#else
    printf("# mix_load target=none count=0 range_lines=0\n");
#endif
}

static void print_test_header(int stride, int train_step, uint64_t rounds) {
    printf("# %s %s-stride prefetch latency map\n",
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
    printf("# access mode: %s (%s), same noinline PC for train and trigger\n",
#if TRAIN_ACCESS_PREFETCH
           "prefetch",
#ifdef __x86_64__
           "prefetcht0"
#else
           "PRFM PLDL1KEEP"
#endif
#elif TRAIN_ACCESS_LOAD
           "load",
#ifdef __x86_64__
           "movb load"
#else
           "ldrb"
#endif
#else
           "store",
#ifdef __x86_64__
           "movb store"
#else
           "strb"
#endif
#endif
    );
    printf("# stride_bytes=%d train_step=%d rounds=%llu probe_positions=%d\n",
           stride, train_step, (unsigned long long)rounds, PROBE_POSITIONS);
    printf("# timer: %s %s\n", TIMESTAMP_SOURCE_NAME, TIMESTAMP_UNIT_NAME);
    printf("# position\toffset_bytes\tavg_%s\tprobes\n", TIMESTAMP_UNIT_NAME);
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
    mLoad(&array2[i * LINE_SIZE]);
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
    // print_test_header(stride, train_step, rounds);
      // for(int train_step = 1; train_step <= 32 ; train_step++){
          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {

            dummyAccesses();//for dummy accesses , reset the prefetcher state

            for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);
              }

            // mfence();

            // train access
            for(int step = 0; step < train_step-2; step++){
                stride_access(array2 + (step * stride));
            }

            // int dummy2 = array3[24*LINE_SIZE];
            // mLoad_noinline(array2 + 61 * LINE_SIZE);
            // mLoad_noinline(array2 + 37 * LINE_SIZE);
            // mLoad_noinline(array2 + 111 * LINE_SIZE);
            // context_switch_before_trigger();
                  
            //trigger access
#if !NO_TRIGGER
            stride_access(array2 + ((train_step -2) * stride));
            stride_access(array2 + ((train_step -1) * stride));
            // mLoad_noinline(array2 + ((train_step -1) * stride));
#endif
            // mStore_noinline(array2 + 37 * LINE_SIZE);

// #if BUSY_WAIT_BEFORE_TRIGGER
//             busy_wait_before_trigger();
// #endif
            // mfence();
              // }

// #ifdef __aarch64__
//               asm volatile(
//                 "mov x11, %[iters]\n\t"
//                 "mov x12, #0\n"
//                 "1:\n\t"
//                 "add x12, x12, #1\n\t"
//                 "eor x12, x12, x12, ror #7\n\t"
//                 "subs x11, x11, #1\n\t"
//                 "b.ne 1b\n\t"
//                 :
//                 : [iters] "I"(PREFETCH_WAIT_ITERS)
//                 : "x11", "x12", "cc");
// #else
//               for(int i=0;i<PREFETCH_WAIT_ITERS;i++) {
//                 nop();
//               }
// #endif
             

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
          print_mix_load_config();
      // }
      printf("\n");
  // }

  (void)junk;
  return 0;
}
