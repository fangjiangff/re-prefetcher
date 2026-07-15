#ifndef UNTIL_H
#define UNTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef LINE_SIZE
#define LINE_SIZE 64
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

static inline __attribute__((always_inline)) void nop(void) {
    asm volatile("nop");
}

#ifdef __x86_64__
#if defined(RDTSC) && defined(GETTIME)
#error "Select only one x86 timestamp source: RDTSC or GETTIME"
#endif

#if !defined(RDTSC) && !defined(GETTIME)
#define GETTIME 1
#endif

#ifndef USE_RDTSCP
#define USE_RDTSCP 1
#endif

#if defined(RDTSC)
#define TIMESTAMP_SOURCE_NAME "rdtscp"
#define TIMESTAMP_UNIT_NAME "cycles"
#else
#define TIMESTAMP_SOURCE_NAME "clock_gettime(CLOCK_MONOTONIC)"
#define TIMESTAMP_UNIT_NAME "ns"
#endif

static inline __attribute__((always_inline)) void mfence(void) {
    asm volatile("mfence" ::: "memory");
}

static inline __attribute__((always_inline)) void flush(void *addr) {
    asm volatile("clflush (%0)" :: "r"(addr) : "memory");
}

#define _mStore(pre, addr)                      \
    asm volatile(pre "movb $1, (%0)\n\t"        \
                 :: "r"(addr)                   \
                 : "memory")

#define _mLoad(pre, addr)                       \
    asm volatile(pre "movb (%0), %%al\n\t"      \
                 :: "r"(addr)                   \
                 : "memory", "rax")

#define _mPrefetch(pre, addr)                   \
    asm volatile(pre "prefetcht0 (%0)\n\t"      \
                 :: "r"(addr)                   \
                 : "memory")

static inline __attribute__((always_inline)) uint64_t timestamp(void) {
#if defined(RDTSC)
    uint32_t lo;
    uint32_t hi;

    mfence();
#if USE_RDTSCP
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
#else
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
#endif
    mfence();

    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec t;

    mfence();
    clock_gettime(CLOCK_MONOTONIC, &t);
    mfence();

    return t.tv_sec * 1000ULL * 1000ULL * 1000ULL + t.tv_nsec;
#endif
}
#elif defined(__aarch64__)
#if (defined(CNTVCT) + defined(GETTIME) + defined(PMCCNTR)) > 1
#error "Select only one aarch64 timestamp source: CNTVCT, GETTIME, or PMCCNTR"
#endif

#if !defined(CNTVCT) && !defined(GETTIME) && !defined(PMCCNTR)
#define CNTVCT 1
#endif

#if defined(CNTVCT)
#define TIMESTAMP_SOURCE_NAME "cntvct_el0"
#define TIMESTAMP_UNIT_NAME "ticks"
#elif defined(PMCCNTR)
#define TIMESTAMP_SOURCE_NAME "pmccntr_el0"
#define TIMESTAMP_UNIT_NAME "cycles"
#else
#define TIMESTAMP_SOURCE_NAME "clock_gettime(CLOCK_MONOTONIC)"
#define TIMESTAMP_UNIT_NAME "ns"
#endif

static inline __attribute__((always_inline)) void mfence(void) {
    asm volatile("DSB SY\nISB" ::: "memory");
}

static inline __attribute__((always_inline)) void flush(void *addr) {
    asm volatile("DC CIVAC, %0" :: "r"(addr) : "memory");
}
// static inline __attribute__((always_inline)) void flush(void *p) {
//     asm volatile("dc civac, %0"::"r"(p));
//     asm volatile("dsb ish");
//     asm volatile("isb");
// }


#define _mStore(pre, addr)                      \
    asm volatile(pre "strb w0, [%0]\n\t"        \
                 :: "r"(addr)                   \
                 : "memory", "w0")

#define _mLoad(pre, addr)                       \
    asm volatile(pre "ldrb w0, [%0]\n\t"        \
                 :: "r"(addr)                   \
                 : "memory", "w0")

#define _mPrefetch(pre, addr)                   \
    asm volatile(pre "PRFM PLDL1KEEP, [%0]\n\t" \
                 :: "r"(addr))


static inline __attribute__((always_inline)) uint64_t timestamp(void) {
#if defined(CNTVCT)
    uint64_t time;

    asm volatile(
        "dsb sy\n\t"
        "isb\n\t"
        "mrs %0, cntvct_el0\n\t"
        "isb\n\t"
        : "=r"(time)
        :
        : "memory");
    return time;
#elif defined(PMCCNTR)
    uint64_t cycles;

    asm volatile(
        "dsb sy\n\t"
        "isb\n\t"
        "mrs %0, pmccntr_el0\n\t"
        "isb\n\t"
        : "=r"(cycles)
        :
        : "memory");
    return cycles;
#else
    struct timespec t;

    asm volatile("DSB SY" ::: "memory");
    asm volatile("ISB" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t);
    asm volatile("ISB" ::: "memory");
    asm volatile("DSB SY" ::: "memory");

    return t.tv_sec * 1000ULL * 1000ULL * 1000ULL + t.tv_nsec;
#endif
}
#else
#error "unknown architecture. Only x86_64 and aarch64 are supported"
#endif

static inline __attribute__((always_inline)) void mStore_inline(void *addr) {
    _mStore("", addr);
}

static inline __attribute__((always_inline)) void mLoad_inline(void *addr) {
    _mLoad("", addr);
}

static inline __attribute__((always_inline)) void mPrefetch_inline(void *addr) {
    _mPrefetch("", addr);
}

static inline __attribute__((always_inline)) void mStore(void *addr) {
    mStore_inline(addr);
}

static inline __attribute__((always_inline)) void mLoad(void *addr) {
    mLoad_inline(addr);
}

static inline __attribute__((always_inline)) void mPrefetch(void *addr) {
    mPrefetch_inline(addr);
}

void mStore_noinline(void *addr);
void mLoad_noinline(void *addr);
void mPrefetch_noinline(void *addr);

static inline __attribute__((always_inline)) void maccess(void *addr) {
    mLoad_inline(addr);
}

static inline __attribute__((always_inline)) void dummyAccess(void *buffer,
                                                             size_t size) {
    uint8_t *dummy_buffer = (uint8_t *)buffer;
    size_t lines = size / LINE_SIZE;
    // size_t step = 97;


    for (size_t n = 0; n < lines; n++) {
        // size_t line = (n) % lines;
        mPrefetch_inline(dummy_buffer + n * LINE_SIZE);
        // asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(dummy_buffer + line * LINE_SIZE));
    }
    // mfence();
}

static inline __attribute__((always_inline)) void nops(){
    for(int k = 0; k < 100; k++) {
        nop();
    }
}

typedef struct {
    uint8_t *base_addr;
    size_t size;
} Mapping;

#ifndef RANDOM_ACTIVITY_ITERS
#define RANDOM_ACTIVITY_ITERS 100000
#endif

static inline Mapping allocate_mapping(size_t mem_size) {
    uint8_t *base_addr = (uint8_t *)mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                                         MAP_POPULATE | MAP_PRIVATE | MAP_ANONYMOUS,
                                         -1, 0);
    if (base_addr == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    Mapping mapping = {base_addr, mem_size};
    return mapping;
}

static inline void unmap_mapping(Mapping mapping) {
    munmap(mapping.base_addr, mapping.size);
}

static inline __attribute__((always_inline)) size_t permute(size_t upper_bound,
                                                            size_t original_idx) {
    return ((original_idx * 167u) + 13u) & (upper_bound - 1);
}

static inline void flush_mapping(Mapping mapping) {
    for (uint8_t *ptr = mapping.base_addr;
         ptr < mapping.base_addr + mapping.size;
         ptr += LINE_SIZE) {
        flush(ptr);
    }
    mfence();
}

static inline void cpp_rctx(void)
{
#ifdef __aarch64__
    asm volatile(
        // "isb\n\t"
        "cpp rctx, xzr\n"
        "dsb sy\n\t"
        "isb\n\t"
        ::: "memory");
#endif
}

static inline void random_activity(Mapping mapping) {
    size_t cls_per_page = PAGE_SIZE / LINE_SIZE;

    for (size_t i = 0; i < RANDOM_ACTIVITY_ITERS; i++) {
        flush_mapping(mapping);
        for (size_t page = 0; page < mapping.size / PAGE_SIZE; page++) {
            for (size_t cl = 0; cl < cls_per_page; cl += 2) {
                uint8_t *addr = mapping.base_addr + page * PAGE_SIZE +
                                permute(cls_per_page, cl) * LINE_SIZE;
                maccess(addr);
            }
            mfence();
        }
    }
}

#endif
