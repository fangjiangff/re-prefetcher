#ifndef CACHEUTILS_H
#define CACHEUTILS_H


#define CACHE_LINE_SIZE 64
#define PAGE_SIZE 4096


__attribute__((always_inline)) static inline uint64_t rdtsc() {
  uint64_t a, d;
  asm volatile ("mfence");
  asm volatile ("rdtsc" : "=a" (a), "=d" (d));
  a = (d<<32) | a;
  asm volatile ("mfence");
  return a;
}

// __attribute__((always_inline)) static inline void maccess(void* p)
// {
//   asm volatile ("movq (%0), %%rax\n"
//     :
//     : "c" (p)
//     : "rax");
// }


// void randomAaccess()
// {
//   maccess();
// }

__attribute__((always_inline)) static inline void flush(void* p) {
    asm volatile ("clflush 0(%0)\n"
      :
      : "c" (p)
      : "rax");
}

__attribute__((always_inline)) static inline void mfence() { 
  asm volatile("mfence"); 
}

// __attribute__((always_inline)) static inline int flush_reload_t(void *ptr) {
// 	uint64_t start = 0, end = 0;

// 	start = rdtsc();
// 	maccess(ptr);
// 	end = rdtsc();

// 	mfence();

// 	flush(ptr);

// 	return (int)(end - start);
// }


#endif
