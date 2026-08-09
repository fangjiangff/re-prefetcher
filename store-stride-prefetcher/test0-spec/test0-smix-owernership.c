#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PMU_CORE_X925
#define PMU_CORE_X925 1
#endif

#ifndef PMU_CORE_A55
#define PMU_CORE_A55 0
#endif

#ifndef PMU_WINDOW_NAME
#define PMU_WINDOW_NAME "ownership-observer-per-round"
#endif

#include "../pmu.h"

#define CACHE_LINE_SIZE 64
#define ARRAY2_ENTRIES 256
#define LOGICAL_ARRAY1_SIZE 16
#define OUT_OF_BOUNDARY_X 64
#define TRAINING_VALUE 1
#define TARGET_VALUE 115
#define OLD_VALUE 1
#define NEW_VALUE 17

enum action {
    ACTION_NONE = 0,
    ACTION_LOAD,
    ACTION_STORE,
};

enum test_mode {
    MODE_IDLE = 0,
    MODE_ARCH_LOAD,
    MODE_ARCH_STORE,
    MODE_TRANSIENT_NO_ACCESS,
    MODE_TRANSIENT_LOAD,
    MODE_TRANSIENT_STORE,
    MODE_COUNT,
};

static const char *const mode_names[MODE_COUNT] = {
    "idle",
    "architectural_load",
    "architectural_store",
    "transient_no_access",
    "transient_load",
    "transient_store",
};

struct cacheline_u64 {
    volatile uint64_t value;
    uint8_t padding[CACHE_LINE_SIZE - sizeof(uint64_t)];
} __attribute__((aligned(CACHE_LINE_SIZE)));

struct cacheline_size {
    volatile size_t value;
    uint8_t padding[CACHE_LINE_SIZE - sizeof(size_t)];
} __attribute__((aligned(CACHE_LINE_SIZE)));

struct config {
    int actor_core;
    int observer_core;
    uint64_t iterations;
    uint64_t warmup;
    uint64_t delay_nops;
    int train_times;
    uint64_t hit_threshold_ns;
};

struct mode_result {
    uint64_t *latencies;
    uint64_t *spec_latencies;
    uint64_t hits;
    uint64_t spec_hits;
    uint64_t old_values;
    uint64_t new_values;
    uint64_t other_values;
    uint64_t preload_bad;
};

struct shared_state {
    struct cacheline_u64 stage;
    struct cacheline_u64 start;
    struct cacheline_u64 actor_ready;
    struct cacheline_u64 observer_ready;
    struct cacheline_u64 actor_sink;
    struct cacheline_u64 observer_sink;
    struct config cfg;
    struct mode_result results[MODE_COUNT];
    struct mode_result store_validation;
};

static struct cacheline_size array1_size = {
    .value = LOGICAL_ARRAY1_SIZE,
};
static volatile uint8_t array1[ARRAY2_ENTRIES]
    __attribute__((aligned(CACHE_LINE_SIZE)));
static volatile uint8_t array2[ARRAY2_ENTRIES * CACHE_LINE_SIZE]
    __attribute__((aligned(4096)));
static volatile uint8_t spec_witness[ARRAY2_ENTRIES * CACHE_LINE_SIZE]
    __attribute__((aligned(4096)));

static int observer_pmu_ready;

static inline void cpu_relax(void)
{
    asm volatile("yield" ::: "memory");
}

static inline void full_barrier(void)
{
    asm volatile("dsb ish\n\tisb" ::: "memory");
}

static inline void flush_memory(const volatile void *address)
{
    asm volatile(
        "dc civac, %0\n\t"
        "dsb ish\n\t"
        "isb\n\t"
        :
        : "r"(address)
        : "memory");
}

static inline uint64_t get_time_ns(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static inline void wait_until(volatile uint64_t *address, uint64_t value)
{
    while (__atomic_load_n(address, __ATOMIC_ACQUIRE) != value)
        cpu_relax();
}

static inline void publish(volatile uint64_t *address, uint64_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELEASE);
}

static int pin_current_thread(int core)
{
    cpu_set_t cpus;
    int error;

    CPU_ZERO(&cpus);
    CPU_SET(core, &cpus);
    error = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (error != 0) {
        fprintf(stderr, "pthread_setaffinity_np(core=%d): %s\n",
                core, strerror(error));
        return -1;
    }
    return 0;
}

static inline volatile uint8_t *target_address(void)
{
    return &array2[TARGET_VALUE * CACHE_LINE_SIZE];
}

static inline volatile uint8_t *training_address(void)
{
    return &array2[TRAINING_VALUE * CACHE_LINE_SIZE];
}

static inline volatile uint8_t *witness_address(void)
{
    return &spec_witness[TARGET_VALUE * CACHE_LINE_SIZE];
}

/*
 * Keep one bounds-check PC for all transient modes. The action argument changes
 * only the memory operation after the bounds check. Valid training iterations
 * touch TRAINING_VALUE; the out-of-boundary iteration selects TARGET_VALUE.
 */
__attribute__((noinline))
static void bounded_gadget(size_t x, enum action action)
{
    if (x < array1_size.value) {
        uint8_t index = array1[x];
        volatile uint8_t *address = &array2[(size_t)index * CACHE_LINE_SIZE];

        if (action == ACTION_LOAD) {
            asm volatile("ldrb wzr, [%0]" : : "r"(address) : "memory");
        } else if (action == ACTION_STORE) {
            uint32_t value = NEW_VALUE;
            asm volatile("strb %w1, [%0]"
                         :
                         : "r"(address), "r"(value)
                         : "memory");
        } else {
            asm volatile("" : : "r"(address) : "memory");
        }

        /*
         * A separate cache line records that the mispredicted path reached the
         * end of the gadget. array2[TARGET_VALUE] cannot serve as this witness
         * because the observer deliberately caches that line before the actor
         * runs in order to measure ownership loss.
         */
        volatile uint8_t *witness =
            &spec_witness[(size_t)index * CACHE_LINE_SIZE];
        asm volatile("ldrb wzr, [%0]" : : "r"(witness) : "memory");
    }
}

/* Match the direct-store gadget shape used by test0-smix-pmu.c. */
__attribute__((noinline))
static void transient_store_gadget(size_t x)
{
    if (x < array1_size.value) {
        uint8_t index = array1[x];
        volatile uint8_t *address =
            &array2[(size_t)index * CACHE_LINE_SIZE];
        uint32_t value = NEW_VALUE;

        asm volatile("strb %w1, [%0]"
                     :
                     : "r"(address), "r"(value)
                     : "memory");

        volatile uint8_t *witness =
            &spec_witness[(size_t)index * CACHE_LINE_SIZE];
        asm volatile("ldrb wzr, [%0]" : : "r"(witness) : "memory");
    }
}

static void delay_nops(uint64_t count)
{
    for (uint64_t i = 0; i < count; i++)
        asm volatile("nop");
}

static void run_transient_case(struct shared_state *state, enum action action)
{
    /*
     * Make the selected index available quickly, while making the bounds load
     * slow. PMU is already running for the whole mode, so no syscall is inserted
     * between training and the trigger.
     */
    state->actor_sink.value += array1[OUT_OF_BOUNDARY_X];

    for (int i = 0; i < state->cfg.train_times; i++)
        bounded_gadget(0, action);

    flush_memory(&array1_size.value);
    bounded_gadget(OUT_OF_BOUNDARY_X, action);
}

/*
 * Reproduce test0-smix-pmu.c: TRAIN_TIMES valid calls followed by one
 * branchlessly selected out-of-boundary call, with a slow bounds load for
 * every invocation. The validation phase additionally flushes array2 exactly
 * as the PMU test does; ownership trials must preserve the observer copy.
 */
static void run_transient_store_sequence(struct shared_state *state,
                                         int flush_array2)
{
    size_t training_x = 0;

    for (int64_t j = state->cfg.train_times; j >= 0; j--) {
        size_t x = ((j % (state->cfg.train_times + 1)) - 1) & ~0xFFFFULL;
        x = x | (x >> 16);
        x = training_x ^ (x & (OUT_OF_BOUNDARY_X ^ training_x));

        if (flush_array2) {
            for (size_t i = 0; i < ARRAY2_ENTRIES; i++)
                flush_memory(&array2[i * CACHE_LINE_SIZE]);
        }
        flush_memory(&array1_size.value);
        transient_store_gadget(x);
        asm volatile("dsb ish" ::: "memory");
    }
}

static void validate_transient_store(struct shared_state *state)
{
    struct mode_result *result = &state->store_validation;
    uint64_t total = state->cfg.warmup + state->cfg.iterations;

    for (uint64_t iteration = 0; iteration < total; iteration++) {
        volatile uint8_t *target = target_address();
        uint64_t begin;
        uint64_t elapsed;
        uint8_t value;

        *target = OLD_VALUE;
        full_barrier();
        run_transient_store_sequence(state, 1);

        begin = get_time_ns();
        value = *target;
        full_barrier();
        elapsed = get_time_ns() - begin;
        state->actor_sink.value += value;

        if (iteration >= state->cfg.warmup) {
            uint64_t index = iteration - state->cfg.warmup;

            result->latencies[index] = elapsed;
            result->hits += elapsed < state->cfg.hit_threshold_ns;
            if (value == OLD_VALUE)
                result->old_values++;
            else if (value == NEW_VALUE)
                result->new_values++;
            else
                result->other_values++;
        }
    }
}

static void run_actor_mode(struct shared_state *state, enum test_mode mode)
{
    volatile uint8_t *target = target_address();

    switch (mode) {
    case MODE_IDLE:
        asm volatile("" ::: "memory");
        break;
    case MODE_ARCH_LOAD:
        state->actor_sink.value += *target;
        break;
    case MODE_ARCH_STORE:
        *target = NEW_VALUE;
        break;
    case MODE_TRANSIENT_NO_ACCESS:
        run_transient_case(state, ACTION_NONE);
        break;
    case MODE_TRANSIENT_LOAD:
        run_transient_case(state, ACTION_LOAD);
        break;
    case MODE_TRANSIENT_STORE:
        run_transient_store_sequence(state, 0);
        break;
    default:
        break;
    }

    delay_nops(state->cfg.delay_nops);
    full_barrier();
}

static void *actor_thread(void *argument)
{
    struct shared_state *state = argument;
    uint64_t total = state->cfg.warmup + state->cfg.iterations;

    if (pin_current_thread(state->cfg.actor_core) != 0)
        return (void *)1;

    publish(&state->actor_ready.value, 1);
    wait_until(&state->start.value, 1);

    validate_transient_store(state);

    for (int mode = 0; mode < MODE_COUNT; mode++) {
        for (uint64_t iteration = 0; iteration < total; iteration++) {
            volatile uint8_t *target = target_address();

            wait_until(&state->stage.value, 0);

            /*
             * Establish old memory contents, then invalidate all cached copies.
             * The observer reloads the line after this point and becomes its
             * owner/shared holder before the actor operation begins.
             */
            *target = OLD_VALUE;
            full_barrier();
            flush_memory(target);
            flush_memory(witness_address());
            publish(&state->stage.value, 1);

            wait_until(&state->stage.value, 2);
            run_actor_mode(state, (enum test_mode)mode);
            publish(&state->stage.value, 3);

            wait_until(&state->stage.value, 4);
            publish(&state->stage.value, 0);
        }
    }

    return NULL;
}

static void *observer_thread(void *argument)
{
    struct shared_state *state = argument;
    uint64_t total = state->cfg.warmup + state->cfg.iterations;

    if (pin_current_thread(state->cfg.observer_core) != 0)
        return (void *)1;

    publish(&state->observer_ready.value, 1);
    wait_until(&state->start.value, 1);

    for (int mode = 0; mode < MODE_COUNT; mode++) {
        int pmu_running = 0;

        pmu_reset_accumulated();

        for (uint64_t iteration = 0; iteration < total; iteration++) {
            volatile uint8_t *target = target_address();
            uint8_t preload_value;
            uint8_t observed_value;
            uint8_t witness_value;
            uint64_t begin;
            uint64_t elapsed;
            uint64_t spec_begin;
            uint64_t spec_elapsed;

            if (iteration == state->cfg.warmup && observer_pmu_ready) {
                pmu_running = (pmu_start() == 0);
                if (!pmu_running) {
                    fprintf(stderr,
                            "# PMU unavailable: observer counters could not start\n");
                    observer_pmu_ready = 0;
                }
            }

            wait_until(&state->stage.value, 1);

            /*
             * This load establishes the observer's private-cache copy. No target
             * flush is permitted until after the timed probe.
             */
            preload_value = *target;
            full_barrier();
            publish(&state->stage.value, 2);

            wait_until(&state->stage.value, 3);

            begin = get_time_ns();
            observed_value = *target;
            full_barrier();
            elapsed = get_time_ns() - begin;

            spec_begin = get_time_ns();
            witness_value = *witness_address();
            full_barrier();
            spec_elapsed = get_time_ns() - spec_begin;

            state->observer_sink.value += observed_value + witness_value;
            if (iteration >= state->cfg.warmup) {
                uint64_t index = iteration - state->cfg.warmup;
                struct mode_result *result = &state->results[mode];

                result->latencies[index] = elapsed;
                result->spec_latencies[index] = spec_elapsed;
                result->hits += elapsed < state->cfg.hit_threshold_ns;
                result->spec_hits +=
                    spec_elapsed < state->cfg.hit_threshold_ns;
                result->preload_bad += preload_value != OLD_VALUE;
                if (observed_value == OLD_VALUE)
                    result->old_values++;
                else if (observed_value == NEW_VALUE)
                    result->new_values++;
                else
                    result->other_values++;
            }

            publish(&state->stage.value, 4);
        }

        if (pmu_running)
            pmu_stop_and_accumulate();

        printf("# PMU_MODE %s\n", mode_names[mode]);
        if (pmu_running)
            pmu_print_accumulated(state->cfg.iterations);
        else
            printf("# PMU unavailable for this mode\n");
    }

    return NULL;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *values, uint64_t count,
                           unsigned int percent)
{
    uint64_t *copy;
    uint64_t index;
    uint64_t answer;

    copy = malloc((size_t)count * sizeof(*copy));
    if (copy == NULL) {
        perror("malloc percentile");
        exit(1);
    }

    memcpy(copy, values, (size_t)count * sizeof(*copy));
    qsort(copy, (size_t)count, sizeof(*copy), compare_u64);
    index = ((count - 1) * percent) / 100;
    answer = copy[index];
    free(copy);
    return answer;
}

static void print_results(const struct shared_state *state)
{
    printf("# actor_core=%d observer_core=%d iterations=%" PRIu64
           " warmup=%" PRIu64 " train_times=%d delay_nops=%" PRIu64
           " hit_threshold_ns=%" PRIu64 "\n",
           state->cfg.actor_core, state->cfg.observer_core,
           state->cfg.iterations, state->cfg.warmup,
           state->cfg.train_times, state->cfg.delay_nops,
           state->cfg.hit_threshold_ns);
    printf("# target=array2[%d*%d] old=%d transient_store_value=%d\n",
           TARGET_VALUE, CACHE_LINE_SIZE, OLD_VALUE, NEW_VALUE);
    printf("# interpretation: observer miss + old value => ownership/invalidation "
           "without store-data commit\n");
    printf("# spec witness is a separate line loaded at the end of the "
           "bounds-check wrong path\n");
    printf("mode\tavg_ns\tp50_ns\tp95_ns\thit_rate\told\tnew\tother\tpreload_bad"
           "\tspec_avg_ns\tspec_hit_rate\n");

    {
        const struct mode_result *result = &state->store_validation;
        uint64_t sum = 0;

        for (uint64_t i = 0; i < state->cfg.iterations; i++)
            sum += result->latencies[i];

        printf("transient_store_validation\t%.2f\t%" PRIu64
               "\t%" PRIu64 "\t%.6f\t%" PRIu64 "\t%" PRIu64
               "\t%" PRIu64 "\t0\t0.00\t0.000000\n",
               (double)sum / (double)state->cfg.iterations,
               percentile(result->latencies, state->cfg.iterations, 50),
               percentile(result->latencies, state->cfg.iterations, 95),
               (double)result->hits / (double)state->cfg.iterations,
               result->old_values, result->new_values, result->other_values);
    }

    for (int mode = 0; mode < MODE_COUNT; mode++) {
        const struct mode_result *result = &state->results[mode];
        uint64_t sum = 0;
        uint64_t spec_sum = 0;

        for (uint64_t i = 0; i < state->cfg.iterations; i++) {
            sum += result->latencies[i];
            spec_sum += result->spec_latencies[i];
        }

        printf("%s\t%.2f\t%" PRIu64 "\t%" PRIu64 "\t%.6f\t%" PRIu64
               "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%.2f\t%.6f\n",
               mode_names[mode],
               (double)sum / (double)state->cfg.iterations,
               percentile(result->latencies, state->cfg.iterations, 50),
               percentile(result->latencies, state->cfg.iterations, 95),
               (double)result->hits / (double)state->cfg.iterations,
               result->old_values, result->new_values,
               result->other_values, result->preload_bad,
               (double)spec_sum / (double)state->cfg.iterations,
               (double)result->spec_hits / (double)state->cfg.iterations);
    }
}

static uint64_t parse_u64(const char *text, const char *name)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(1);
    }
    return (uint64_t)value;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -a CORE   actor core (default 6, Cortex-X925)\n"
            "  -b CORE   observer core (default 7, Cortex-X925)\n"
            "  -n N      measured iterations per mode (default 10000)\n"
            "  -w N      warmup iterations per mode (default 1000)\n"
            "  -t N      valid predictor-training calls per trial (default 15)\n"
            "  -T NS     observer cache-hit threshold (default 80 ns)\n"
            "  -d NOPS   delay after actor operation (default 100)\n"
            "  -h        show this help\n",
            program);
}

int main(int argc, char **argv)
{
    struct shared_state *state;
    pthread_t actor;
    pthread_t observer;
    void *actor_status = NULL;
    void *observer_status = NULL;
    long online_cpus;
    int option;

#if !defined(__aarch64__)
    fprintf(stderr, "This experiment requires AArch64 cache-maintenance instructions.\n");
    return 2;
#endif

    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        perror("calloc shared_state");
        return 1;
    }

    state->cfg.actor_core = 6;
    state->cfg.observer_core = 7;
    state->cfg.iterations = 10000;
    state->cfg.warmup = 1000;
    state->cfg.delay_nops = 100;
    state->cfg.train_times = 15;
    state->cfg.hit_threshold_ns = 80;

    while ((option = getopt(argc, argv, "a:b:n:w:t:T:d:h")) != -1) {
        switch (option) {
        case 'a':
            state->cfg.actor_core = (int)parse_u64(optarg, "actor core");
            break;
        case 'b':
            state->cfg.observer_core = (int)parse_u64(optarg, "observer core");
            break;
        case 'n':
            state->cfg.iterations = parse_u64(optarg, "iterations");
            break;
        case 'w':
            state->cfg.warmup = parse_u64(optarg, "warmup");
            break;
        case 't':
            state->cfg.train_times = (int)parse_u64(optarg, "train times");
            break;
        case 'T':
            state->cfg.hit_threshold_ns = parse_u64(optarg, "hit threshold");
            break;
        case 'd':
            state->cfg.delay_nops = parse_u64(optarg, "delay nops");
            break;
        case 'h':
            usage(argv[0]);
            free(state);
            return 0;
        default:
            usage(argv[0]);
            free(state);
            return 1;
        }
    }

    online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (state->cfg.actor_core < 0 || state->cfg.observer_core < 0 ||
        state->cfg.actor_core == state->cfg.observer_core ||
        state->cfg.actor_core >= online_cpus ||
        state->cfg.observer_core >= online_cpus) {
        fprintf(stderr, "actor/observer cores must be distinct online CPUs\n");
        free(state);
        return 1;
    }
    if (state->cfg.iterations == 0 || state->cfg.hit_threshold_ns == 0) {
        fprintf(stderr, "iterations and hit threshold must be nonzero\n");
        free(state);
        return 1;
    }

    memset((void *)array1, 0, sizeof(array1));
    memset((void *)array2, OLD_VALUE, sizeof(array2));
    memset((void *)spec_witness, OLD_VALUE, sizeof(spec_witness));
    array1[0] = TRAINING_VALUE;
    array1[OUT_OF_BOUNDARY_X] = TARGET_VALUE;
    *training_address() = OLD_VALUE;
    *target_address() = OLD_VALUE;
    full_barrier();

    for (int mode = 0; mode < MODE_COUNT; mode++) {
        state->results[mode].latencies =
            calloc((size_t)state->cfg.iterations, sizeof(uint64_t));
        state->results[mode].spec_latencies =
            calloc((size_t)state->cfg.iterations, sizeof(uint64_t));
        if (state->results[mode].latencies == NULL ||
            state->results[mode].spec_latencies == NULL) {
            perror("calloc latencies");
            return 1;
        }
    }

    state->store_validation.latencies =
        calloc((size_t)state->cfg.iterations, sizeof(uint64_t));
    if (state->store_validation.latencies == NULL) {
        perror("calloc store validation latencies");
        return 1;
    }

    observer_pmu_ready = (pmu_setup_cpu(state->cfg.observer_core) == 0);
    if (!observer_pmu_ready)
        fprintf(stderr, "# PMU unavailable; latency/value experiment will continue\n");

    if (pthread_create(&actor, NULL, actor_thread, state) != 0) {
        perror("pthread_create actor");
        return 1;
    }
    if (pthread_create(&observer, NULL, observer_thread, state) != 0) {
        perror("pthread_create observer");
        return 1;
    }

    wait_until(&state->actor_ready.value, 1);
    wait_until(&state->observer_ready.value, 1);
    publish(&state->start.value, 1);

    if (pthread_join(actor, &actor_status) != 0) {
        perror("pthread_join actor");
        return 1;
    }
    if (pthread_join(observer, &observer_status) != 0) {
        perror("pthread_join observer");
        return 1;
    }
    if (actor_status != NULL || observer_status != NULL) {
        fprintf(stderr, "worker thread failed\n");
        return 1;
    }

    print_results(state);
    printf("# sink=%" PRIu64 "\n",
           state->actor_sink.value + state->observer_sink.value);

    pmu_cleanup();
    for (int mode = 0; mode < MODE_COUNT; mode++) {
        free(state->results[mode].latencies);
        free(state->results[mode].spec_latencies);
    }
    free(state->store_validation.latencies);
    free(state);
    return 0;
}
