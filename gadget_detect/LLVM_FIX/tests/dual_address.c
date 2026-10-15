__attribute__((noinline))
int dual_load_min(volatile unsigned char *base, unsigned secret) {
  if (secret)
    return base[0] + base[64];
  return 0;
}

__attribute__((noinline))
void dual_store_max(volatile unsigned char *base, unsigned secret) {
  if (secret) {
    base[0] = 1;
    base[1984] = 2;
  }
}

__attribute__((noinline))
int dual_mixed(volatile unsigned char *base, unsigned secret) {
  if (secret) {
    int value = base[0];
    base[64] = value;
    return value;
  }
  return 0;
}

__attribute__((noinline))
int dual_store_load(volatile unsigned char *base, unsigned secret) {
  if (secret) {
    base[0] = 1;
    return base[64];
  }
  return 0;
}

__attribute__((noinline))
int dual_after_empty_block(volatile unsigned char *base, unsigned secret,
                           unsigned gate) {
  if (secret) {
    int value = base[0];
    if (gate)
      __asm__ volatile("nop" ::: "memory");
    return value + base[64];
  }
  return 0;
}

__attribute__((noinline))
int dual_too_near(volatile unsigned char *base, unsigned secret) {
  if (secret)
    return base[0] + base[63];
  return 0;
}

__attribute__((noinline))
int dual_too_far(volatile unsigned char *base, unsigned secret) {
  if (secret)
    return base[0] + base[1985];
  return 0;
}
