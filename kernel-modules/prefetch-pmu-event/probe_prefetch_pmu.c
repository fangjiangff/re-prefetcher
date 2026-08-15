#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "prefetch_pmu_event.h"
#include "prefetch_pmu_ioctl.h"

int main(void)
{
	struct prefetch_pmu_snapshot before;
	struct prefetch_pmu_snapshot after;
	int fd;
	int cpu = sched_getcpu();

	if (cpu < 0) {
		fprintf(stderr, "sched_getcpu failed: %s\n", strerror(errno));
		return 1;
	}

	fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n",
			PREFETCH_PMU_DEVICE_PATH, strerror(errno));
		return 1;
	}
	if (ioctl(fd, PREFETCH_PMU_IOC_REARM) != 0) {
		fprintf(stderr, "PMU rearm failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	before = prefetch_pmu_read();
	asm volatile(
		".rept 1000\n\t"
		"nop\n\t"
		".endr\n\t"
		:::
		"memory");
	after = prefetch_pmu_read();

	printf("cpu=%d counter4_l1 before=%llu after=%llu delta=%u\n",
	       cpu, (unsigned long long)before.l1_hwprf,
	       (unsigned long long)after.l1_hwprf,
	       prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf));
	printf("cpu=%d counter5_l2 before=%llu after=%llu delta=%u\n",
	       cpu, (unsigned long long)before.l2_prefetch,
	       (unsigned long long)after.l2_prefetch,
	       prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch));
	close(fd);
	return 0;
}
