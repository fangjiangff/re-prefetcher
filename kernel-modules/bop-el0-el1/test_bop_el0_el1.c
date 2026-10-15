#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../until.h"
#include "bop_el0_el1_ioctl.h"
#include "../prefetch-pmu-event/prefetch_pmu_event.h"
#include "../prefetch-pmu-event/prefetch_pmu_ioctl.h"

#define PAGE_POOL 8192
#define DEFAULT_ROUNDS 30
#define DEFAULT_TRAIN_PAIRS 3000
#define DEFAULT_OFFSET_LINES 16
#define DEFAULT_THRESHOLD_NS 80
#define LINE_BYTES 64
#define PAGE_SEED 0x243f6a8885a308d3ULL

#ifndef ENABLE_PMU
#define ENABLE_PMU 1
#endif

static uint64_t timer_freq_hz;
static size_t page_order[PAGE_POOL];
static size_t slot_order[PAGE_POOL];

#if ENABLE_PMU
static int pmu_fd = -1;
static int pmu_ready;
#endif

static uint64_t splitmix64_next(uint64_t *state)
{
	uint64_t value;

	*state += 0x9e3779b97f4a7c15ULL;
	value = *state;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

static size_t base_line_count(size_t offset_lines)
{
	return PAGE_SIZE / LINE_BYTES - offset_lines;
}

static void init_slot_order(size_t offset_lines)
{
	uint64_t rng = PAGE_SEED ^ 0x9e3779b97f4a7c15ULL ^ offset_lines;

	for (size_t i = 0; i < PAGE_POOL; i++)
		slot_order[i] = i;
	for (size_t i = PAGE_POOL - 1; i > 0; i--) {
		size_t j = (size_t)(splitmix64_next(&rng) % (i + 1));
		size_t temporary = slot_order[i];

		slot_order[i] = slot_order[j];
		slot_order[j] = temporary;
	}
}

static void init_page_order(void)
{
	uint64_t rng = PAGE_SEED;

	for (size_t i = 0; i < PAGE_POOL; i++)
		page_order[i] = i;
	for (size_t i = PAGE_POOL - 1; i > 0; i--) {
		size_t j = (size_t)(splitmix64_next(&rng) % (i + 1));
		size_t temporary = page_order[i];

		page_order[i] = page_order[j];
		page_order[j] = temporary;
	}
}

static inline uint64_t read_cntvct_el0(void)
{
	uint64_t value;

	asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb"
		     : "=r"(value) :: "memory");
	return value;
}

static inline uint64_t read_cntfrq_el0(void)
{
	uint64_t value;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

static uint64_t ticks_to_ns(uint64_t ticks)
{
	__uint128_t product = (__uint128_t)ticks * 1000000000ULL;

	product += timer_freq_hz / 2;
	return (uint64_t)(product / timer_freq_hz);
}

#if ENABLE_PMU
static int pmu_setup(void)
{
	pmu_fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
	if (pmu_fd < 0)
		return -1;
	if (ioctl(pmu_fd, PREFETCH_PMU_IOC_REARM) != 0) {
		close(pmu_fd);
		pmu_fd = -1;
		return -1;
	}
	return 0;
}
#endif

__attribute__((noinline)) static void train_load_el0(void *address)
{
	asm volatile("ldr x1, [%0]" :: "r"(address) : "x1", "memory");
}

__attribute__((noinline)) static void trigger_load(void *address)
{
	asm volatile("ldr x17, [%0]" :: "r"(address) : "x17", "memory");
}

__attribute__((noinline)) static uint64_t timed_reload(void *address)
{
	uint64_t start;
	uint64_t end;

	mfence();
	start = read_cntvct_el0();
	mLoad_inline(address);
	end = read_cntvct_el0();
	mfence();
	flush(address);
	return ticks_to_ns(end - start);
}

static void prefault_mapping(void *mapping)
{
	unsigned char *bytes = mapping;
	uint64_t rng = PAGE_SEED ^ 0xa4093822299f31d0ULL;

	/* Allocate private pages; untouched anonymous loads can share the zero page. */
	for (size_t i = 0; i < PAGE_POOL; i++) {
		size_t page = page_order[i];
		size_t line = (size_t)(splitmix64_next(&rng) %
				       (PAGE_SIZE / LINE_BYTES));

		bytes[page * PAGE_SIZE + line * LINE_BYTES] = (unsigned char)i;
	}
	mfence();
}

static inline void training_wait(void)
{
	for (int k = 0; k < 100; k++)
		asm volatile("nop" ::: "memory");
}

static void train_user(void *mapping, size_t offset_lines,
			       size_t first_pair, size_t train_pairs)
{
	uint64_t rng = PAGE_SEED ^ ((uint64_t)offset_lines << 32);
	size_t line_count = base_line_count(offset_lines);

	for (size_t i = 0; i < first_pair + train_pairs; i++) {
		size_t page = slot_order[i];
		size_t line = (size_t)(splitmix64_next(&rng) % line_count);
		unsigned char *base = (unsigned char *)mapping +
			page * PAGE_SIZE + line * LINE_BYTES;
		unsigned char *target = base + offset_lines * LINE_BYTES;

		if (i < first_pair)
			continue;
		train_load_el0(base);
		training_wait();
		train_load_el0(target);
		training_wait();
	}
}

static int flush_kernel_training_pool(int fd, size_t offset_lines)
{
	struct bop_el0_el1_request request = {
		.offset_lines = (uint32_t)offset_lines,
	};

	return ioctl(fd, BOP_EL0_EL1_IOC_FLUSH, &request);
}

static int train_kernel(int fd, void *mapping, size_t offset_lines,
			 size_t first_pair, size_t train_pairs)
{
	struct bop_el0_el1_request request = {
		.base_addr = (uint64_t)(uintptr_t)mapping,
		.offset_lines = (uint32_t)offset_lines,
		.repeats = (uint32_t)train_pairs,
		.first_pair = (uint32_t)first_pair,
	};

	return ioctl(fd, BOP_EL0_EL1_IOC_TRAIN, &request);
}

static int switch_el(int fd)
{
	return ioctl(fd, BOP_EL0_EL1_IOC_SWITCH);
}

static int probe_case(const char *name, int fd, void *base,
			      size_t rounds, size_t offset_lines, size_t train_pairs,
			      size_t kernel_train_pairs, size_t threshold_ns,
			      int train, int kernel_train, int el_switch)
{
	int kernel_trigger = strcmp(name, "el0_train_kernel_trigger") == 0 ||
		strcmp(name, "untrained_kernel_trigger") == 0;
	uint64_t hits = 0;
	uint64_t total = 0;
#if ENABLE_PMU
	uint64_t l1_events = 0;
	uint64_t l2_events = 0;
#endif
	Mapping mapping_state = {
		.base_addr = base,
		.size = PAGE_POOL * PAGE_SIZE,
	};
	unsigned char *trigger = (unsigned char *)base +
		slot_order[PAGE_POOL - 1] * PAGE_SIZE;
	uint64_t trigger_rng = PAGE_SEED ^ 0x13198a2e03707344ULL;
	size_t trigger_line = (size_t)(splitmix64_next(&trigger_rng) %
					 base_line_count(offset_lines));

	trigger += trigger_line * LINE_BYTES;
	unsigned char *probe = trigger + offset_lines * LINE_BYTES;

	struct bop_el0_el1_request trigger_request = {
		.base_addr = (uint64_t)(uintptr_t)trigger,
	};
	printf("# trigger_level=%s pmu_window=%s\n",
	       kernel_trigger ? "EL1" : "EL0",
	       kernel_trigger ? "trigger_ioctl_and_wait" : "trigger_and_wait");

	for (size_t round = 0; round < rounds; round++) {
#if ENABLE_PMU
		struct prefetch_pmu_snapshot before = {0};
		struct prefetch_pmu_snapshot after = {0};
#endif

		flush_mapping(mapping_state);
		if (kernel_train && kernel_train_pairs != 0 &&
		    flush_kernel_training_pool(fd, offset_lines) != 0) {
			fprintf(stderr, "%s: kernel training flush ioctl: %s\n",
				name, strerror(errno));
			return -1;
		}
		cpp_rctx();
		if (train)
			train_user(base, offset_lines, 0, train_pairs);
		if (el_switch && switch_el(fd) != 0) {
			fprintf(stderr, "%s: EL switch ioctl: %s\n",
				name, strerror(errno));
			return -1;
		}
		if (kernel_train && kernel_train_pairs != 0 &&
		    train_kernel(fd, base, offset_lines, train_pairs,
				 kernel_train_pairs) != 0) {
			fprintf(stderr, "%s: kernel training ioctl: %s\n",
				name, strerror(errno));
			return -1;
		}
		if (el_switch && kernel_train_pairs != 0)
			train_user(base, offset_lines, train_pairs, kernel_train_pairs);
#if ENABLE_PMU
		if (pmu_ready)
			before = prefetch_pmu_read();
#endif
		if (kernel_trigger) {
			if (ioctl(fd, BOP_EL0_EL1_IOC_TRIGGER, &trigger_request) != 0) {
				fprintf(stderr, "%s: kernel trigger ioctl: %s\n", name, strerror(errno));
				return -1;
			}
		} else {
			trigger_load(trigger);
		}
		for (size_t i = 0; i < 2000; i++)
			asm volatile("nop" ::: "memory");
#if ENABLE_PMU
		if (pmu_ready) {
			after = prefetch_pmu_read();
			l1_events += prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
			l2_events += prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
		}
#endif
		if (timed_reload(probe) <= threshold_ns)
			hits++;
		total++;
	}

#if ENABLE_PMU
	if (pmu_ready) {
		printf("# PMU l1d_cache_hwprf total=%llu per_trigger=%.6f\n",
		       (unsigned long long)l1_events, (double)l1_events / (double)total);
		printf("# PMU l2d_cache_hwprf total=%llu per_trigger=%.6f\n",
		       (unsigned long long)l2_events, (double)l2_events / (double)total);
	} else {
		printf("# PMU unavailable\n");
	}
#endif
	printf("%s\t%llu\t%llu\t%.2f\n", name,
	       (unsigned long long)total, (unsigned long long)hits,
	       1000.0 * (double)hits / (double)total);
	return 0;
}

static int case_parameters(const char *case_name, int *train,
			       int *kernel_train, int *el_switch)
{
	if (strcmp(case_name, "el0_only_user_trigger") == 0 ||
	    strcmp(case_name, "el0_train_kernel_trigger") == 0) {
		*train = 1;
		*kernel_train = 0;
		*el_switch = 0;
		return 0;
	}
	if (strcmp(case_name, "untrained_user_trigger") == 0 ||
	    strcmp(case_name, "untrained_kernel_trigger") == 0) {
		*train = 0;
		*kernel_train = 0;
		*el_switch = 0;
		return 0;
	}
	if (strcmp(case_name, "el_switch_user_train") == 0) {
		*train = 1;
		*kernel_train = 0;
		*el_switch = 1;
		return 0;
	}
	if (strcmp(case_name, "split_train_user_trigger") == 0) {
		*train = 1;
		*kernel_train = 1;
		*el_switch = 0;
		return 0;
	}
	return -1;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s --case CASE [--device PATH] [--rounds N] "
		"[--train-pairs N] [--kernel-train-pairs N] "
		"[--offset-lines N] [--threshold-ns N] [--cpu N]\n", program);
	fprintf(stderr, "CASE: untrained_user_trigger, el0_only_user_trigger, "
		"el_switch_user_train, split_train_user_trigger, "
		"el0_train_kernel_trigger, untrained_kernel_trigger\n");
}

int main(int argc, char **argv)
{
	const char *device = BOP_EL0_EL1_DEVICE_PATH;
	const char *case_name = NULL;
	size_t rounds = DEFAULT_ROUNDS;
	size_t train_pairs = DEFAULT_TRAIN_PAIRS;
	size_t kernel_train_pairs = 0;
	size_t offset_lines = DEFAULT_OFFSET_LINES;
	size_t threshold_ns = DEFAULT_THRESHOLD_NS;
	size_t cpu = SIZE_MAX;
	int option;
	static const struct option options[] = {
		{"case", required_argument, NULL, 'a'},
		{"device", required_argument, NULL, 'd'},
		{"rounds", required_argument, NULL, 'r'},
		{"train-pairs", required_argument, NULL, 't'},
		{"kernel-train-pairs", required_argument, NULL, 'k'},
		{"offset-lines", required_argument, NULL, 'o'},
		{"threshold-ns", required_argument, NULL, 'n'},
		{"cpu", required_argument, NULL, 'c'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	while ((option = getopt_long(argc, argv, "a:d:r:t:k:o:n:c:h", options,
				     NULL)) != -1) {
		size_t value;

		if (option == 'h') {
			usage(argv[0]);
			return 0;
		}
		if (option == 'a')
			case_name = optarg;
		else if (option == 'd')
			device = optarg;
		else if ((value = strtoull(optarg, NULL, 0)) == 0 && option != 'k')
			return 1;
		else if (option == 'r')
			rounds = value;
		else if (option == 't')
			train_pairs = value;
		else if (option == 'k')
			kernel_train_pairs = value;
		else if (option == 'o')
			offset_lines = value;
		else if (option == 'n')
			threshold_ns = value;
		else if (option == 'c')
			cpu = value;
		else
			return 1;
	}
	int train;
	int kernel_train;
	int el_switch;
	size_t total_train_pairs;

	if (case_name == NULL || case_parameters(case_name, &train, &kernel_train,
					       &el_switch) != 0 ||
	    offset_lines == 0 || offset_lines >= PAGE_SIZE / LINE_BYTES ||
	    rounds == 0 ||
	    train_pairs > BOP_EL0_EL1_TRAIN_PAGES ||
	    kernel_train_pairs > BOP_EL0_EL1_TRAIN_PAGES || threshold_ns == 0)
		return 1;
	total_train_pairs = (train ? train_pairs : 0) +
		((kernel_train || el_switch) ? kernel_train_pairs : 0);
	if ((train && train_pairs == 0) ||
	    total_train_pairs > BOP_EL0_EL1_TRAIN_PAGES)
		return 1;
	if (cpu != SIZE_MAX) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) != 0)
			return 1;
	}

	timer_freq_hz = read_cntfrq_el0();
	if (timer_freq_hz == 0)
		return 1;
	int fd = open(device, O_RDWR | O_CLOEXEC);
	void *mapping;

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", device, strerror(errno));
		return 1;
	}
	mapping = mmap(NULL, PAGE_POOL * PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (mapping == MAP_FAILED)
		return 1;
	init_slot_order(offset_lines);
	init_page_order();
	prefault_mapping(mapping);
#if ENABLE_PMU
	pmu_ready = pmu_setup() == 0;
	if (!pmu_ready)
		fprintf(stderr, "# PMU unavailable; continuing cache-only\n");
#endif
	printf("# offset_lines=%zu rounds=%zu first_el0_pairs=%zu "
	       "second_pairs=%zu second_level=%s total_train_pairs=%zu threshold_ns=%zu\n",
	       offset_lines, rounds, train ? train_pairs : 0,
	       (kernel_train || el_switch) ? kernel_train_pairs : 0,
	       kernel_train ? "EL1" : (el_switch ? "EL0_after_switch" : "none"),
	       total_train_pairs, threshold_ns);
	printf("# mapping_flush=whole_mapping_each_round "
	       "address_sequence=fixed_seed_0 trigger_probe=prepared_once\n");
	printf("case\tprobes\thits\thits_per_1000\n");
	if (probe_case(case_name, fd, mapping, rounds,
		       offset_lines, train_pairs, kernel_train_pairs, threshold_ns, train,
		       kernel_train, el_switch) != 0) {
#if ENABLE_PMU
		if (pmu_fd >= 0)
			close(pmu_fd);
#endif
		munmap(mapping, PAGE_POOL * PAGE_SIZE);
		close(fd);
		return 1;
	}
#if ENABLE_PMU
	if (pmu_fd >= 0)
		close(pmu_fd);
#endif
	munmap(mapping, PAGE_POOL * PAGE_SIZE);
	close(fd);
	return 0;
}
