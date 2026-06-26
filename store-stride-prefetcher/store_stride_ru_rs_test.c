#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "until.h"

#define CACHELINE 64
#define TARGET_INDEX 30
#define MAX_INDEX 40
#define TRAIN_N 6
#define CORE1_PROBE_N 3
#define ITEMS 10240
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

enum test_mode {
	MODE_BASELINE = 0,
	MODE_STORE_STRIDE,
	MODE_STORE_STRIDE_STORE30,
	MODE_LOAD_STRIDE,
	MODE_BROKEN_STRIDE,
	MODE_POSITIVE,
	MODE_COUNT
};

static const char *mode_names[MODE_COUNT] = {
	"baseline",
	"store_stride",
	"store_stride_store30",
	"load_stride",
	"broken_stride",
	"positive_store_T",
};

static const int core1_probe_idx[CORE1_PROBE_N] = { 30, 35, 40 };

struct alignas_cacheline_u64 {
	volatile uint64_t v;
	char pad[CACHELINE - sizeof(uint64_t)];
} __attribute__((aligned(CACHELINE)));

struct config {
	int core0;
	int core1;
	uint64_t iterations;
	uint64_t warmup;
	uint64_t delay_nops;
	size_t elem_stride_bytes;
};

struct shared_state {
	struct alignas_cacheline_u64 stage;
	struct alignas_cacheline_u64 start;
	struct alignas_cacheline_u64 ready0;
	struct alignas_cacheline_u64 ready1;
	struct alignas_cacheline_u64 sink0;
	struct alignas_cacheline_u64 sink1;
	uint8_t *base;
	uint8_t *dummy_buffer;
	volatile uint64_t *target;
	uint64_t *lat[MODE_COUNT];
	struct config cfg;
};

static uint8_t array2[ITEMS * CACHELINE] __attribute__((aligned(PAGE_SIZE)));

static inline void cpu_relax(void)
{
	asm volatile("yield" ::: "memory");
}

static inline void wait_until(volatile uint64_t *p, uint64_t value)
{
	while (__atomic_load_n(p, __ATOMIC_ACQUIRE) != value)
		cpu_relax();
}

static inline void publish(volatile uint64_t *p, uint64_t value)
{
	__atomic_store_n(p, value, __ATOMIC_RELEASE);
}

static inline void delay_nops(uint64_t nops)
{
	uint64_t i;

	for (i = 0; i < nops; i++)
		nop();
}

static int pin_to_core(int core)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(core, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0) {
		fprintf(stderr, "sched_setaffinity(core=%d) failed: %s\n",
			core, strerror(errno));
		return -1;
	}

	return 0;
}

static inline volatile uint64_t *addr_at(struct shared_state *s, int idx)
{
	return (volatile uint64_t *)(void *)(s->base +
					     (size_t)idx * s->cfg.elem_stride_bytes);
}

static void flush_test_array(struct shared_state *s)
{
	size_t offset;
	size_t used = ((size_t)MAX_INDEX + 1) * s->cfg.elem_stride_bytes;

	for (offset = 0; offset < used; offset += CACHELINE)
		flush(s->base + offset);
}

static void warm_test_array(struct shared_state *s)
{
	size_t offset;
	size_t used = ((size_t)MAX_INDEX + 1) * s->cfg.elem_stride_bytes;

	for (offset = 0; offset < used; offset += CACHELINE)
		mLoad_noinline(s->base + offset);
}

static void dummy_accesses(struct shared_state *s)
{
	dummyAccess(s->dummy_buffer, DUMMY_BUFFER_SIZE);
}

static void run_baseline(struct shared_state *s)
{
	(void)s;
}

static void run_store_stride(struct shared_state *s, uint64_t iter)
{
	static const int idx[TRAIN_N] = { 0, 5, 10, 15, 20, 25 };
	int i;

	for (i = 0; i < TRAIN_N; i++)
		mStore_noinline((void *)addr_at(s, idx[i]));

	s->sink1.v += iter;
}

static void run_load_stride(struct shared_state *s)
{
	static const int idx[TRAIN_N] = { 0, 5, 10, 15, 20, 25 };
	int i;

	for (i = 0; i < TRAIN_N; i++)
		mLoad_noinline((void *)addr_at(s, idx[i]));

	s->sink1.v++;
}

static void run_broken_stride(struct shared_state *s, uint64_t iter)
{
	static const int idx[TRAIN_N] = { 0, 17, 3, 22, 9, 25 };
	int i;

	for (i = 0; i < TRAIN_N; i++)
		mStore_noinline((void *)addr_at(s, idx[i]));

	s->sink1.v += iter;
}

static void run_store_stride_store30(struct shared_state *s, uint64_t iter)
{
	run_store_stride(s, iter);
	mStore_noinline((void *)s->target);
}

static void run_positive(struct shared_state *s, uint64_t iter)
{
	mStore_noinline((void *)s->target);
	s->sink1.v += iter;
}

static void *observer_thread(void *arg)
{
	struct shared_state *s = arg;
	uint64_t total = s->cfg.warmup + s->cfg.iterations;
	uint64_t i;
	int mode;

	if (pin_to_core(s->cfg.core0) != 0)
		exit(1);

	publish(&s->ready0.v, 1);
	wait_until(&s->start.v, 1);

	for (mode = 0; mode < MODE_COUNT; mode++) {
		for (i = 0; i < total; i++) {
			uint64_t t0, t1;

			wait_until(&s->stage.v, 0);

			mLoad_noinline((void *)s->target);
			s->sink0.v++;
			publish(&s->stage.v, 1);

			wait_until(&s->stage.v, 2);

			t0 = timestamp();
			mLoad_noinline((void *)s->target);
			t1 = timestamp();

			s->sink0.v++;
			if (i >= s->cfg.warmup)
				s->lat[mode][i - s->cfg.warmup] = t1 - t0;

			publish(&s->stage.v, 3);
		}
	}

	return NULL;
}

static void *prefetcher_thread(void *arg)
{
	struct shared_state *s = arg;
	uint64_t total = s->cfg.warmup + s->cfg.iterations;
	uint64_t i;
	int mode;

	if (pin_to_core(s->cfg.core1) != 0)
		exit(1);

	publish(&s->ready1.v, 1);
	wait_until(&s->start.v, 1);

	for (mode = 0; mode < MODE_COUNT; mode++) {
		for (i = 0; i < total; i++) {
			wait_until(&s->stage.v, 1);

			dummy_accesses(s);

			switch (mode) {
			case MODE_BASELINE:
				run_baseline(s);
				break;
			case MODE_STORE_STRIDE:
				run_store_stride(s, i);
				break;
			case MODE_STORE_STRIDE_STORE30:
				run_store_stride_store30(s, i);
				break;
			case MODE_LOAD_STRIDE:
				run_load_stride(s);
				break;
			case MODE_BROKEN_STRIDE:
				run_broken_stride(s, i);
				break;
			case MODE_POSITIVE:
				run_positive(s, i);
				break;
			default:
				break;
			}

			delay_nops(s->cfg.delay_nops);
			publish(&s->stage.v, 2);

			wait_until(&s->stage.v, 3);
			publish(&s->stage.v, 0);
		}
	}

	return NULL;
}

static void print_stats(struct shared_state *s)
{
	uint64_t n = s->cfg.iterations;
	int mode;

	printf("# core0=%d core1=%d iterations=%" PRIu64
	       " warmup=%" PRIu64 " delay_nops=%" PRIu64
	       " elem_stride_bytes=%zu target_index=%d\n",
	       s->cfg.core0, s->cfg.core1, s->cfg.iterations, s->cfg.warmup,
	       s->cfg.delay_nops, s->cfg.elem_stride_bytes, TARGET_INDEX);
	printf("# cross_core_avg_latency_ns\n");
	printf("# mode,avg_ns\n");

	for (mode = 0; mode < MODE_COUNT; mode++) {
		uint64_t sum = 0;
		uint64_t i;

		for (i = 0; i < n; i++)
			sum += s->lat[mode][i];

		printf("%s,%.2f\n", mode_names[mode], (double)sum / (double)n);
	}
}

static void run_core1_probe_case(struct shared_state *s, int explicit_store_30)
{
	uint64_t total = s->cfg.warmup + s->cfg.iterations;
	uint64_t sums[CORE1_PROBE_N] = { 0 };
	int p;

	if (pin_to_core(s->cfg.core1) != 0)
		exit(1);

	for (p = 0; p < CORE1_PROBE_N; p++) {
		uint64_t i;

		for (i = 0; i < total; i++) {
			uint64_t t0, dt;

			dummy_accesses(s);
			flush_test_array(s);
			run_store_stride(s, i);
			if (explicit_store_30)
				mStore_noinline((void *)addr_at(s, TARGET_INDEX));
			delay_nops(s->cfg.delay_nops);

			t0 = timestamp();
			mLoad_noinline((void *)addr_at(s, core1_probe_idx[p]));
			dt = timestamp() - t0;

			if (i >= s->cfg.warmup)
				sums[p] += dt;
		}
	}

	printf("# %s\n", explicit_store_30 ?
	       "core1_explicit_store30_probe_avg_latency_ns" :
	       "core1_store_stride_prefetch_probe_avg_latency_ns");
	printf("# position,avg_ns\n");
	for (p = 0; p < CORE1_PROBE_N; p++) {
		printf("%d,%.2f\n", core1_probe_idx[p],
		       (double)sums[p] / (double)s->cfg.iterations);
	}
}

static void run_core1_prefetch_probe(struct shared_state *s)
{
	run_core1_probe_case(s, 0);
	run_core1_probe_case(s, 1);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -a CORE   observer core, default 0\n"
		"  -b CORE   prefetcher core, default 1\n"
		"  -n N      measured iterations per mode, default 100000\n"
		"  -w N      warmup iterations per mode, default 10000\n"
		"  -d NOPS   nop delay after Core1 accesses, default 100\n"
		"  -s BYTES  bytes between logical array elements, default 320\n"
		"  -h        show this help\n"
		"\n"
		"Example:\n"
		"  %s -a 0 -b 1 -n 200000 -w 20000 -d 100\n",
		prog, prog);
}

static uint64_t parse_u64(const char *s, const char *name)
{
	char *end = NULL;
	uint64_t v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || !end || *end != '\0') {
		fprintf(stderr, "invalid %s: %s\n", name, s);
		exit(1);
	}
	return v;
}

int main(int argc, char **argv)
{
	struct shared_state *s;
	pthread_t t0, t1;
	size_t alloc_size;
	int opt, mode;

	s = calloc(1, sizeof(*s));
	if (!s) {
		perror("calloc shared state");
		return 1;
	}

	s->cfg.core0 = 0;
	s->cfg.core1 = 1;
	s->cfg.iterations = 100000;
	s->cfg.warmup = 10000;
	s->cfg.delay_nops = 100;
	s->cfg.elem_stride_bytes = 5 * CACHELINE;

	while ((opt = getopt(argc, argv, "a:b:n:w:d:s:h")) != -1) {
		switch (opt) {
		case 'a':
			s->cfg.core0 = (int)parse_u64(optarg, "observer core");
			break;
		case 'b':
			s->cfg.core1 = (int)parse_u64(optarg, "prefetcher core");
			break;
		case 'n':
			s->cfg.iterations = parse_u64(optarg, "iterations");
			break;
		case 'w':
			s->cfg.warmup = parse_u64(optarg, "warmup");
			break;
		case 'd':
			s->cfg.delay_nops = parse_u64(optarg, "delay nops");
			break;
		case 's':
			s->cfg.elem_stride_bytes =
				(size_t)parse_u64(optarg, "element stride bytes");
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (s->cfg.iterations == 0 || s->cfg.elem_stride_bytes < CACHELINE) {
		fprintf(stderr, "iterations must be nonzero and stride >= %d\n",
			CACHELINE);
		return 1;
	}

	alloc_size = (MAX_INDEX + 1) * s->cfg.elem_stride_bytes + CACHELINE;
	if (alloc_size > sizeof(array2)) {
		fprintf(stderr, "test array range exceeds static array2 size\n");
		return 1;
	}

	s->base = array2;
	memset(array2, -1, sizeof(array2));
	s->target = addr_at(s, TARGET_INDEX);

	s->dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (s->dummy_buffer == MAP_FAILED) {
		perror("mmap dummy_buffer");
		return 1;
	}

	warm_test_array(s);
	flush_test_array(s);

	run_core1_prefetch_probe(s);

	for (mode = 0; mode < MODE_COUNT; mode++) {
		s->lat[mode] = malloc(s->cfg.iterations * sizeof(uint64_t));
		if (!s->lat[mode]) {
			perror("malloc latency buffer");
			return 1;
		}
	}

	if (pthread_create(&t0, NULL, observer_thread, s) != 0) {
		perror("pthread_create observer");
		return 1;
	}
	if (pthread_create(&t1, NULL, prefetcher_thread, s) != 0) {
		perror("pthread_create prefetcher");
		return 1;
	}

	wait_until(&s->ready0.v, 1);
	wait_until(&s->ready1.v, 1);
	publish(&s->stage.v, 0);
	publish(&s->start.v, 1);

	if (pthread_join(t0, NULL) != 0) {
		perror("pthread_join observer");
		return 1;
	}
	if (pthread_join(t1, NULL) != 0) {
		perror("pthread_join prefetcher");
		return 1;
	}

	print_stats(s);
	fprintf(stderr, "# sink=%" PRIu64 "\n", s->sink0.v + s->sink1.v);

	return 0;
}
