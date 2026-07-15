#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../until.h"
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


#define ARRAY2_SIZE (Items * LINE_SIZE * sizeof(uint8_t))

static uint8_t *array2;

long long int latency_sum[PROBE_POSITIONS] = {0};
int probe_count[PROBE_POSITIONS] = {0};

uint8_t array1[100*LINE_SIZE]={0};

uint8_t array3[Items * LINE_SIZE] __attribute__((aligned(4096)));;

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * DUMMY_BUFFER_PAGES)

static uint8_t* dummy_buffer;

static int parent_to_child[2] = {-1, -1};
static int child_to_parent[2] = {-1, -1};
static pid_t trigger_child_pid = -1;

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t ret = write(fd, p, len);
        if (ret < 0) {
            die("write pipe");
        }
        p += ret;
        len -= (size_t)ret;
    }
}

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t ret = read(fd, p, len);
        if (ret == 0) {
            return 0;
        }
        if (ret < 0) {
            die("read pipe");
        }
        p += ret;
        len -= (size_t)ret;
    }
    return 1;
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

// static void context_switch_before_trigger(void) {
// #if CONTEXT_SWITCH_BEFORE_TRIGGER
//     for (int i = 0; i < CONTEXT_SWITCH_YIELDS; i++) {
//         sched_yield();
//     }
// #endif
// }




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
    printf("# access mode: %s (%s), cross-process capable trigger\n",
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

// static void delay_after_trigger(void) {
//     uint64_t dummy = 0;

//     for (int k = 0; k < 100; k++) {
//         dummy += array1[k * LINE_SIZE];
//     }
//     for (int i = 0; i < 100; i++) {
//         nop();
//     }

//     (void)dummy;
// }

static inline __attribute__((always_inline)) void trigger_access(int train_step, int stride) {
    mStore_inline(array2 + ((train_step - 1) * stride));
    // mStore_inline(array2 + ((train_step) * stride));
    nops();
}

static void child_trigger_loop(int train_step, int stride) {
    close(parent_to_child[1]);
    close(child_to_parent[0]);

    for (;;) {
        uint8_t command;
        if (!read_full(parent_to_child[0], &command, sizeof(command))) {
            break;
        }
        if (command == 0) {
            break;
        }

        // trigger_access(train_step, stride);

        uint8_t done = 1;
        write_full(child_to_parent[1], &done, sizeof(done));
    }

    _exit(0);
}

static void start_trigger_child(int train_step, int stride) {
    if (pipe(parent_to_child) != 0) {
        die("pipe parent_to_child");
    }
    if (pipe(child_to_parent) != 0) {
        die("pipe child_to_parent");
    }

    trigger_child_pid = fork();
    if (trigger_child_pid < 0) {
        die("fork trigger child");
    }
    if (trigger_child_pid == 0) {
        child_trigger_loop(train_step, stride);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);
}

static void request_child_trigger(void) {
    uint8_t command = 1;
    uint8_t done = 0;

    write_full(parent_to_child[1], &command, sizeof(command));
    if (!read_full(child_to_parent[0], &done, sizeof(done)) || done != 1) {
        fprintf(stderr, "trigger child did not acknowledge trigger\n");
        exit(1);
    }
}

static void stop_trigger_child(void) {
    int status = 0;
    uint8_t command = 0;

    if (trigger_child_pid <= 0) {
        return;
    }
    write_full(parent_to_child[1], &command, sizeof(command));
    close(parent_to_child[1]);
    close(child_to_parent[0]);
    waitpid(trigger_child_pid, &status, 0);
}

int main(int argc, char **argv){
  register uint64_t time1, time2;
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;
  int child_trigger = !(argc > 1 && strcmp(argv[1], "--parent-trigger") == 0);


  array2 = (uint8_t*)mmap(NULL, ARRAY2_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (array2 == MAP_FAILED) {
      perror("mmap array2");
      return 1;
  }

  memset(array2,-1,ARRAY2_SIZE);
  if (mlock(array2, ARRAY2_SIZE) != 0) {
      perror("mlock array2");
  }


//   dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
//   if(dummy_buffer == MAP_FAILED) {
//       printf("failed to map memory to access!\n");
//       exit(1);
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

    if (child_trigger) {
        start_trigger_child(train_step, stride);
    }

          for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
            cpp_rctx();

            for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
                  flush(&array2[offset]);//flush 0-256. probe 0-64
            }
            for(int step = 0; step < train_step-1; step++){
                mStore_inline(array2 + (step * stride));
                nops();
            }
            // context_switch_before_trigger();
#if !NO_TRIGGER
            // if (child_trigger) {
                request_child_trigger();
            // } else {
                // trigger_access(train_step, stride);
                mStore_inline(array2 + ((train_step-1)) * stride);
                nops();
                mStore_inline(array2 + ((train_step)) * stride);
            // }
#endif
        // }
            // mStore_inline(array2 +  (0 * stride));
            // nops();
            // mStore_inline(array2 +  (1 * stride));
            // nops();
            // mStore_inline(array2 +  (2 * stride));
            // nops();
            // mStore_inline(array2 +  (3 * stride));
            // nops();
            // mStore_inline(array2 +  (4 * stride));

            int probe_pos = (atkRound*13) % PROBE_POSITIONS;//test one position each round
            probe_addr = array2 + (probe_pos * LINE_SIZE);
            time1 = timestamp();
            mStore_inline((void*)probe_addr);
            time2 = timestamp() - time1;

            latency_sum[probe_pos] += time2;
            probe_count[probe_pos]++;
            // printf("%llu\n", (unsigned long long)time2);
          }
          if (child_trigger) {
              stop_trigger_child();
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
