#ifndef UNTIL_H
#define UNTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifndef __aarch64__
#error "until.h expects AArch64 DC CIVAC, DSB/ISB, and STRB/LDRB."
#endif

#ifndef LINE_SIZE
#define LINE_SIZE 64
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

static inline __attribute__((always_inline)) void nop(void) {
    asm volatile("nop");
}

static inline __attribute__((always_inline)) void mfence(void) {
    asm volatile("DSB SY\nISB" ::: "memory");
}

static inline __attribute__((always_inline)) void flush(void *addr) {
    asm volatile("DC CIVAC, %0" :: "r"(addr) : "memory");
}

#define _mStore(pre, addr)                      \
    asm volatile(pre "strb w0, [%0]\n\t"        \
                 :: "r"(addr)                   \
                 : "memory", "w0")

#define _mLoad(pre, addr)                       \
    asm volatile(pre "ldrb w0, [%0]\n\t"        \
                 :: "r"(addr)                   \
                 : "memory", "w0")

static inline __attribute__((always_inline)) void mStore_inline(void *addr) {
    _mStore("", addr);
}

static inline __attribute__((always_inline)) void mLoad_inline(void *addr) {
    _mLoad("", addr);
}

static inline __attribute__((always_inline)) void mStore(void *addr) {
    mStore_inline(addr);
}

static inline __attribute__((always_inline)) void mLoad(void *addr) {
    mLoad_inline(addr);
}

void mStore_noinline(void *addr);
void mLoad_noinline(void *addr);

static inline __attribute__((always_inline)) void maccess(void *addr) {
    mLoad_inline(addr);
}

static inline __attribute__((always_inline)) uint64_t timestamp(void) {
    struct timespec t;

    asm volatile("DSB SY" ::: "memory");
    asm volatile("ISB" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t);
    asm volatile("ISB" ::: "memory");
    asm volatile("DSB SY" ::: "memory");

    return t.tv_sec * 1000ULL * 1000ULL * 1000ULL + t.tv_nsec;
}

static inline __attribute__((always_inline)) void dummyAccess(void *buffer,
                                                             size_t size) {
    uint8_t *dummy_buffer = (uint8_t *)buffer;

    for (size_t i = 0; i < size; i += LINE_SIZE) {
        mLoad(dummy_buffer + i);
    }
    mfence();
}

#endif
