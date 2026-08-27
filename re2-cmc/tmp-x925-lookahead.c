#define main test_cmc_base_main
#include "test-cmc.c"
#undef main

#define MAX_LOOKAHEAD (NODE_COUNT - 1)
#define SCANNED_NODE_COUNT (NODE_COUNT - 1)

struct lookahead_result {
    uint64_t samples;
    uint64_t hits;
    uint64_t latency_ns;
    uint64_t l1_events;
    uint64_t l2_events;
    uint64_t l1_active_samples;
    uint64_t l1_active_hits;
    uint64_t l2_active_samples;
    uint64_t l2_active_hits;
};

static struct lookahead_result results[2][SCANNED_NODE_COUNT]
                                              [MAX_LOOKAHEAD];
static uint8_t *prelude_mapping;

static void *prelude_addr(size_t index)
{
    size_t page = (index * 73 + 19) % MAPPING_PAGES;
    size_t line = (index * 29 + 7) % LINES_PER_PAGE;

    return prelude_mapping + page * PAGE_SIZE + line * LINE_SIZE;
}

static void run_lookahead_sample(int trained, size_t node_index,
                                 size_t distance, int training_replays,
                                 uint64_t threshold_ns,
                                 size_t prefix_misses,
                                 struct lookahead_result *result)
{
    void *trigger = node_addr(node_index);
    size_t target_index = (node_index + distance) % NODE_COUNT;
    void *target = node_addr(target_index);
    uint64_t l1_delta = 0;
    uint64_t l2_delta = 0;
    int hit = 0;
#if ENABLE_PMU
    struct prefetch_pmu_snapshot before;
    struct prefetch_pmu_snapshot after;
#endif

    cpp_rctx();
    if (trained)
        train_sequence(training_replays);

    /* Every target starts absent. Probe exactly one target in this sample. */
    flush_nodes();
    for (size_t i = 0; i < prefix_misses; i++)
        flush(prelude_addr(i));
    mfence();
    for (size_t i = 0; i < prefix_misses; i++) {
        cmc_access(prelude_addr(i));
        nops();
    }

#if ENABLE_PMU
    before = prefetch_pmu_read();
#endif
    cmc_access(trigger);
    wait_for_prefetch();
#if ENABLE_PMU
    after = prefetch_pmu_read();
    l1_delta = prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
    l2_delta = prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
    result->l1_events += l1_delta;
    result->l2_events += l2_delta;
#endif

#if ENABLE_CACHE_PROBE
    {
        uint64_t latency = timed_reload_ns(target);

        result->latency_ns += latency;
        hit = latency <= threshold_ns;
        result->hits += hit;
    }
#else
    (void)target;
    (void)threshold_ns;
#endif
    if (l1_delta != 0) {
        result->l1_active_samples++;
        result->l1_active_hits += hit;
    }
    if (l2_delta != 0) {
        result->l2_active_samples++;
        result->l2_active_hits += hit;
    }
    result->samples++;
}

static void run_lookahead_experiment(int samples, int training_replays,
                                     uint64_t threshold_ns,
                                     size_t maximum_lookahead,
                                     int trigger_node,
                                     size_t prefix_misses)
{
    for (int sample = 0; sample < samples; sample++) {
        size_t node_count = trigger_node < 0 ? SCANNED_NODE_COUNT : 1;

        for (size_t node_slot = 0; node_slot < node_count;
             node_slot++) {
            size_t node_index = trigger_node < 0 ?
                (node_slot + (size_t)sample) % SCANNED_NODE_COUNT :
                (size_t)trigger_node;

            size_t distance_count = maximum_lookahead;

            for (size_t distance_slot = 0;
                 distance_slot < distance_count; distance_slot++) {
                size_t distance = 1 +
                    (distance_slot + (size_t)sample + node_index) %
                    distance_count;
                int first = sample & 1;
                int second = first ^ 1;

                run_lookahead_sample(
                    first, node_index, distance, training_replays,
                    threshold_ns, prefix_misses,
                    &results[first][node_index][distance - 1]);
                run_lookahead_sample(
                    second, node_index, distance, training_replays,
                    threshold_ns, prefix_misses,
                    &results[second][node_index][distance - 1]);
            }
        }
    }
}

static void print_lookahead_case(const char *name, int trained,
                                 size_t maximum_lookahead,
                                 int trigger_node)
{
    printf("\n[%s]\n", name);
    printf("# node distance target_node target_page target_line samples "
           "hits per_1000 avg_latency_ns l1_events avg_l1_pmu "
           "l2_events avg_l2_pmu l1_active_samples l1_active_hits "
           "l2_active_samples l2_active_hits\n");
    for (size_t node_index = 0; node_index < SCANNED_NODE_COUNT;
         node_index++) {
        if (trigger_node >= 0 && node_index != (size_t)trigger_node)
            continue;
        size_t distance_count = maximum_lookahead;

        for (size_t distance = 1; distance <= distance_count; distance++) {
            size_t target_index = (node_index + distance) % NODE_COUNT;
            const struct lookahead_result *result =
                &results[trained][node_index][distance - 1];
            double denominator = result->samples == 0 ?
                1.0 : (double)result->samples;

            printf("%3zu %3zu %3zu %3zu %2zu %5llu %5llu %8.2f "
                   "%14.2f %9llu %10.4f %9llu %10.4f "
                   "%9llu %9llu %9llu %9llu\n",
                   node_index, distance, target_index,
                   nodes[target_index].page, nodes[target_index].line,
                   (unsigned long long)result->samples,
                   (unsigned long long)result->hits,
                   1000.0 * (double)result->hits / denominator,
                   (double)result->latency_ns / denominator,
                   (unsigned long long)result->l1_events,
                   (double)result->l1_events / denominator,
                   (unsigned long long)result->l2_events,
                   (double)result->l2_events / denominator,
                   (unsigned long long)result->l1_active_samples,
                   (unsigned long long)result->l1_active_hits,
                   (unsigned long long)result->l2_active_samples,
                   (unsigned long long)result->l2_active_hits);
        }
    }
}

static void print_lookahead_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [samples_per_node_distance threshold_ns "
            "training_replays maximum_lookahead trigger_node "
            "prefix_misses]\n",
            program);
}

int main(int argc, char **argv)
{
    int samples = 40;
    uint64_t threshold_ns = 80;
    int training_replays = 1;
    size_t maximum_lookahead = MAX_LOOKAHEAD;
    int trigger_node = -1;
    size_t prefix_misses = 0;
    int result = 0;

    if (argc != 1 && argc != 7) {
        print_lookahead_usage(argv[0]);
        return 1;
    }
    if (argc == 7) {
        samples = atoi(argv[1]);
        threshold_ns = strtoull(argv[2], NULL, 0);
        training_replays = atoi(argv[3]);
        maximum_lookahead = strtoull(argv[4], NULL, 0);
        trigger_node = atoi(argv[5]);
        prefix_misses = strtoull(argv[6], NULL, 0);
    }
    if (samples <= 0 || threshold_ns == 0 || training_replays <= 0 ||
        maximum_lookahead == 0 || maximum_lookahead > MAX_LOOKAHEAD) {
        print_lookahead_usage(argv[0]);
        return 1;
    }
    if (trigger_node < -1 || trigger_node >= SCANNED_NODE_COUNT) {
        print_lookahead_usage(argv[0]);
        return 1;
    }
    if (prefix_misses > MAPPING_PAGES) {
        print_lookahead_usage(argv[0]);
        return 1;
    }

    init_nodes();
    timer_freq_hz = read_cntfrq_el0();
    if (timer_freq_hz == 0) {
        fprintf(stderr, "invalid cntfrq_el0 value\n");
        return 1;
    }

    mapping_size = (size_t)MAPPING_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    memset(mapping, 0xa5, mapping_size);
    for (size_t offset = 0; offset < mapping_size; offset += LINE_SIZE)
        mLoad_inline(mapping + offset);
    flush_nodes();

    prelude_mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                           -1, 0);
    if (prelude_mapping == MAP_FAILED) {
        fprintf(stderr, "prelude mmap failed: %s\n", strerror(errno));
        munmap(mapping, mapping_size);
        return 1;
    }
    memset(prelude_mapping, 0x5a, mapping_size);

    printf("# X925 CMC temporal-lookahead attribution test\n");
    printf("# samples_per_node_distance=%d training_replays=%d "
           "nodes=%d scanned_nodes=%d lookahead=1..%zu threshold_ns=%llu "
           "wait_nops=%d trigger_node=%d prefix_misses=%zu "
           "cpp_rctx=each_sample\n",
           samples, training_replays, NODE_COUNT, SCANNED_NODE_COUNT,
           maximum_lookahead, (unsigned long long)threshold_ns,
           PREFETCH_WAIT_NOPS, trigger_node, prefix_misses);
    printf("# target_node=(trigger_node+lookahead)%%nodes; wrap=cyclic\n");
    printf("# each sample probes exactly one target after one trigger\n");
    printf("# enable_pmu=%d enable_cache_probe=%d\n",
           ENABLE_PMU, ENABLE_CACHE_PROBE);

#if ENABLE_PMU
    pmu_ready = (pmu_setup() == 0);
    if (!pmu_ready) {
        fprintf(stderr, "# direct PMU counter setup failed\n");
        result = 2;
        goto cleanup;
    }
#else
    printf("# PMU disabled by ENABLE_PMU=0\n");
#endif

    run_lookahead_experiment(samples, training_replays, threshold_ns,
                             maximum_lookahead, trigger_node, prefix_misses);
    print_lookahead_case("untrained", 0, maximum_lookahead, trigger_node);
    print_lookahead_case("trained", 1, maximum_lookahead, trigger_node);

cleanup:
#if ENABLE_PMU
    pmu_cleanup();
#endif
    if (munmap(mapping, mapping_size) != 0) {
        fprintf(stderr, "munmap failed: %s\n", strerror(errno));
        return 1;
    }
    if (munmap(prelude_mapping, mapping_size) != 0) {
        fprintf(stderr, "prelude munmap failed: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
