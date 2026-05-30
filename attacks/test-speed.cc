#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <vector>
#include "time.h"

#ifndef LINE_SIZE
#define LINE_SIZE 64
#endif

#ifndef DEFAULT_ARRAY_MB
#define DEFAULT_ARRAY_MB 256
#endif

#ifndef DEFAULT_SAMPLES
#define DEFAULT_SAMPLES 1000
#endif

#ifndef PRFM_MODE
#define PRFM_MODE PLDL3KEEP
#endif

#ifndef USE_CNTVCT
#define USE_CNTVCT 0
#endif

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static volatile uint64_t load_sink = 0;

static inline uint64_t read_cycles() {
#if defined(__aarch64__)
    uint64_t value;
#if USE_CNTVCT
    asm volatile("ISB\n\tMRS %0, CNTVCT_EL0\n\tISB" : "=r"(value) :: "memory");
#else
    asm volatile("ISB\n\tMRS %0, PMCCNTR_EL0\n\tISB" : "=r"(value) :: "memory");
#endif
    return value;
#elif defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    asm volatile("LFENCE\n\tRDTSC" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
#error "This benchmark needs an AArch64 PMCCNTR_EL0 or x86 RDTSCP cycle counter."
#endif
}


static inline void mfence() {
#if defined(__aarch64__)
    asm volatile("DSB SY\nISB" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("MFENCE\nLFENCE" ::: "memory");
#endif
}

static inline void compiler_barrier() {
    asm volatile("" ::: "memory");
}

static inline void flush_line(void *addr) {
#if defined(__aarch64__)
    asm volatile("DC CIVAC, %0" :: "r"(addr) : "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("CLFLUSH 0(%0)" :: "r"(addr) : "memory");
#endif
}

static inline void sw_prefetch(void *addr) {
#if defined(__aarch64__)
    asm volatile("PRFM " STR(PRFM_MODE) ", [%0]\n\t" :: "r"(addr) : "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("PREFETCHT0 0(%0)\n\t" :: "r"(addr) : "memory");
#endif
}

static inline uint8_t load_byte(void *addr) {
    uint8_t value;
#if defined(__aarch64__)
    asm volatile("LDRB %w0, [%1]\n\t" : "=r"(value) : "r"(addr) : "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("MOVB 0(%1), %0\n\t" : "=r"(value) : "r"(addr) : "memory");
#endif
    return value;
}

static void initialize_lines(uint8_t *buffer, size_t lines) {
    for (size_t i = 0; i < lines; ++i) {
        buffer[i * LINE_SIZE] = static_cast<uint8_t>(i + 1);
    }
}

static std::vector<size_t> random_line_order(size_t lines, int samples, uint32_t seed) {
    std::vector<size_t> order(lines);
    for (size_t i = 0; i < lines; ++i) {
        order[i] = i * LINE_SIZE;
    }

    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);

    if (static_cast<size_t>(samples) < order.size()) {
        order.resize(samples);
    }
    return order;
}

static double average_cycles(const std::vector<uint64_t> &values) {
    long double sum = 0.0;
    for (uint64_t value : values) {
        sum += value;
    }
    return values.empty() ? 0.0 : static_cast<double>(sum / values.size());
}

static double subtract_baseline(double value, double baseline) {
    return value > baseline ? value - baseline : 0.0;
}

static uint64_t subtract_baseline_u64(uint64_t value, double baseline) {
    return value > baseline ? static_cast<uint64_t>(value - baseline) : 0;
}

static int write_raw_samples(const char *path,
                             const std::vector<uint64_t> &empty_cycles,
                             const std::vector<uint64_t> &prefetch_cycles,
                             const std::vector<uint64_t> &hit_cycles,
                             const std::vector<uint64_t> &miss_cycles,
                             double empty_avg) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "failed to open raw csv '%s': %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "sample,empty_cycles,prefetch_raw_cycles,load_hit_raw_cycles,load_miss_raw_cycles,"
                "prefetch_cycles,load_hit_cycles,load_miss_cycles\n");

    const size_t samples = prefetch_cycles.size();
    for (size_t i = 0; i < samples; ++i) {
        fprintf(fp, "%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                i,
                static_cast<unsigned long long>(empty_cycles[i]),
                static_cast<unsigned long long>(prefetch_cycles[i]),
                static_cast<unsigned long long>(hit_cycles[i]),
                static_cast<unsigned long long>(miss_cycles[i]),
                static_cast<unsigned long long>(subtract_baseline_u64(prefetch_cycles[i], empty_avg)),
                static_cast<unsigned long long>(subtract_baseline_u64(hit_cycles[i], empty_avg)),
                static_cast<unsigned long long>(subtract_baseline_u64(miss_cycles[i], empty_avg)));
    }

    fclose(fp);
    return 0;
}

static uint64_t measure_empty_once() {
    const uint64_t start = read_cycles();
    compiler_barrier();
    const uint64_t end = read_cycles();
    return end - start;
}

static uint64_t measure_prefetch_once(uint8_t *addr) {
    const uint64_t start = read_cycles();
    mfence();
    sw_prefetch(addr);
    mfence();
    const uint64_t end = read_cycles();
    // mfence();
    return end - start;
}

static uint64_t measure_load_hit_once(uint8_t *addr) {
    load_sink += load_byte(addr);
    mfence();

    const uint64_t start = read_cycles();
    mfence();
    const uint8_t value = load_byte(addr);
    mfence();
    const uint64_t end = read_cycles();
    // mfence();
    load_sink += value;
    return end - start;
}

static uint64_t measure_load_miss_once(uint8_t *addr) {
    flush_line(addr);
    mfence();

    const uint64_t start = read_cycles();
    mfence();
    const uint8_t value = load_byte(addr);
    mfence();
    const uint64_t end = read_cycles();
    // mfence();

    load_sink += value;
    return end - start;
}

int main(int argc, char **argv) {
    const size_t array_mb = argc > 1 ? strtoull(argv[1], NULL, 0) : DEFAULT_ARRAY_MB;
    const int samples = argc > 2 ? atoi(argv[2]) : DEFAULT_SAMPLES;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(strtoul(argv[3], NULL, 0)) : 0x5eed1234U;
    const char *raw_csv_path = argc > 4 ? argv[4] : NULL;

    if (array_mb == 0 || samples <= 0) {
        fprintf(stderr, "usage: %s [array_mb=%d] [samples=%d] [seed=0x5eed1234] [raw_csv]\n",
                argv[0], DEFAULT_ARRAY_MB, DEFAULT_SAMPLES);
        return 1;
    }

    const size_t bytes = array_mb * 1024ULL * 1024ULL;
    const size_t lines = bytes / LINE_SIZE;
    if (lines < static_cast<size_t>(samples)) {
        fprintf(stderr, "array has only %zu lines, but samples=%d\n", lines, samples);
        return 1;
    }

    void *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap(%zu bytes) failed: %s\n", bytes, strerror(errno));
        return 1;
    }

    uint8_t *buffer = static_cast<uint8_t *>(mapping);
    initialize_lines(buffer, lines);

    std::vector<size_t> order = random_line_order(lines, samples, seed);
    std::vector<uint64_t> empty_cycles;
    std::vector<uint64_t> prefetch_cycles;
    std::vector<uint64_t> hit_cycles;
    std::vector<uint64_t> miss_cycles;

    empty_cycles.reserve(samples);
    prefetch_cycles.reserve(samples);
    hit_cycles.reserve(samples);
    miss_cycles.reserve(samples);

    mfence();
    for (int i = 0; i < samples; ++i) {
        empty_cycles.push_back(measure_empty_once());
    }

    for (int i = 0; i < samples; ++i) {
        uint8_t *addr = buffer + order[i];
        flush_line(addr);
        mfence();
        prefetch_cycles.push_back(measure_prefetch_once(addr));
    }

    for (int i = 0; i < samples; ++i) {
        uint8_t *addr = buffer + order[i];
        hit_cycles.push_back(measure_load_hit_once(addr));
    }

    for (int i = 0; i < samples; ++i) {
        uint8_t *addr = buffer + order[i];
        miss_cycles.push_back(measure_load_miss_once(addr));
    }
    mfence();

    for(int i=0; i<samples; i++){
        printf("sample %d: empty=%llu prefetch_raw=%llu hit_raw=%llu miss_raw=%llu\n",
               i,
               static_cast<unsigned long long>(empty_cycles[i]),
               static_cast<unsigned long long>(prefetch_cycles[i]),
               static_cast<unsigned long long>(hit_cycles[i]),
               static_cast<unsigned long long>(miss_cycles[i]));
    }

    const double empty_avg = average_cycles(empty_cycles);
    const double prefetch_raw_avg = average_cycles(prefetch_cycles);
    const double hit_raw_avg = average_cycles(hit_cycles);
    const double miss_raw_avg = average_cycles(miss_cycles);

    if (raw_csv_path && write_raw_samples(raw_csv_path, empty_cycles, prefetch_cycles,
                                          hit_cycles, miss_cycles, empty_avg) != 0) {
        munmap(mapping, bytes);
        return 1;
    }

    printf("array_mb=%zu line_size=%d lines=%zu samples=%d seed=0x%x\n",
           array_mb, LINE_SIZE, lines, samples, seed);
#if defined(__aarch64__)
#if USE_CNTVCT
    printf("cycle_counter=CNTVCT_EL0 pw_instruction=PRFM_%s\n", STR(PRFM_MODE));
#else
    printf("cycle_counter=PMCCNTR_EL0 pw_instruction=PRFM_%s\n", STR(PRFM_MODE));
#endif
#else
    printf("cycle_counter=RDTSC pw_instruction=PREFETCHT0\n");
#endif
    printf("empty_avg_cycles=%.2f\n", empty_avg);
    printf("prefetch_raw_avg_cycles=%.2f prefetch_avg_cycles=%.2f\n",
           prefetch_raw_avg, subtract_baseline(prefetch_raw_avg, empty_avg));
    printf("load_hit_raw_avg_cycles=%.2f load_hit_avg_cycles=%.2f\n",
           hit_raw_avg, subtract_baseline(hit_raw_avg, empty_avg));
    printf("load_miss_raw_avg_cycles=%.2f load_miss_avg_cycles=%.2f sink=%llu\n",
           miss_raw_avg, subtract_baseline(miss_raw_avg, empty_avg),
           static_cast<unsigned long long>(load_sink));
    if (raw_csv_path) {
        printf("raw_csv=%s\n", raw_csv_path);
    }

    munmap(mapping, bytes);
    return 0;
}
