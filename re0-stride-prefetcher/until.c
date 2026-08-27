#include "../until.h"
#include <signal.h>

#if defined(__aarch64__) && defined(ENABLE_CPP_RCTX) && ENABLE_CPP_RCTX
static void cpp_rctx_el0_sigill(int signo)
{
    static const char message[] =
        "ERROR: CPP RCTX is not available at EL0.\n"
        "On Cortex-A725, please load the rctx_el0 kernel module first:\n"
        "  sudo /home/jiangfang/workspace/FetchBench/covert-channel/kernel-modules/rctx-el0/module_load.sh\n";

    (void)signo;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(132);
}

/*
 * Probe EL0 access once before main().  Keeping the SIGILL handler confined to
 * this probe avoids adding signal-handling overhead to benchmark hot paths.
 */
__attribute__((constructor)) static void check_cpp_rctx_el0_access(void)
{
    struct sigaction action = {0};
    struct sigaction old_action;

    action.sa_handler = cpp_rctx_el0_sigill;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, &old_action) != 0)
        return;

    asm volatile("cpp rctx, xzr" ::: "memory");
    (void)sigaction(SIGILL, &old_action, NULL);
}
#endif


__attribute__((noinline)) void mStore_noinline(void *addr) {
    _mStore("", addr);
}

__attribute__((noinline)) void mLoad_noinline(void *addr) {
    _mLoad("", addr);
}

__attribute__((noinline)) void mPrefetch_noinline(void *addr) {
    _mPrefetch("", addr);
}
