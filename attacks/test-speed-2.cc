#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <vector>

#ifndef LINE_SIZE
#define LINE_SIZE 64
#endif

#ifndef DEFAULT_ARRAY_MB
#define DEFAULT_ARRAY_MB 256
#endif

#ifndef DEFAULT_TRIALS
#define DEFAULT_TRIALS 1000
#endif

#ifndef PRFM_MODE
#define PRFM_MODE PLDL1KEEP
#endif

#ifndef USE_CNTVCT
#define USE_CNTVCT 0
#endif

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static const int kGroupSizes[] = {100, 200, 400, 600, 800, 1000};
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
#error "This benchmark needs an AArch64 PMCCNTR_EL0 or x86 cycle counter."
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

static std::vector<size_t> random_line_order(size_t lines, uint32_t seed) {
    std::vector<size_t> order(lines);
    for (size_t i = 0; i < lines; ++i) {
        order[i] = i * LINE_SIZE;
    }

    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
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

static uint64_t measure_empty_group(int count) {
    const uint64_t start = read_cycles();
    for (int i = 0; i < count; ++i) {
        compiler_barrier();
    }
    const uint64_t end = read_cycles();
    return end - start;
}

static void flush_group(uint8_t *buffer, const std::vector<size_t> &order, size_t start_idx, int count) {
    for (int i = 0; i < count; ++i) {
        flush_line(buffer + order[start_idx + i]);
    }
    mfence();
}

static uint64_t measure_prefetch_group(uint8_t *buffer,
                                       const std::vector<size_t> &order,
                                       size_t start_idx,
                                       int count) {
    flush_group(buffer, order, start_idx, count);

    const uint64_t start = read_cycles();
    mfence();
    for (int i = 0; i < count; ++i) {
        sw_prefetch(buffer + order[start_idx + i]);
    }
    mfence();
    const uint64_t end = read_cycles();
    return end - start;
}

static uint64_t measure_load_group(uint8_t *buffer,
                                   const std::vector<size_t> &order,
                                   size_t start_idx,
                                   int count) {
    flush_group(buffer, order, start_idx, count);

    uint64_t sum = 0;
    const uint64_t start = read_cycles();
    mfence();
    for (int i = 0; i < count; ++i) {
        sum += load_byte(buffer + order[start_idx + i]);
    }
    mfence();
    const uint64_t end = read_cycles();

    load_sink += sum;
    return end - start;
}

static int write_header(FILE *fp) {
    return fprintf(fp,
                   "count,trials,empty_avg_cycles,prefetch_raw_avg_cycles,load_raw_avg_cycles,"
                   "prefetch_avg_cycles,load_avg_cycles,prefetch_cycles_per_elem,load_cycles_per_elem\n") < 0
               ? -1
               : 0;
}

int main(int argc, char **argv) {
    const size_t array_mb = argc > 1 ? strtoull(argv[1], NULL, 0) : DEFAULT_ARRAY_MB;
    const int trials = argc > 2 ? atoi(argv[2]) : DEFAULT_TRIALS;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(strtoul(argv[3], NULL, 0)) : 0x5eed1234U;
    const char *csv_path = argc > 4 ? argv[4] : NULL;

    if (array_mb == 0 || trials <= 0) {
        fprintf(stderr, "usage: %s [array_mb=%d] [trials=%d] [seed=0x5eed1234] [csv]\n",
                argv[0], DEFAULT_ARRAY_MB, DEFAULT_TRIALS);
        return 1;
    }

    const size_t bytes = array_mb * 1024ULL * 1024ULL;
    const size_t lines = bytes / LINE_SIZE;
    const int max_count = kGroupSizes[sizeof(kGroupSizes) / sizeof(kGroupSizes[0]) - 1];
    const size_t needed_lines = static_cast<size_t>(trials) * static_cast<size_t>(max_count) * 2ULL;
    if (lines < needed_lines) {
        fprintf(stderr, "array has %zu lines, but this experiment needs at least %zu lines\n",
                lines, needed_lines);
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
    std::vector<size_t> order = random_line_order(lines, seed);

    FILE *csv = NULL;
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) {
            fprintf(stderr, "failed to open csv '%s': %s\n", csv_path, strerror(errno));
            munmap(mapping, bytes);
            return 1;
        }
        if (write_header(csv) != 0) {
            fprintf(stderr, "failed to write csv header\n");
            fclose(csv);
            munmap(mapping, bytes);
            return 1;
        }
    }

    printf("array_mb=%zu line_size=%d lines=%zu trials=%d seed=0x%x\n",
           array_mb, LINE_SIZE, lines, trials, seed);
#if defined(__aarch64__)
#if USE_CNTVCT
    printf("cycle_counter=CNTVCT_EL0 pw_instruction=PRFM_%s\n", STR(PRFM_MODE));
#else
    printf("cycle_counter=PMCCNTR_EL0 pw_instruction=PRFM_%s\n", STR(PRFM_MODE));
#endif
#else
    printf("cycle_counter=RDTSC pw_instruction=PREFETCHT0\n");
#endif
    printf("count\tempty\tprefetch_total\tload_total\tprefetch_per_elem\tload_per_elem\n");

    for (size_t gi = 0; gi < sizeof(kGroupSizes) / sizeof(kGroupSizes[0]); ++gi) {
        const int count = kGroupSizes[gi];
        std::vector<uint64_t> empty_cycles;
        std::vector<uint64_t> prefetch_cycles;
        std::vector<uint64_t> load_cycles;
        empty_cycles.reserve(trials);
        prefetch_cycles.reserve(trials);
        load_cycles.reserve(trials);

        const size_t prefetch_base = 0;
        const size_t load_base = static_cast<size_t>(trials) * static_cast<size_t>(max_count);

        for (int t = 0; t < trials; ++t) {
            const size_t prefetch_idx = prefetch_base + static_cast<size_t>(t) * max_count;
            const size_t load_idx = load_base + static_cast<size_t>(t) * max_count;

            empty_cycles.push_back(measure_empty_group(count));
            prefetch_cycles.push_back(measure_prefetch_group(buffer, order, prefetch_idx, count));
            load_cycles.push_back(measure_load_group(buffer, order, load_idx, count));
        }

        const double empty_avg = average_cycles(empty_cycles);
        const double prefetch_raw_avg = average_cycles(prefetch_cycles);
        const double load_raw_avg = average_cycles(load_cycles);
        const double prefetch_avg = subtract_baseline(prefetch_raw_avg, empty_avg);
        const double load_avg = subtract_baseline(load_raw_avg, empty_avg);

        printf("%d\t%.2f\t%.2f\t\t%.2f\t\t%.2f\t\t%.2f\n",
               count, empty_avg, prefetch_avg, load_avg,
               prefetch_avg / count, load_avg / count);

        if (csv) {
            fprintf(csv, "%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f\n",
                    count, trials, empty_avg, prefetch_raw_avg, load_raw_avg,
                    prefetch_avg, load_avg, prefetch_avg / count, load_avg / count);
        }
    }

    if (csv) {
        fclose(csv);
        printf("csv=%s\n", csv_path);
    }
    printf("sink=%llu\n", static_cast<unsigned long long>(load_sink));

    munmap(mapping, bytes);
    return 0;
}
