#ifndef PREFETCH_PMU_EVENT_H
#define PREFETCH_PMU_EVENT_H

#include <stdint.h>

#ifndef __aarch64__
#error "prefetch_pmu_event.h requires AArch64"
#endif

struct prefetch_pmu_snapshot {
	uint64_t l1_hwprf;
	uint64_t l2_prefetch;
};

static inline __attribute__((always_inline)) uint64_t
prefetch_pmu_read_l1(void)
{
	uint64_t value;

	asm volatile(
		"isb\n\t"
		"mrs %0, pmevcntr4_el0\n\t"
		"isb\n\t"
		: "=r"(value)
		:
		: "memory");
	return value;
}

static inline __attribute__((always_inline)) uint64_t
prefetch_pmu_read_l2(void)
{
	uint64_t value;

	asm volatile(
		"isb\n\t"
		"mrs %0, pmevcntr5_el0\n\t"
		"isb\n\t"
		: "=r"(value)
		:
		: "memory");
	return value;
}

static inline __attribute__((always_inline)) struct prefetch_pmu_snapshot
prefetch_pmu_read(void)
{
	struct prefetch_pmu_snapshot snapshot;

	snapshot.l1_hwprf = prefetch_pmu_read_l1();
	snapshot.l2_prefetch = prefetch_pmu_read_l2();
	return snapshot;
}

/* A 32-bit modular delta is sufficient for the intended short windows. */
static inline uint32_t prefetch_pmu_delta(uint64_t after, uint64_t before)
{
	return (uint32_t)after - (uint32_t)before;
}

#endif
