#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <string.h>
#include <random>
#include <sys/mman.h>
#include <time.h>

#include "cacheutils.hh"
#include "utils.hh"

#define CPU_ID 0
#define LINE_SIZE 64
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

/*
 * 在编译时指定软件预取指令，例如：
 *
 *   -DPRFM_MODE=PLDL1KEEP
 *   -DPRFM_MODE=PSTL1KEEP
 *   -DPRFM_MODE=PLDL2KEEP
 *   -DPRFM_MODE=PSTL2KEEP
 *   -DPRFM_MODE=PLDL3KEEP
 *   -DPRFM_MODE=PSTL3KEEP
 *   -DPRFM_MODE=PLDL1STRM
 *   -DPRFM_MODE=PSTL1STRM
 *   -DPRFM_MODE=PLDL2STRM
 *   -DPRFM_MODE=PSTL2STRM
 *   -DPRFM_MODE=PLDL3STRM
 *   -DPRFM_MODE=PSTL3STRM
 *
 * 如果编译时不指定，则默认使用 PLDL1KEEP
 */
#ifndef PRFM_MODE
#define PRFM_MODE PLDL1KEEP
#endif

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/*
 * AArch64 上的 1-byte memory access 原语
 *
 * TEST_ON_ST == 1:
 *   执行 1-byte store
 *
 * TEST_ON_ST == 0:
 *   执行 1-byte load
 */
#if TEST_ON_ST == 1
    #define _maccess(addr)                                      \
        do {                                                    \
            unsigned int value = 0;                             \
            asm volatile(                                       \
                "strb %w0, [%1]\n\t"                            \
                :                                               \
                : "r"(value), "r"(addr)                         \
                : "memory");                                    \
        } while (0)
#else
    #define _maccess(addr)                                      \
        do {                                                    \
            unsigned int value;                                 \
            asm volatile(                                       \
                "ldrb %w0, [%1]\n\t"                            \
                : "=r"(value)                                   \
                : "r"(addr)                                     \
                : "memory");                                    \
        } while (0)
#endif

#define REG_ARG_1 "x0"

#define mfence() asm volatile("DMB SY\nISB" ::: "memory")

/*
 * 这里保留你原来的 flush 宏
 * 如果目标平台/权限环境不支持，可能需要改。
 */
#define flush(addr) asm volatile("DC CIVAC, %0" :: "r" (addr) : "memory")

#define return_asm() "ret"

#define nop() asm volatile("nop")

#define VIRTUAL_ADDRESS_BITS 48
#define PAGE_SIZE 4096
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

/*
 * 普通内存访问原语
 */
static inline void maccess(void *p) {
    _maccess(p);
}

/*
 * 软件预取原语
 * 通过编译参数 -DPRFM_MODE=... 指定具体 PRFM op
 */
static inline void mprefetch(void *p) {
    asm volatile(
        "PRFM " STR(PRFM_MODE) ", [%0]\n\t"
        :
        : "r"(p)
        : "memory"
    );
}

/*
 * 打时间戳
 * 这里沿用你原来的 clock_gettime 方案
 */
static inline __attribute__((always_inline))
uint64_t timestamp(void) {
    asm volatile("DSB SY" ::: "memory");
    asm volatile("ISB" ::: "memory");

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t res = t1.tv_sec * 1000ULL * 1000ULL * 1000ULL + t1.tv_nsec;

    asm volatile("ISB" ::: "memory");
    asm volatile("DSB SY" ::: "memory");
    return res;
}

uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));
long long int res2[100][100] = {0};

static uint8_t *dummy_buffer;

/*
 * 用 dummy accesses 尝试扰动/重置预取器状态
 */
static inline void dummyAccesses() {
    for (uint64_t i = 0; i < DUMMY_BUFFER_SIZE; i += 64) {
        maccess(&dummy_buffer[i]);
    }
}

int main() {
    register uint64_t time1, time2;
    volatile uint8_t *probe_addr;
    unsigned int junk = 0;

    struct timespec const t_req{ .tv_sec = 0, .tv_nsec = 1500 };
    struct timespec t_rem;

    memset(array2, -1, Items * LINE_SIZE * sizeof(uint8_t));

    dummy_buffer = (uint8_t *)mmap(
        NULL,
        DUMMY_BUFFER_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
        0,
        0
    );

    if (dummy_buffer == MAP_FAILED) {
        printf("failed to map memory to access!\n");
        exit(1);
    }

    printf("TEST_ON_HIT=%d, TEST_ON_SW=%d, TEST_ON_ST=%d, PRFM_MODE=%s\n",
           TEST_ON_HIT, TEST_ON_SW, TEST_ON_ST, STR(PRFM_MODE));

    for (int i = 0; i < Items; i++) {
        maccess(&array2[i * 64]);
    }

    if (!TEST_ON_HIT) {
        for (uint64_t offset = 0; offset < Items * LINE_SIZE; offset += LINE_SIZE) {
            flush(&array2[offset]);
        }
    }

    int rounds = 1000;

    for (int stride = 64; stride <= 4096; stride += 64) {
        for (int train_step = 1; train_step <= 20; train_step++) {
            for (uint64_t atkRound = 0; atkRound < (uint64_t)rounds; ++atkRound) {
                dummyAccesses();

                if (TEST_ON_HIT) {
                    for (int step = 0; step < train_step; step++) {
                        array2[step * stride] = 1;
                    }
                    flush(&array2[train_step * stride]);
                    mfence();
                } else {
                    for (uint64_t offset = 0; offset < Items * LINE_SIZE; offset += LINE_SIZE) {
                        flush(&array2[offset]);
                    }
                }

                /*
                 * stride prefetcher training
                 */
                for (int repeat = 0; repeat < 5; repeat++) {
                    for (int step = 0; step < train_step - 1; step++) {
                        if (TEST_ON_SW) {
                            mprefetch(array2 + (step * stride));
                            mfence();
                        } else {
                            maccess(array2 + (step * stride));
                            mfence();
                        }
                    }

                    if (TEST_ON_SW) {
                        mprefetch(array2 + ((train_step - 1) * stride));
                        mfence();
                    } else {
                        maccess(array2 + ((train_step - 1) * stride));
                        mfence();
                    }
                }

                for (int i = 0; i < 100; i++) {
                    nop();
                }
                mfence();

                probe_addr = array2 + (train_step * stride);

                time1 = timestamp();
                junk = *probe_addr;
                time2 = timestamp() - time1;

                res2[stride / 64][train_step] += time2;
            }

            printf("%lld\t", res2[stride / 64][train_step] / rounds);
        }
        printf("\n");
    }

    /*
     * 防止编译器把 junk 优化得太激进
     */
    if (junk == 0xFFFFFFFF) {
        printf("junk=%u\n", junk);
    }

    munmap(dummy_buffer, DUMMY_BUFFER_SIZE);
    return 0;
}