#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include "cacheutils.hh"
#include "utils.hh"
#include <random>
#include <sys/mman.h>
#include <unistd.h>
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
#define TEST_ON_SW 0
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

#define REG_ARG_1 "x0"

#define mfence() asm volatile("DMB SY\nISB")

#define flush(addr) asm volatile("DC CIVAC, %0" :: "r" (addr))

#define return_asm() "ret"

#define nop() asm volatile("nop")

#define VIRTUAL_ADDRESS_BITS 48

typedef void (*load_gadget_f)(void *);

extern char _dummy_load_gadget_asm_start[];
extern char _dummy_load_gadget_asm_end[];

asm(
    ".pushsection .text\n"
    ".global _dummy_load_gadget_asm_start\n"
    ".global _dummy_load_gadget_asm_end\n"
    "_dummy_load_gadget_asm_start:\n"
    "    hint #34\n"
    "    ldrb w0, [x0]\n"
    "    ret\n"
    "_dummy_load_gadget_asm_end:\n"
    "    nop\n"
    ".popsection\n"
);

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


#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 1024)
#define DUMMY_SEQUENTIAL_BUFFER_SIZE (PAGE_SIZE * 10)
#define DUMMY_RANDOM_ACCESSES 4096
#define DUMMY_LOAD_PC_COUNT 128
#define DUMMY_PC_SPACING 0x20ull
#define DEFAULT_DUMMY_LOAD_PC 0x500100120ull

static uint8_t* dummy_buffer;
static load_gadget_f dummy_loads[DUMMY_LOAD_PC_COUNT];
static uint64_t dummy_rng_state = 0x9e3779b97f4a7c15ull;
static size_t os_page_size;

static void sigill_handler(int, siginfo_t *, void *context) {
    ucontext_t *uc = (ucontext_t *)context;
    fprintf(stderr, "SIGILL at pc=0x%016llx\n",
            (unsigned long long)uc->uc_mcontext.pc);
    _exit(132);
}

static void install_sigill_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigill_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
}

static uintptr_t page_base(uintptr_t address) {
    return address - (address % os_page_size);
}

static uint64_t next_dummy_random(void) {
    uint64_t x = dummy_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    dummy_rng_state = x;
    return x;
}

static void init_dummy_loads(void) {
    uintptr_t base_pc = DEFAULT_DUMMY_LOAD_PC;
    uintptr_t mapping_base = page_base(base_pc);
    size_t gadget_size =
        (size_t)(_dummy_load_gadget_asm_end - _dummy_load_gadget_asm_start);
    size_t map_size = os_page_size;

    void *mapping = mmap((void *)mapping_base, map_size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE,
                         -1, 0);
    if (mapping == MAP_FAILED) {
        printf("failed to map dummy load gadgets at 0x%lx: %s\n",
               (unsigned long)mapping_base, strerror(errno));
        exit(1);
    }

    for (int i = 0; i < DUMMY_LOAD_PC_COUNT; i++) {
        uintptr_t pc = base_pc + ((uintptr_t)i * DUMMY_PC_SPACING);
        if ((pc - mapping_base) + gadget_size > map_size) {
            printf("dummy load gadget crosses mapped page\n");
            exit(1);
        }
        memcpy((void *)pc, _dummy_load_gadget_asm_start, gadget_size);
        __builtin___clear_cache((char *)pc, (char *)(pc + gadget_size));
        dummy_loads[i] = (load_gadget_f)(void *)pc;
    }
}

void dummyAccesses(){
    uint64_t line_count = DUMMY_BUFFER_SIZE / LINE_SIZE;
    for (int i = 0; i < DUMMY_RANDOM_ACCESSES; i++) {
        uint64_t line = next_dummy_random() % line_count;
        load_gadget_f dummy_load = dummy_loads[i % DUMMY_LOAD_PC_COUNT];
        dummy_load(dummy_buffer + (line * LINE_SIZE));
    }
    mfence();
}

static inline void dummyAccesses2() {
    // printf("dummyAccesses2\n");
    for (uint64_t i = 0; i < DUMMY_SEQUENTIAL_BUFFER_SIZE; i += 64) {
        asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
        // maccess(&dummy_buffer[i]);
    }
}

int main(){
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;
  install_sigill_handler();
  long detected_page_size = sysconf(_SC_PAGESIZE);
  if (detected_page_size <= 0) {
      printf("failed to detect OS page size\n");
      exit(1);
  }
  os_page_size = (size_t)detected_page_size;

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
  init_dummy_loads();

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
    int stride = 17 * 64;
    for(int train_step = 1; train_step <= 20 ; train_step++){
      printf("Stride %d*64:\t%d\ttrian_step %d\t",stride/64,stride, train_step);
      uint64_t latency = 0;
      int res[1000]={0};
          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
                dummyAccesses2();
                // dummyAccesses();
                
                for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);
                }
                for(int step = 0; step < train_step-1; step++){
                    // sw prefetch
                    // mprefetch(array2 + (step * stride));
                    maccess(array2 + (step * stride));
                    mfence();
                }
                //flush the cache to make sure the target probed line is not in cache before the prefetch
                for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                    flush(&array2[offset]);
                }
              //trigger 
            //   mprefetch(array2 + ((train_step - 1) * stride));
                maccess(array2 + ((train_step - 1) * stride));
              
              uint64_t dummy = 0;
              for(int k =0;k<100;k++){
                dummy += array1[k*64];
                mfence();
              }

              for(int i=0;i<100;i++) nop(); mfence();
              // mfence();
              time1 = timestamp1();
              maccess(array2 + ((train_step+15) * stride));
              /* READ TIMER */
              time2 = timestamp1();
              latency += (time2-time1);
          } 
          printf("%lld\n", latency/rounds);
      }
      // printf("\n");
  // }
  return 0;
}
