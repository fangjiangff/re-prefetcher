#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

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
    __asm__ volatile("prefetcht0 (%0)" : : "r"(addr));
}

static inline void clflush_line(const void *addr)
{
    _mm_clflush(addr);
}

static inline uint64_t rdtscp_cycles(void)
{
    unsigned aux = 0;
    _mm_lfence();
    return __rdtscp(&aux);
}

static int try_read_byte(const void *addr, unsigned char *value)
{
    if (sigsetjmp(g_jmp_env, 1) != 0) {
        return -1;
    }

    *value = *(const volatile unsigned char *)addr;
    return 0;
}

static int try_prefetch_addr(const void *addr)
{
    if (sigsetjmp(g_jmp_env, 1) != 0) {
        return -1;
    }

    sw_prefetch(addr);
    _mm_lfence();
    return 0;
}

static uint64_t timed_load(const void *addr)
{
    volatile unsigned char value = 0;
    uint64_t t0 = 0;
    uint64_t t1 = 0;

    t0 = rdtscp_cycles();
    value = *(const volatile unsigned char *)addr;
    _mm_lfence();
    t1 = rdtscp_cycles();

    (void)value;
    return t1 - t0;
}

static uint64_t timed_load_after_prefetch(const void *addr)
{
    sw_prefetch(addr);

    for (volatile int i = 0; i < 128; ++i) {
        _mm_pause();
    }

    return timed_load(addr);
}

static void print_result_line(
    const char *label,
    const void *addr,
    int read_rc,
    int prefetch_rc,
    unsigned char value
)
{
    printf(
        "%-18s addr=%p | load=%s",
        label,
        addr,
        read_rc == 0 ? "ok" : "FAULT"
    );

    if (read_rc == 0) {
        printf(" (value=0x%02x)", value);
    }

    printf(" | prefetch=%s\n", prefetch_rc == 0 ? "ok" : "FAULT");
}

static uintptr_t parse_kernel_addr(int argc, char **argv)
{
    if (argc >= 2) {
        char *endptr = NULL;
        unsigned long long value = strtoull(argv[1], &endptr, 0);

        if (errno != 0 || endptr == argv[1] || *endptr != '\0') {
            fprintf(stderr, "invalid kernel address: %s\n", argv[1]);
            exit(EXIT_FAILURE);
        }

        return (uintptr_t)value;
    }

    return 0xffff800000000000ULL;
}

int main(int argc, char **argv)
{
    const long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t kernel_addr = 0;
    unsigned char *mapped_page = NULL;
    unsigned char *prot_none_page = NULL;
    void *unmapped_page = NULL;
    unsigned char read_value = 0;
    uint64_t miss_cycles = 0;
    uint64_t prefetched_cycles = 0;
    int rc = 0;

    if (page_size <= 0) {
        fprintf(stderr, "failed to get page size\n");
        return EXIT_FAILURE;
    }

    kernel_addr = parse_kernel_addr(argc, argv);
    install_fault_handlers();

    mapped_page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped_page == MAP_FAILED) {
        perror("mmap mapped_page");
        return EXIT_FAILURE;
    }

    prot_none_page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (prot_none_page == MAP_FAILED) {
        perror("mmap prot_none_page");
        munmap(mapped_page, (size_t)page_size);
        return EXIT_FAILURE;
    }

    unmapped_page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (unmapped_page == MAP_FAILED) {
        perror("mmap unmapped_page");
        munmap(prot_none_page, (size_t)page_size);
        munmap(mapped_page, (size_t)page_size);
        return EXIT_FAILURE;
    }

    mapped_page[0] = 0x5a;
    prot_none_page[0] = 0xa5;

    if (mprotect(prot_none_page, (size_t)page_size, PROT_NONE) != 0) {
        perror("mprotect PROT_NONE");
        munmap(unmapped_page, (size_t)page_size);
        munmap(prot_none_page, (size_t)page_size);
        munmap(mapped_page, (size_t)page_size);
        return EXIT_FAILURE;
    }

    if (munmap(unmapped_page, (size_t)page_size) != 0) {
        perror("munmap unmapped_page");
        munmap(prot_none_page, (size_t)page_size);
        munmap(mapped_page, (size_t)page_size);
        return EXIT_FAILURE;
    }

    printf("Software prefetch on arbitrary virtual addresses\n");
    printf("page size            : %ld bytes\n", page_size);
    printf("kernel test address  : 0x%016" PRIxPTR "\n", kernel_addr);
    printf("\n");
    printf("A real load should fault on inaccessible targets.\n");
    printf("A software prefetch should retire without fault on the same canonical addresses.\n");
    printf("\n");

    read_value = 0;
    rc = try_read_byte(mapped_page, &read_value);
    print_result_line("mapped-rw", mapped_page, rc, try_prefetch_addr(mapped_page), read_value);

    read_value = 0;
    rc = try_read_byte(prot_none_page, &read_value);
    print_result_line("mapped-prot-none", prot_none_page, rc, try_prefetch_addr(prot_none_page), read_value);

    read_value = 0;
    rc = try_read_byte(unmapped_page, &read_value);
    print_result_line("unmapped", unmapped_page, rc, try_prefetch_addr(unmapped_page), read_value);

    read_value = 0;
    rc = try_read_byte((const void *)kernel_addr, &read_value);
    print_result_line("kernel-half", (const void *)kernel_addr, rc,
                      try_prefetch_addr((const void *)kernel_addr), read_value);

    printf("\n");
    printf("Mapped-page timing sanity check\n");
    printf("The second measurement should usually be much smaller because prefetch warms the line.\n");

    clflush_line(mapped_page);
    _mm_mfence();
    miss_cycles = timed_load(mapped_page);

    clflush_line(mapped_page);
    _mm_mfence();
    prefetched_cycles = timed_load_after_prefetch(mapped_page);

    printf("load after clflush   : %" PRIu64 " cycles\n", miss_cycles);
    printf("load after prefetch  : %" PRIu64 " cycles\n", prefetched_cycles);

    printf("\n");
    printf("Interpretation\n");
    printf("1. If `load=FAULT` but `prefetch=ok`, the CPU accepted that virtual address for software prefetch.\n");
    printf("2. `mapped-prot-none` shows prefetch can target a mapped but inaccessible page.\n");
    printf("3. `unmapped` shows prefetch can target a canonical address with no user mapping.\n");
    printf("4. `kernel-half` uses a canonical high-half address. If you know a specific mapped kernel VA,\n");
    printf("   pass it as argv[1] to test that exact address.\n");
    printf("\n");
    printf("Note: non-canonical addresses are a different case and may still fault on x86-64.\n");

    munmap(prot_none_page, (size_t)page_size);
    munmap(mapped_page, (size_t)page_size);
    return EXIT_SUCCESS;
}
