#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kernel_stride_pf_ioctl.h"

#define DEFAULT_ROUNDS 1000
#define TRAIN_REPEATS 5
#define DUMMY_BUFFER_SIZE (4096 * 16)

#if defined(__x86_64__) || defined(__i386__)
#define USER_PREFETCH_MODE "prefetcht0(kernel_va)"

static inline void user_prefetch_kernel_va(const void *addr)
{
    __asm__ volatile("prefetcht0 (%0)" : : "r"(addr) : "memory");
}

static inline void full_barrier_user(void)
{
    __asm__ volatile("mfence" : : : "memory");
}

static inline void cpu_relax_user(void)
{
    __asm__ volatile("pause" : : : "memory");
}

#elif defined(__aarch64__)
#define USER_PREFETCH_MODE "prfm pldl1keep(kernel_va)"

static inline void user_prefetch_kernel_va(const void *addr)
{
    __asm__ volatile("prfm pldl1keep, [%0]" : : "r"(addr) : "memory");
}

static inline void full_barrier_user(void)
{
    __asm__ volatile("dsb ish" : : : "memory");
}

static inline void cpu_relax_user(void)
{
    __asm__ volatile("yield" : : : "memory");
}

#else
#error "kernel_stride_pf_user supports only x86 and arm64"
#endif

static void train_on_kernel_array(uintptr_t kernel_base)
{
    const uintptr_t stride =
        KERNEL_STRIDE_PF_STRIDE_LINES * KERNEL_STRIDE_PF_LINE_SIZE;

    for (int repeat = 0; repeat < TRAIN_REPEATS; ++repeat) {
        for (int step = 0; step < KERNEL_STRIDE_PF_TRAIN_STEP; ++step) {
            user_prefetch_kernel_va((const void *)(kernel_base + (uintptr_t)step * stride));
            full_barrier_user();
        }
    }
}

static void wait_for_prefetch(void)
{
    for (volatile int i = 0; i < 1000; ++i) {
        cpu_relax_user();
    }
}

static void dummy_accesses(unsigned char *dummy_buffer)
{
    volatile uint64_t sum = 0;

    for (size_t i = 0; i < DUMMY_BUFFER_SIZE; i += KERNEL_STRIDE_PF_LINE_SIZE) {
        sum += dummy_buffer[i];
    }

    (void)sum;
}

static size_t parse_rounds(int argc, char **argv)
{
    unsigned long value;
    char *endptr = NULL;

    if (argc < 2)
        return DEFAULT_ROUNDS;

    errno = 0;
    value = strtoul(argv[1], &endptr, 0);
    if (errno != 0 || endptr == argv[1] || *endptr != '\0' || value == 0) {
        fprintf(stderr, "usage: %s [rounds]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    return (size_t)value;
}

static void flush_kernel_array(int fd)
{
    if (ioctl(fd, KERNEL_STRIDE_PF_IOC_FLUSH) != 0) {
        perror("ioctl FLUSH");
        exit(EXIT_FAILURE);
    }
}

static void kernel_train_array(int fd)
{
    if (ioctl(fd, KERNEL_STRIDE_PF_IOC_KERNEL_TRAIN) != 0) {
        perror("ioctl KERNEL_TRAIN");
        exit(EXIT_FAILURE);
    }
}

static uint64_t measure_kernel_line(int fd, unsigned int line)
{
    struct kernel_stride_pf_measure measurement;

    memset(&measurement, 0, sizeof(measurement));
    measurement.line = line;

    if (ioctl(fd, KERNEL_STRIDE_PF_IOC_MEASURE, &measurement) != 0) {
        perror("ioctl MEASURE");
        exit(EXIT_FAILURE);
    }

    return measurement.cycles;
}

static void run_user_prefetch_rounds(
    int fd,
    uintptr_t kernel_base,
    unsigned char *dummy_buffer,
    size_t rounds,
    unsigned long long sums[KERNEL_STRIDE_PF_PROBE_LINES],
    unsigned int counts[KERNEL_STRIDE_PF_PROBE_LINES]
)
{
    for (size_t round = 0; round < rounds; ++round) {
        int line = (int)(round % KERNEL_STRIDE_PF_PROBE_LINES);

        flush_kernel_array(fd);
        dummy_accesses(dummy_buffer);
        train_on_kernel_array(kernel_base);
        wait_for_prefetch();

        sums[line] += measure_kernel_line(fd, (unsigned int)line);
        counts[line]++;
    }
}

static void run_kernel_train_rounds(
    int fd,
    unsigned char *dummy_buffer,
    size_t rounds,
    unsigned long long sums[KERNEL_STRIDE_PF_PROBE_LINES],
    unsigned int counts[KERNEL_STRIDE_PF_PROBE_LINES]
)
{
    for (size_t round = 0; round < rounds; ++round) {
        int line = (int)(round % KERNEL_STRIDE_PF_PROBE_LINES);

        flush_kernel_array(fd);
        dummy_accesses(dummy_buffer);
        kernel_train_array(fd);
        wait_for_prefetch();

        sums[line] += measure_kernel_line(fd, (unsigned int)line);
        counts[line]++;
    }
}

static const char *line_role(int line)
{
    const int stride_line = KERNEL_STRIDE_PF_STRIDE_LINES;

    if (line % stride_line == 0 && line / stride_line < KERNEL_STRIDE_PF_TRAIN_STEP) {
        return "train_input";
    }
    if (line % stride_line == 0 && line / stride_line >= KERNEL_STRIDE_PF_TRAIN_STEP) {
        return "stride_prediction";
    }
    return "probe";
}

static void print_result_table(
    const char *title,
    const char *mode,
    const unsigned long long sums[KERNEL_STRIDE_PF_PROBE_LINES],
    const unsigned int counts[KERNEL_STRIDE_PF_PROBE_LINES]
)
{
    printf("\n# %s\n", title);
    printf("# access mode: %s\n", mode);
    printf("# line\toffset_bytes\tavg_cycles\trole\n");

    for (int line = 0; line < KERNEL_STRIDE_PF_PROBE_LINES; ++line) {
        printf("%3d\t%12d\t%10llu\t%s\n",
               line,
               line * KERNEL_STRIDE_PF_LINE_SIZE,
               counts[line] == 0 ? 0 : sums[line] / (unsigned long long)counts[line],
               line_role(line));
    }
}

int main(int argc, char **argv)
{
    const char *dev_path = "/dev/kernel_stride_pf_probe";
    const size_t rounds = parse_rounds(argc, argv);
    unsigned long long user_sums[KERNEL_STRIDE_PF_PROBE_LINES] = {0};
    unsigned int user_counts[KERNEL_STRIDE_PF_PROBE_LINES] = {0};
    unsigned long long kernel_sums[KERNEL_STRIDE_PF_PROBE_LINES] = {0};
    unsigned int kernel_counts[KERNEL_STRIDE_PF_PROBE_LINES] = {0};
    unsigned char *dummy_buffer;
    uint64_t kernel_base = 0;
    int fd;

    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev_path, strerror(errno));
        fprintf(stderr, "build and load the module first:\n");
        fprintf(stderr, "  make module\n");
        fprintf(stderr, "  sudo insmod kernel_stride_pf_probe.ko\n");
        return EXIT_FAILURE;
    }

    if (ioctl(fd, KERNEL_STRIDE_PF_IOC_GET_BASE, &kernel_base) != 0) {
        perror("ioctl GET_BASE");
        close(fd);
        return EXIT_FAILURE;
    }

    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        perror("mmap dummy_buffer");
        close(fd);
        return EXIT_FAILURE;
    }

    memset(dummy_buffer, 0xa5, DUMMY_BUFFER_SIZE);

    run_user_prefetch_rounds(fd, (uintptr_t)kernel_base, dummy_buffer,
                             rounds, user_sums, user_counts);
    run_kernel_train_rounds(fd, dummy_buffer, rounds, kernel_sums, kernel_counts);

    printf("# kernel-array stride prefetch probe from user mode\n");
    printf("# kernel_array2_va=0x%016" PRIx64 "\n", kernel_base);
    printf("# stride_lines=%d stride_bytes=%d train_step=%d train_repeats=%d rounds=%zu probe_lines=%d\n",
           KERNEL_STRIDE_PF_STRIDE_LINES,
           KERNEL_STRIDE_PF_STRIDE_LINES * KERNEL_STRIDE_PF_LINE_SIZE,
           KERNEL_STRIDE_PF_TRAIN_STEP,
           TRAIN_REPEATS,
           rounds,
           KERNEL_STRIDE_PF_PROBE_LINES);
    print_result_table("test: user prefetch(kernel_va)",
                       "user " USER_PREFETCH_MODE, user_sums, user_counts);
    print_result_table("control: kernel load(array2 + step * stride)",
                       "kernel load(kernel_va)", kernel_sums, kernel_counts);

    munmap(dummy_buffer, DUMMY_BUFFER_SIZE);
    close(fd);
    return EXIT_SUCCESS;
}
