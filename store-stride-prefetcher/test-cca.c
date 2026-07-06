// read_pfr0.c
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>

static sigjmp_buf jb;

static void sigill_handler(int signo) {
    siglongjmp(jb, 1);
}

int main(void) {
    uint64_t v;

    signal(SIGILL, sigill_handler);

    if (sigsetjmp(jb, 1)) {
        printf("Cannot read ID_AA64PFR0_EL1 from user space on this kernel.\n");
        return 1;
    }

    asm volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(v));

    unsigned el3 = (v >> 12) & 0xf;
    unsigned el2 = (v >> 8) & 0xf;
    unsigned rme = (v >> 52) & 0xf;

    printf("ID_AA64PFR0_EL1 = 0x%016lx\n", v);
    printf("EL2 field       = 0x%x\n", el2);
    printf("EL3 field       = 0x%x  %s\n", el3, el3 ? "EL3 implemented" : "EL3 not reported");
    printf("RME field       = 0x%x  %s\n", rme, rme ? "RME/CCA reported" : "No RME/CCA reported");

    return 0;
}