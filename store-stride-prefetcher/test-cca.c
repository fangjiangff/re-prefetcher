#include <stdio.h>
#include <stdint.h>

int main(void) {
    uint64_t pfr0;
    asm volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(pfr0));

    unsigned el3 = (pfr0 >> 12) & 0xf;
    unsigned sel2 = (pfr0 >> 36) & 0xf;
    unsigned rme = (pfr0 >> 52) & 0xf;

    printf("ID_AA64PFR0_EL1 = 0x%016lx\n", pfr0);
    printf("EL3  = %u\n", el3);
    printf("SEL2 = %u\n", sel2);
    printf("RME  = %u\n", rme);

    return 0;
}