// probe_cpp.c
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static sigjmp_buf env;

static void sigill_handler(int sig)
{
    siglongjmp(env, 1);
}
static void enable_cpp_rctx_el0(void *unused)
{
    u64 value = read_sysreg(sctlr_el1);

    value |= BIT_ULL(10);       /* SCTLR_EL1.EnRCTX */
    write_sysreg(value, sctlr_el1);
    isb();
}

int main(void)
{
    on_each_cpu(enable_cpp_rctx_el0, NULL, 1);
    signal(SIGILL, sigill_handler);

    if (sigsetjmp(env, 1)) {
        puts("CPP RCTX: SIGILL at EL0");
        return 1;
    }

    asm volatile(
        "cpp rctx, xzr\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        ::: "memory");

    puts("CPP RCTX: executed successfully at EL0");
    return 0;
}