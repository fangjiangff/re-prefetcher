#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
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
#define DUMMY_BUFFER_PAGES 0
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

#ifndef TRAIN_STORE_PC0
#define TRAIN_STORE_PC0 0x783709b0120ull
#endif

#ifndef TRAIN_STORE_PC1
#define TRAIN_STORE_PC1 0x2d650271c2a4ull
#endif

#ifndef TRAIN_STORE_PC2
#define TRAIN_STORE_PC2 0x646f3e8ac548ull
#endif

#ifndef TRAIN_STORE_PC3
#define TRAIN_STORE_PC3 0x3a74fdac8a90ull
#endif

#define TRAIN_STORE_GADGETS 4
#define MAX_MAPPED_GADGET_PAGES 16

typedef void (*store_gadget_f)(void *);

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

uint8_t array1[100*LINE_SIZE]={0};

uint8_t array3[Items * LINE_SIZE] __attribute__((aligned(4096)));;

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t* dummy_buffer;

static size_t page_size;
static uintptr_t mapped_gadget_pages[MAX_MAPPED_GADGET_PAGES];
static int mapped_gadget_page_count;
static store_gadget_f train_stores[TRAIN_STORE_GADGETS];

extern char _store_gadget_asm_start[];
extern char _store_gadget_asm_end[];

asm(
    ".pushsection .text, \"ax\"\n"
    ".balign 4\n"
    ".global _store_gadget_asm_start\n"
    ".global _store_gadget_asm_end\n"
    "_store_gadget_asm_start:\n"
    "    strb w0, [x0]\n"
    "    ret\n"
    "_store_gadget_asm_end:\n"
    "    nop\n"
    ".popsection\n"
);

static uintptr_t page_base(uintptr_t address) {
    return address - (address % page_size);
}

static int gadget_page_is_mapped(uintptr_t page) {
    for (int i = 0; i < mapped_gadget_page_count; i++) {
        if (mapped_gadget_pages[i] == page) {
            return 1;
        }
    }

    return 0;
}

static int ensure_gadget_page(uintptr_t page) {
    if (gadget_page_is_mapped(page)) {
        return 0;
    }

    if (mapped_gadget_page_count >= MAX_MAPPED_GADGET_PAGES) {
        fprintf(stderr, "too many mapped gadget pages\n");
        return -1;
    }

    void *mapping = mmap((void *)page, page_size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_FIXED_NOREPLACE | MAP_ANONYMOUS |
                         MAP_PRIVATE | MAP_POPULATE,
                         -1, 0);

    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap gadget page 0x%016lx failed: %s\n",
                (unsigned long)page, strerror(errno));
        return -1;
    }

    if ((uintptr_t)mapping != page) {
        fprintf(stderr,
                "mmap returned wrong page: expected 0x%016lx got %p\n",
                (unsigned long)page, mapping);
        return -1;
    }

    mapped_gadget_pages[mapped_gadget_page_count++] = page;
    return 0;
}

static store_gadget_f map_store_gadget(uintptr_t store_pc) {
    uintptr_t page = page_base(store_pc);
    size_t page_offset = store_pc - page;
    size_t gadget_size =
        (size_t)(_store_gadget_asm_end - _store_gadget_asm_start);

    if (page_offset + gadget_size > page_size) {
        fprintf(stderr, "store gadget at 0x%016lx crosses a page boundary\n",
                (unsigned long)store_pc);
        return NULL;
    }

    if (ensure_gadget_page(page) != 0) {
        return NULL;
    }

    memcpy((void *)store_pc, _store_gadget_asm_start, gadget_size);
    __builtin___clear_cache((char *)store_pc,
                            (char *)(store_pc + gadget_size));

    return (store_gadget_f)(void *)store_pc;
}

static int init_train_store_gadgets(void) {
    static const uintptr_t pcs[TRAIN_STORE_GADGETS] = {
        TRAIN_STORE_PC0,
        TRAIN_STORE_PC1,
        TRAIN_STORE_PC2,
        TRAIN_STORE_PC3,
    };

    for (int i = 0; i < TRAIN_STORE_GADGETS; i++) {
        train_stores[i] = map_store_gadget(pcs[i]);
        if (!train_stores[i]) {
            return -1;
        }
    }

    printf("# train_store_pc0=0x%016lx\n", (unsigned long)pcs[0]);
    printf("# train_store_pc1=0x%016lx\n", (unsigned long)pcs[1]);
    printf("# train_store_pc2=0x%016lx\n", (unsigned long)pcs[2]);
    printf("# train_store_pc3=0x%016lx\n", (unsigned long)pcs[3]);

    return 0;
}


void dummyAccesses(void){
    // printf("dummySize %d\n", DUMMY_BUFFER_SIZE);
  // dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
    for(uint32_t j = 0; j < DUMMY_BUFFER_SIZE; j+=64){
        // asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(&dummy_buffer[i]));
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(&dummy_buffer[j]));
        // asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[j]) : "memory", "w0");
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


// static inline void cpp_rctx(void)
// {
// #ifdef __aarch64__
//     asm volatile(
//         "cpp rctx, xzr\n"
//         ::: "memory");
// #endif
// }


int main(){
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;


  memset(array2,-1,Items*LINE_SIZE*sizeof(uint8_t));

  long detected_page_size = sysconf(_SC_PAGESIZE);
  if (detected_page_size <= 0) {
      fprintf(stderr, "failed to detect OS page size\n");
      return 1;
  }
  page_size = (size_t)detected_page_size;

  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }

  if (init_train_store_gadgets() != 0) {
      return 1;
  }

//   for(int i=0; i< Items; i++){
//     mLoad(&array2[i * LINE_SIZE]);
//     // mLoad_inline(&array2[i * LINE_SIZE]);
//   }
//   for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
//     flush(&array2[offset]);
//   }

  // mfence();


  uint64_t rounds = ROUNDS;

    int stride = STRIDE_BYTES;
    int train_step = TRAIN_STEP;
    if ((uint64_t)(train_step - 1) * (uint64_t)stride >= Items * LINE_SIZE) {
      fprintf(stderr, "training range exceeds array2 size\n");
      return 1;
    }
    uint64_t probe_offset = train_step * (uint64_t)stride;
    int latency_sum2 = 0;


          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
            // cpp_rctx();
            context_switch_before_trigger();
            // dummyAccesses();  
            // mfence();
            for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);//flush 0-256. probe 0-64
            }
            // mfence();

            // for(int step = 0; step < train_step-1; step++){
            //     mStore_noinline(array2 + (step * stride));
            // }
            train_stores[0](array2 + 0 * stride);
            for(int i=0;i<1000;i++){nop();}
            train_stores[1](array2 + 1 * stride);
            for(int i=0;i<1000;i++){nop();}
            train_stores[2](array2 + 2 * stride);
            for(int i=0;i<1000;i++){nop();}
            train_stores[3](array2 + 3 * stride);
            for(int i=0;i<1000;i++){nop();}
            // mfence();
            
            // mLoad_inline(array2 + 78*LINE_SIZE);//trigger access
            // stride_access(array3 + 1*LINE_SIZE);
            // context_switch_before_trigger();
            // cpp_rctx();
            // mLoad_inline(array2 +  61*LINE_SIZE);//trigger access
// #if !NO_TRIGGER
//             mStore_inline(array2 +  ((train_step -1) * stride));
//             // stride_access(array2 +  ((train_step -1) * stride));
// #endif
            // stride_access(array2 +  ((train_step) * stride));
            // stride_access(array2 +  ((train_step+1) * stride));
            // stride_access(array2 +  ((train_step+2) * stride));

            int probe_pos = (atkRound) % PROBE_POSITIONS;//test one position each round
            probe_addr = array2 + (probe_pos * LINE_SIZE);
            // mLoad_inline((void*)array2 + 5*LINE_SIZE);//probe 0

              // probe_addr = array2 + probe_offset;

            time1 = timestamp();
            // junk = *probe_addr;
            // mLoad_inline((void*)probe_addr);
            mStore_inline((void*)probe_addr);
            time2 = timestamp() - time1;

              // latency_sum2 += time2;
            latency_sum[probe_pos] += time2;
            probe_count[probe_pos]++;
              // printf("%llu\n", (unsigned long long)time2);
          }
          // printf("avg latency: %llu\n", (unsigned long long)(latency_sum2 / rounds));
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
