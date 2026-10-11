#include "until.h"

__attribute__((noinline)) void mStore_noinline(void *addr) {
    _mStore("", addr);
}

__attribute__((noinline)) void mLoad_noinline(void *addr) {
    _mLoad("", addr);
}

__attribute__((noinline)) void mPrefetch_noinline(void *addr) {
    _mPrefetch("", addr);
}
