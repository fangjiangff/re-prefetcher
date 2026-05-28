#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <x86intrin.h>

#include "prefetch_kcache_ioctl.h"

#define DEFAULT_ROUNDS 1000

static sigjmp_buf g_jmp_env;

static void fault_handler(int signo, siginfo_t *info, void *ucontext)
{
    (void)signo;
    (void)info;
    (void)ucontext;
    siglongjmp(g_jmp_env, 1);
}

static void install_fault_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGSEGV, &sa, NULL) != 0 || sigaction(SIGBUS, &sa, NULL) != 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

static inline void sw_prefetch(const void *addr)
{
    __asm__ volatile("prefetcht0 (%0)" : : "r"(addr) : "memory");
}

static void user_prefetch_many(const void *addr)
{
    for (int i = 0; i < 16; ++i) {
        sw_prefetch(addr);
    }
}

static int try_user_load(const void *addr)
{
    volatile unsigned char value;

    if (sigsetjmp(g_jmp_env, 1) != 0) {
        return -1;
    }

    value = *(const volatile unsigned char *)addr;
    (void)value;
    return 0;
}

static void wait_for_prefetch(void)
{
    for (volatile int i = 0; i < 512; ++i) {
        _mm_pause();
    }
}

static int cmp_u64(const void *lhs, const void *rhs)
{
    const uint64_t a = *(const uint64_t *)lhs;
    const uint64_t b = *(const uint64_t *)rhs;

    return (a > b) - (a < b);
}

static uint64_t percentile(uint64_t *values, size_t n, unsigned pct)
{
    size_t idx;

    if (n == 0)
        return 0;

    idx = ((n - 1) * (size_t)pct) / 100;
    return values[idx];
}

static double mean_cycles(const uint64_t *values, size_t n)
{
    long double sum = 0.0;

    for (size_t i = 0; i < n; ++i) {
        sum += values[i];
    }

    return (double)(sum / (long double)n);
}

static void print_stats(const char *label, uint64_t *values, size_t n)
{
    qsort(values, n, sizeof(values[0]), cmp_u64);
    printf("%-24s median=%" PRIu64 " p10=%" PRIu64 " p90=%" PRIu64 " mean=%.1f\n",
           label,
           percentile(values, n, 50),
           percentile(values, n, 10),
           percentile(values, n, 90),
           mean_cycles(values, n));
}

static uint64_t kernel_measure_once(int fd)
{
    uint64_t cycles = 0;

    if (ioctl(fd, PREFETCH_KCACHE_IOC_MEASURE, &cycles) != 0) {
        perror("ioctl MEASURE");
        exit(EXIT_FAILURE);
    }

    return cycles;
}

static void kernel_flush_once(int fd)
{
    if (ioctl(fd, PREFETCH_KCACHE_IOC_FLUSH) != 0) {
        perror("ioctl FLUSH");
        exit(EXIT_FAILURE);
    }
}

static void kernel_prefetch_once(int fd)
{
    if (ioctl(fd, PREFETCH_KCACHE_IOC_PREFETCH) != 0) {
        perror("ioctl PREFETCH");
        exit(EXIT_FAILURE);
    }
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

int main(int argc, char **argv)
{
    const char *dev_path = "/dev/prefetch_kcache_probe";
    const size_t rounds = parse_rounds(argc, argv);
    uint64_t kernel_addr = 0;
    uint64_t *cold = NULL;
    uint64_t *hot = NULL;
    uint64_t *after_kernel_prefetch = NULL;
    uint64_t *after_user_prefetch = NULL;
    uint64_t cold_median;
    uint64_t hot_median;
    uint64_t kernel_prefetch_median;
    uint64_t user_prefetch_median;
    int fd;

    install_fault_handlers();

    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev_path, strerror(errno));
        fprintf(stderr, "build and load the module first:\n");
        fprintf(stderr, "  make module\n");
        fprintf(stderr, "  sudo insmod prefetch_kcache_probe.ko\n");
        return EXIT_FAILURE;
    }

    if (ioctl(fd, PREFETCH_KCACHE_IOC_GET_ADDR, &kernel_addr) != 0) {
        perror("ioctl GET_ADDR");
        close(fd);
        return EXIT_FAILURE;
    }

    cold = calloc(rounds, sizeof(cold[0]));
    hot = calloc(rounds, sizeof(hot[0]));
    after_kernel_prefetch = calloc(rounds, sizeof(after_kernel_prefetch[0]));
    after_user_prefetch = calloc(rounds, sizeof(after_user_prefetch[0]));
    if (!cold || !hot || !after_kernel_prefetch || !after_user_prefetch) {
        perror("calloc");
        free(after_user_prefetch);
        free(after_kernel_prefetch);
        free(hot);
        free(cold);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Kernel-address software prefetch cache proof\n");
    printf("kernel probe VA      : 0x%016" PRIx64 "\n", kernel_addr);
    printf("rounds               : %zu\n", rounds);
    printf("user load kernel VA  : %s\n",
           try_user_load((const void *)(uintptr_t)kernel_addr) == 0 ? "ok" : "FAULT");
    printf("\n");

    for (size_t i = 0; i < rounds; ++i) {
        kernel_flush_once(fd);
        cold[i] = kernel_measure_once(fd);

        hot[i] = kernel_measure_once(fd);

        kernel_flush_once(fd);
        kernel_prefetch_once(fd);
        wait_for_prefetch();
        after_kernel_prefetch[i] = kernel_measure_once(fd);

        kernel_flush_once(fd);
        user_prefetch_many((const void *)(uintptr_t)kernel_addr);
        wait_for_prefetch();
        after_user_prefetch[i] = kernel_measure_once(fd);
    }

    print_stats("kernel load after flush", cold, rounds);
    print_stats("kernel hot load", hot, rounds);
    print_stats("after kernel prefetch", after_kernel_prefetch, rounds);
    print_stats("after user prefetch", after_user_prefetch, rounds);

    cold_median = percentile(cold, rounds, 50);
    hot_median = percentile(hot, rounds, 50);
    kernel_prefetch_median = percentile(after_kernel_prefetch, rounds, 50);
    user_prefetch_median = percentile(after_user_prefetch, rounds, 50);

    printf("\n");
    printf("RESULT:\n");
    if (hot_median * 2 < cold_median && kernel_prefetch_median * 2 < cold_median) {
        if (user_prefetch_median * 2 < cold_median) {
            printf("SUCCESS: user-mode prefetcht0 warmed this kernel VA cache line.\n");
        } else {
            printf("NO KERNEL-LINE FILL OBSERVED: controls are hot, but user prefetch stays cold.\n");
            printf("On this system, user-mode prefetcht0 did not translate/fill this kernel VA.\n");
        }
    } else {
        printf("INCONCLUSIVE: kernel-side hot/prefetch controls did not separate from cold loads.\n");
        printf("Check CPU affinity, interrupts, virtualization noise, or the module measurement path.\n");
    }

    free(after_user_prefetch);
    free(after_kernel_prefetch);
    free(hot);
    free(cold);
    close(fd);
    return EXIT_SUCCESS;
}
