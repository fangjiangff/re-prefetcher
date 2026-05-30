#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <time.h>
#include <vector>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#ifndef LINE_SIZE
#define LINE_SIZE 64
#endif

#ifndef DEFAULT_ARRAY_MB
#define DEFAULT_ARRAY_MB 256
#endif

#ifndef DEFAULT_SAMPLES
#define DEFAULT_SAMPLES 1000
#endif

#ifndef USE_CNTVCT
#define USE_CNTVCT 0
#endif

#ifndef USE_CLOCK_GETTIME
#define USE_CLOCK_GETTIME 0
#endif

#ifndef LOAD_MISS_MAX_CYCLES
#define LOAD_MISS_MAX_CYCLES 600
#endif

static volatile uint64_t load_sink = 0;

enum PrefetchOp {
#if defined(__aarch64__)
    PLDL1KEEP,
    PLDL1STRM,
    PLDL2KEEP,
    PLDL2STRM,
    PLDL3KEEP,
    PLDL3STRM,
    PLIL1KEEP,
    PLIL1STRM,
    PLIL2KEEP,
    PLIL2STRM,
    PLIL3KEEP,
    PLIL3STRM,
    PSTL1KEEP,
    PSTL1STRM,
    PSTL2KEEP,
    PSTL2STRM,
    PSTL3KEEP,
    PSTL3STRM,
#elif defined(__x86_64__) || defined(__i386__)
    PREFETCHNTA_OP,
    PREFETCHT0_OP,
    PREFETCHT1_OP,
    PREFETCHT2_OP,
    PREFETCHW_OP,
    PREFETCHWT1_OP,
#endif
};

struct PrefetchCase {
    PrefetchOp op;
    const char *name;
};

static const PrefetchCase kPrefetchCases[] = {
#if defined(__aarch64__)
    {PLDL1KEEP, "PLDL1KEEP"},
    {PLDL1STRM, "PLDL1STRM"},
    {PLDL2KEEP, "PLDL2KEEP"},
    {PLDL2STRM, "PLDL2STRM"},
    {PLDL3KEEP, "PLDL3KEEP"},
    {PLDL3STRM, "PLDL3STRM"},
    {PLIL1KEEP, "PLIL1KEEP"},
    {PLIL1STRM, "PLIL1STRM"},
    {PLIL2KEEP, "PLIL2KEEP"},
    {PLIL2STRM, "PLIL2STRM"},
    {PLIL3KEEP, "PLIL3KEEP"},
    {PLIL3STRM, "PLIL3STRM"},
    {PSTL1KEEP, "PSTL1KEEP"},
    {PSTL1STRM, "PSTL1STRM"},
    {PSTL2KEEP, "PSTL2KEEP"},
    {PSTL2STRM, "PSTL2STRM"},
    {PSTL3KEEP, "PSTL3KEEP"},
    {PSTL3STRM, "PSTL3STRM"},
#elif defined(__x86_64__) || defined(__i386__)
    {PREFETCHNTA_OP, "PREFETCHNTA"},
    {PREFETCHT0_OP, "PREFETCHT0"},
    {PREFETCHT1_OP, "PREFETCHT1"},
    {PREFETCHT2_OP, "PREFETCHT2"},
    {PREFETCHW_OP, "PREFETCHW"},
    {PREFETCHWT1_OP, "PREFETCHWT1"},
#endif
};

static bool is_prefetch_supported(PrefetchOp op) {
#if defined(__x86_64__) || defined(__i386__)
    if (op == PREFETCHW_OP) {
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        return __get_cpuid(0x80000001U, &eax, &ebx, &ecx, &edx) && ((ecx & (1U << 8)) != 0);
    }
    if (op == PREFETCHWT1_OP) {
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        return __get_cpuid_count(7U, 0U, &eax, &ebx, &ecx, &edx) && ((ecx & (1U << 0)) != 0);
    }
#else
    (void)op;
#endif
    return true;
}

static inline uint64_t read_cycles() {
#if USE_CLOCK_GETTIME
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
#elif defined(__aarch64__)
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

static inline void sw_prefetch(void *addr, PrefetchOp op) {
#if defined(__aarch64__)
    switch (op) {
    case PLDL1KEEP:
        asm volatile("PRFM PLDL1KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLDL1STRM:
        asm volatile("PRFM PLDL1STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLDL2KEEP:
        asm volatile("PRFM PLDL2KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLDL2STRM:
        asm volatile("PRFM PLDL2STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLDL3KEEP:
        asm volatile("PRFM PLDL3KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLDL3STRM:
        asm volatile("PRFM PLDL3STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLIL1KEEP:
        asm volatile("PRFM PLIL1KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLIL1STRM:
        asm volatile("PRFM PLIL1STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLIL2KEEP:
        asm volatile("PRFM PLIL2KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLIL2STRM:
        asm volatile("PRFM PLIL2STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLIL3KEEP:
        asm volatile("PRFM PLIL3KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PLIL3STRM:
        asm volatile("PRFM PLIL3STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PSTL1KEEP:
        asm volatile("PRFM PSTL1KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PSTL1STRM:
        asm volatile("PRFM PSTL1STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PSTL2KEEP:
        asm volatile("PRFM PSTL2KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PSTL2STRM:
        asm volatile("PRFM PSTL2STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PSTL3KEEP:
        asm volatile("PRFM PSTL3KEEP, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    case PSTL3STRM:
        asm volatile("PRFM PSTL3STRM, [%0]\n\t" :: "r"(addr) : "memory");
        break;
    }
#elif defined(__x86_64__) || defined(__i386__)
    switch (op) {
    case PREFETCHNTA_OP:
        asm volatile("PREFETCHNTA 0(%0)\n\t" :: "r"(addr) : "memory");
        break;
    case PREFETCHT0_OP:
        asm volatile("PREFETCHT0 0(%0)\n\t" :: "r"(addr) : "memory");
        break;
    case PREFETCHT1_OP:
        asm volatile("PREFETCHT1 0(%0)\n\t" :: "r"(addr) : "memory");
        break;
    case PREFETCHT2_OP:
        asm volatile("PREFETCHT2 0(%0)\n\t" :: "r"(addr) : "memory");
        break;
    case PREFETCHW_OP:
        asm volatile("PREFETCHW 0(%0)\n\t" :: "r"(addr) : "memory");
        break;
    case PREFETCHWT1_OP:
        asm volatile("PREFETCHWT1 0(%0)\n\t" :: "r"(addr) : "memory");
        break;
    }
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

static int write_result(FILE *fp,
                        const char *kind,
                        const char *name,
                        int samples,
                        double empty_avg,
                        double raw_avg) {
    return fprintf(fp, "%s,%s,%d,%.2f,%.2f,%.2f\n",
                   kind, name, samples, empty_avg, raw_avg,
                   subtract_baseline(raw_avg, empty_avg)) < 0
               ? -1
               : 0;
}

static uint64_t measure_empty_once() {
    const uint64_t start = read_cycles();
    compiler_barrier();
    const uint64_t end = read_cycles();
    return end - start;
}

static uint64_t measure_prefetch_once(uint8_t *addr, PrefetchOp op) {
    const uint64_t start = read_cycles();
    mfence();
    sw_prefetch(addr, op);
    mfence();
    const uint64_t end = read_cycles();
    // mfence();
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

    printf("array_mb=%zu line_size=%d lines=%zu samples=%d seed=0x%x\n",
           array_mb, LINE_SIZE, lines, samples, seed);
#if USE_CLOCK_GETTIME
    printf("arch=portable timer=CLOCK_MONOTONIC_RAW unit=ns sw_prefetch_cases=%zu\n",
           sizeof(kPrefetchCases) / sizeof(kPrefetchCases[0]));
#elif defined(__aarch64__)
#if USE_CNTVCT
    printf("arch=aarch64 cycle_counter=CNTVCT_EL0 sw_prefetch_cases=%zu\n",
           sizeof(kPrefetchCases) / sizeof(kPrefetchCases[0]));
#else
    printf("arch=aarch64 cycle_counter=PMCCNTR_EL0 sw_prefetch_cases=%zu\n",
           sizeof(kPrefetchCases) / sizeof(kPrefetchCases[0]));
#endif
#else
    printf("arch=x86 cycle_counter=RDTSC sw_prefetch_cases=%zu\n",
           sizeof(kPrefetchCases) / sizeof(kPrefetchCases[0]));
#endif
    FILE *csv = NULL;
    if (raw_csv_path) {
        csv = fopen(raw_csv_path, "w");
        if (!csv) {
            fprintf(stderr, "failed to open csv '%s': %s\n", raw_csv_path, strerror(errno));
            munmap(mapping, bytes);
            return 1;
        }
        fprintf(csv, "kind,name,samples,empty_avg_cycles,raw_avg_cycles,avg_cycles\n");
    }

    std::vector<uint64_t> empty_cycles;
    std::vector<uint64_t> load_miss_cycles;
    empty_cycles.reserve(samples);
    load_miss_cycles.reserve(samples);

    mfence();
    for (int i = 0; i < samples; ++i) {
        empty_cycles.push_back(measure_empty_once());
    }
    for (int i = 0; i < samples; ++i) {
        uint8_t *addr = buffer + order[i];
        const uint64_t cycles = measure_load_miss_once(addr);
        load_miss_cycles.push_back(std::min<uint64_t>(cycles, LOAD_MISS_MAX_CYCLES));
    }
    mfence();

    const double empty_avg = average_cycles(empty_cycles);
    const double load_miss_raw_avg = average_cycles(load_miss_cycles);
    const double load_miss_avg = subtract_baseline(load_miss_raw_avg, empty_avg);

    printf("kind name empty_avg raw_avg adjusted_avg\n");
    printf("LOAD_MISS load_miss %.2f %.2f %.2f\n",
           empty_avg, load_miss_raw_avg, load_miss_avg);
    if (csv && write_result(csv, "LOAD_MISS", "load_miss", samples,
                            empty_avg, load_miss_raw_avg) != 0) {
        fprintf(stderr, "failed to write csv\n");
        fclose(csv);
        munmap(mapping, bytes);
        return 1;
    }

    for (size_t case_idx = 0; case_idx < sizeof(kPrefetchCases) / sizeof(kPrefetchCases[0]); ++case_idx) {
        const PrefetchCase &prefetch_case = kPrefetchCases[case_idx];
        if (!is_prefetch_supported(prefetch_case.op)) {
            printf("SW_PREFETCH %s unsupported_by_cpu\n", prefetch_case.name);
            continue;
        }

        std::vector<uint64_t> prefetch_cycles;
        prefetch_cycles.reserve(samples);

        for (int i = 0; i < samples; ++i) {
            uint8_t *addr = buffer + order[i];
            flush_line(addr);
            mfence();
            prefetch_cycles.push_back(measure_prefetch_once(addr, prefetch_case.op));
        }
        mfence();

        const double prefetch_raw_avg = average_cycles(prefetch_cycles);
        const double prefetch_avg = subtract_baseline(prefetch_raw_avg, empty_avg);

        printf("SW_PREFETCH %s %.2f %.2f %.2f\n",
               prefetch_case.name, empty_avg, prefetch_raw_avg, prefetch_avg);
        if (csv && write_result(csv, "SW_PREFETCH", prefetch_case.name, samples,
                                empty_avg, prefetch_raw_avg) != 0) {
            fprintf(stderr, "failed to write csv\n");
            fclose(csv);
            munmap(mapping, bytes);
            return 1;
        }
    }

    if (csv) {
        fclose(csv);
    }
    printf("sink=%llu\n", static_cast<unsigned long long>(load_sink));
    if (raw_csv_path) {
        printf("csv=%s\n", raw_csv_path);
    }

    munmap(mapping, bytes);
    return 0;
}
