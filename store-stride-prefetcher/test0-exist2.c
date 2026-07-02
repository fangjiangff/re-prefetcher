#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include "until.h"
// #include "victim.h"

#ifndef ARRAY2_PAGES
#define ARRAY2_PAGES 160
#endif

#define ARRAY2_SIZE (ARRAY2_PAGES * PAGE_SIZE)
#define Items (ARRAY2_SIZE / LINE_SIZE)

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

#ifndef PREFETCH_WAIT_ITERS
#define PREFETCH_WAIT_ITERS 100
#endif

static Mapping array2_mapping;
static uint8_t *array2;

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

uint8_t array1[100*LINE_SIZE]={0};

uint8_t array3[Items * LINE_SIZE] __attribute__((aligned(4096)));;

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t* dummy_buffer;


void dummyAccesses(void){
  dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
    //    for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
    //     // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
    //     asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
    //     // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
    //  }
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


  array2_mapping = allocate_mapping(ARRAY2_SIZE);
  array2 = array2_mapping.base_addr;
  memset(array2, -1, array2_mapping.size);
  random_activity(array2_mapping);
  flush_mapping(array2_mapping);

  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }

  for(int i=0; i< Items; i++){
    mLoad(&array2[i * LINE_SIZE]);
  }
  flush_mapping(array2_mapping);


  uint64_t rounds = ROUNDS;

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
    if ((uint64_t)(train_step - 1) * (uint64_t)stride >= ARRAY2_SIZE) {
      fprintf(stderr, "training range exceeds array2 size\n");
      return 1;
    }

    uint64_t probe_offset = train_step * (uint64_t)stride;

          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {

            // dummyAccesses();//for dummy accesses , reset the prefetcher state
            // random_activity(array2_mapping);
            flush_mapping(array2_mapping);


            for(int step = 0; step < train_step-1; step++){
                stride_access(array2 + (step * stride));
            }
            //trigger access
#if !NO_TRIGGER
            stride_access(array2 +  ((train_step -1) * stride));
#endif

            int probe_pos = (atkRound) % PROBE_POSITIONS;//test one position each round

            probe_addr = array2 + (probe_pos * LINE_SIZE);

              time1 = timestamp();
            //   junk = *probe_addr;
              mLoad_inline((void*)probe_addr);
              time2 = timestamp() - time1;

            //   latency_sum += time2;
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

  unmap_mapping(array2_mapping);
  munmap(dummy_buffer, DUMMY_BUFFER_SIZE);
  (void)junk;
  return 0;
}
