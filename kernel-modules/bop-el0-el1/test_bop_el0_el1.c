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

#define PAGE_POOL 8192
#define DEFAULT_ROUNDS 30
#define DEFAULT_TRAIN_PAIRS 3000
#define DEFAULT_OFFSET_LINES 16
#define DEFAULT_THRESHOLD_NS 80
#define LINE_BYTES 64

static uint64_t timer_freq_hz;

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

__attribute__((noinline)) static void train_load(void *address)
{
	asm volatile("ldr x1, [%0]" :: "r"(address) : "x1", "memory");
}

__attribute__((noinline)) static void trigger_load(void *address)
{
	asm volatile("ldr x17, [%0]" :: "r"(address) : "x17", "memory");
}

static uint64_t timed_reload(void *address)
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

static void train_user(void *mapping, size_t offset_lines,
			       size_t train_pairs, size_t round)
{
	for (size_t i = 0; i < train_pairs; i++) {
		size_t page = bop_el0_el1_pair_page((uint32_t)i, (uint32_t)round);
		unsigned char *base = (unsigned char *)mapping + page * PAGE_SIZE;
		unsigned char *target = base + offset_lines * LINE_BYTES;

		flush(base);
		flush(target);
		mfence();
		train_load(base);
		nops();
		train_load(target);
		nops();
	}
}

static int train_kernel(int fd, void *mapping, size_t offset_lines,
			 size_t train_pairs, size_t round, int stride_mode)
{
	unsigned char *kernel_training_base = (unsigned char *)mapping +
		(PAGE_POOL / 2) * PAGE_SIZE;

	for (size_t i = 0; i < train_pairs; i++) {
		size_t page = bop_el0_el1_pair_page((uint32_t)i, (uint32_t)round);
		unsigned char *base = stride_mode ? kernel_training_base +
			i * offset_lines * LINE_BYTES :
			kernel_training_base + page * PAGE_SIZE;
		unsigned char *target = base + offset_lines * LINE_BYTES;

		flush(base);
		flush(target);
	}
	mfence();

	struct bop_el0_el1_request request = {
		.base_addr = (uint64_t)(uintptr_t)kernel_training_base,
		.offset_lines = (uint32_t)offset_lines,
		.repeats = (uint32_t)train_pairs,
		.mode = stride_mode ? BOP_EL0_EL1_TRAIN_STRIDE :
			BOP_EL0_EL1_TRAIN_PAIRS,
		.page_seed = (uint32_t)round,
	};

	return ioctl(fd, BOP_EL0_EL1_IOC_TRAIN, &request);
}

static int probe_case(const char *name, int fd, void *base,
			      size_t rounds, size_t offset_lines, size_t train_pairs,
			      size_t kernel_train_pairs, size_t threshold_ns,
			      int train, int kernel_train,
			      int kernel_trigger, int kernel_stride)
{
	uint64_t hits = 0;
	uint64_t total = 0;

	for (size_t round = 0; round < rounds; round++) {
		unsigned char *trigger = (unsigned char *)base +
			(PAGE_POOL - 1) * PAGE_SIZE;
		unsigned char *probe = trigger + offset_lines * LINE_BYTES;

		cpp_rctx();
		if (train)
			train_user(base, offset_lines, train_pairs, round);
		if (kernel_train && train_kernel(fd, base, offset_lines,
					  kernel_train_pairs, round, kernel_stride) != 0) {
			fprintf(stderr, "%s: kernel training ioctl: %s\n",
				name, strerror(errno));
			return -1;
		}
		flush(trigger);
		flush(probe);
		mfence();
		if (kernel_trigger) {
			struct bop_el0_el1_request request = {
				.base_addr = (uint64_t)(uintptr_t)trigger,
				.offset_lines = (uint32_t)offset_lines,
			};

			if (ioctl(fd, BOP_EL0_EL1_IOC_TRIGGER, &request) != 0) {
				fprintf(stderr, "%s: ioctl: %s\n", name, strerror(errno));
				return -1;
			}
		} else {
			trigger_load(trigger);
		}
		for (size_t i = 0; i < 2000; i++)
			asm volatile("nop" ::: "memory");
		if (timed_reload(probe) <= threshold_ns)
			hits++;
		total++;
	}

	printf("%s\t%llu\t%llu\t%.2f\n", name,
	       (unsigned long long)total, (unsigned long long)hits,
	       1000.0 * (double)hits / (double)total);
	return 0;
}

static int case_parameters(const char *case_name, int *train,
			       int *kernel_train, int *kernel_trigger, int *kernel_stride)
{
	if (strcmp(case_name, "user_trigger") == 0) {
		*train = 1;
		*kernel_train = 0;
		*kernel_trigger = 0;
		*kernel_stride = 0;
		return 0;
	}
	if (strcmp(case_name, "el0_only_user_trigger") == 0) {
		*train = 1;
		*kernel_train = 0;
		*kernel_trigger = 0;
		*kernel_stride = 0;
		return 0;
	}
	if (strcmp(case_name, "kernel_trigger") == 0) {
		*train = 1;
		*kernel_train = 0;
		*kernel_trigger = 1;
		*kernel_stride = 0;
		return 0;
	}
	if (strcmp(case_name, "untrained_kernel") == 0) {
		*train = 0;
		*kernel_train = 0;
		*kernel_trigger = 1;
		*kernel_stride = 0;
		return 0;
	}
	if (strcmp(case_name, "kernel_train_user_trigger") == 0) {
		*train = 1;
		*kernel_train = 1;
		*kernel_trigger = 0;
		*kernel_stride = 0;
		return 0;
	}
	if (strcmp(case_name, "kernel_stride_user_trigger") == 0) {
		*train = 1;
		*kernel_train = 1;
		*kernel_trigger = 0;
		*kernel_stride = 1;
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
	fprintf(stderr, "CASE: user_trigger, el0_only_user_trigger, "
		"kernel_trigger, untrained_kernel, kernel_train_user_trigger, "
		"or kernel_stride_user_trigger\n");
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
		else if ((value = strtoull(optarg, NULL, 0)) == 0)
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
	int kernel_trigger;
	int kernel_stride;

	if (case_name == NULL || case_parameters(case_name, &train, &kernel_train,
						 &kernel_trigger, &kernel_stride) != 0 ||
	    offset_lines == 0 || offset_lines >= PAGE_SIZE / LINE_BYTES ||
	    rounds == 0 || train_pairs == 0 ||
	    train_pairs > BOP_EL0_EL1_TRAIN_PAGES ||
	    kernel_train_pairs > BOP_EL0_EL1_TRAIN_PAGES || threshold_ns == 0)
		return 1;
	if (kernel_train && kernel_train_pairs == 0)
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
	printf("# offset_lines=%zu rounds=%zu el0_train_pairs=%zu "
	       "el1_train_pairs=%zu threshold_ns=%zu\n", offset_lines, rounds,
	       train_pairs, kernel_train_pairs, threshold_ns);
	printf("case\tprobes\thits\thits_per_1000\n");
	if (probe_case(case_name, fd, mapping, rounds,
		       offset_lines, train_pairs, kernel_train_pairs, threshold_ns, train,
		       kernel_train, kernel_trigger, kernel_stride) != 0) {
		munmap(mapping, PAGE_POOL * PAGE_SIZE);
		close(fd);
		return 1;
	}
	munmap(mapping, PAGE_POOL * PAGE_SIZE);
	close(fd);
	return 0;
}
