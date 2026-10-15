#if defined(TRAIN_STORE) && TRAIN_STORE
#error "test-repeat-offset requires load training"
#endif

__attribute__((noinline, noclone))
static void repeat_offset_pair(void *base, void *mate)
{
    asm volatile(
        "ldr x1, [%0]\n\t"
        ".rept 100\n\t"
        "nop\n\t"
        ".endr\n\t"
        "ldr x2, [%1]\n\t"
        ".rept 100\n\t"
        "nop\n\t"
        ".endr"
        :: "r"(base), "r"(mate) : "x1", "x2", "memory");
}

#define BOP_TRAIN_PAIR(base, mate) repeat_offset_pair(base, mate)
#define BOP_TEST_TITLE "BOP repeated-function offset test"
#define BOP_TRAINING_PC_POLICY "repeated_single_function_pair"
#include "test-bop.c"
