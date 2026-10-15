#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef __aarch64__
#error "This test requires AArch64 CPP RCTX and cache maintenance instructions."
#endif

#include "../until.h"

#define LINE_COUNT (PAGE_SIZE / LINE_SIZE)
#define TRAIN_PC_COUNT 16
#define POOL_PAGES 16384
#define DEFAULT_ROUNDS 30
#define DEFAULT_THRESHOLD_NS 80
#define DEFAULT_TRAIN_PAIRS 300
#define DEFAULT_LEFT_PAIRS 300
#define DEFAULT_TRIGGER_BATCH 64
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

enum left_mode { LEFT_NONE, LEFT_SINGLE_PC_STRIDE, LEFT_MULTI_PC_STRIDE,
                 LEFT_SINGLE_PC_PAGE_PAIR, LEFT_MULTI_PAGE_PAIR };
struct options { size_t rounds, threshold_ns, train_pairs, left_pairs;
                 size_t trigger_batch, offset_lines, cpu; enum left_mode mode; };
struct stats { uint64_t probes, hits, latency_ns; };
typedef void (*access_function)(void *);

static uint8_t *mapping;
static size_t mapping_size;
static uint64_t timer_freq_hz;

static uint64_t splitmix64_next(uint64_t *state)
{
    uint64_t value;

    *state += 0x9e3779b97f4a7c15ULL;
    value = *state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

#define DEFINE_ACCESS(number, reg)                                      \
    __attribute__((noinline)) static void train_pc##number(void *addr)   \
    { asm volatile("ldr " #reg ", [%0]" :: "r"(addr) : #reg, "memory"); }
DEFINE_ACCESS(0, x1) DEFINE_ACCESS(1, x2) DEFINE_ACCESS(2, x3)
DEFINE_ACCESS(3, x4) DEFINE_ACCESS(4, x5) DEFINE_ACCESS(5, x6)
DEFINE_ACCESS(6, x7) DEFINE_ACCESS(7, x8) DEFINE_ACCESS(8, x9)
DEFINE_ACCESS(9, x10) DEFINE_ACCESS(10, x11) DEFINE_ACCESS(11, x12)
DEFINE_ACCESS(12, x13) DEFINE_ACCESS(13, x14) DEFINE_ACCESS(14, x15)
DEFINE_ACCESS(15, x16)
static access_function const train_pcs[TRAIN_PC_COUNT] = {
    train_pc0, train_pc1, train_pc2, train_pc3, train_pc4, train_pc5,
    train_pc6, train_pc7, train_pc8, train_pc9, train_pc10, train_pc11,
    train_pc12, train_pc13, train_pc14, train_pc15,
};
__attribute__((noinline)) static void trigger_load(void *addr)
{ asm volatile("ldr x17, [%0]" :: "r"(addr) : "x17", "memory"); }

static inline uint64_t cntvct(void)
{ uint64_t v; asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(v)); return v; }
static inline uint64_t cntfrq(void)
{ uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
static uint64_t ticks_ns(uint64_t ticks)
{ return (uint64_t)(((__uint128_t)ticks * 1000000000ULL + timer_freq_hz / 2) / timer_freq_hz); }
static uint8_t *addr_at(size_t page, size_t line)
{ return mapping + page * PAGE_SIZE + line * LINE_SIZE; }
static uint64_t reload_ns(void *addr)
{ uint64_t a, b; mfence(); a = cntvct(); mLoad_inline(addr); b = cntvct(); mfence(); flush(addr); return ticks_ns(b - a); }

static void train_initial(const struct options *o, size_t round)
{
    uint64_t rng = 0x243f6a8885a308d3ULL ^ ((uint64_t)round * 977u);
    for (size_t i = 0; i < o->train_pairs; i++) {
        size_t page = (i * 977u + round) % (POOL_PAGES / 2);
        size_t line = (size_t)(splitmix64_next(&rng) % (LINE_COUNT - o->offset_lines));
        uint8_t *base = addr_at(page, line);
        uint8_t *target = addr_at(page, line + o->offset_lines);
        flush(base); flush(target); mfence();
        train_pcs[(i * 5u) % TRAIN_PC_COUNT](base); nops();
        train_pcs[(i * 7u + 3u) % TRAIN_PC_COUNT](target); nops();
    }
}

static void train_left_offset(const struct options *o, size_t round)
{
    size_t delta = o->offset_lines * LINE_SIZE;
    uint8_t *left_base = mapping + (POOL_PAGES / 2) * PAGE_SIZE;

    if (o->mode == LEFT_NONE)
        return;
    if (o->mode == LEFT_SINGLE_PC_STRIDE || o->mode == LEFT_MULTI_PC_STRIDE) {
        for (size_t i = 0; i < o->left_pairs; i++)
            flush(left_base + i * delta);
        mfence();
        for (size_t i = 0; i < o->left_pairs; i++) {
            uint8_t *address = left_base + i * delta;
            if (o->mode == LEFT_SINGLE_PC_STRIDE)
                train_pc0(address);
            else
                train_pcs[(i * 5u + round) % TRAIN_PC_COUNT](address);
            nops();
        }
        return;
    }
    for (size_t i = 0; i < o->left_pairs; i++) {
        size_t page = (i * 977u + round + 17u) % (POOL_PAGES / 2);
        size_t line = (i * 13u + 7u) % (LINE_COUNT - o->offset_lines);
        uint8_t *base = addr_at(page + POOL_PAGES / 2, line);
        uint8_t *target = addr_at(page + POOL_PAGES / 2,
                                  line + o->offset_lines);
        flush(base); flush(target); mfence();
        if (o->mode == LEFT_SINGLE_PC_PAGE_PAIR) {
            train_pc0(base); nops(); train_pc0(target); nops();
        } else {
            train_pcs[(i * 5u + round) % TRAIN_PC_COUNT](base); nops();
            train_pcs[(i * 7u + round + 3u) % TRAIN_PC_COUNT](target); nops();
        }
    }
}

static void prepare_triggers(const struct options *o, size_t round,
                             void **triggers, void **targets)
{
    size_t first_page = POOL_PAGES - o->trigger_batch - 1;
    for (size_t i = 0; i < o->trigger_batch; i++) {
        size_t page = first_page + i;
        size_t line = (i * 11u + round) % (LINE_COUNT - o->offset_lines);
        triggers[i] = addr_at(page, line);
        targets[i] = addr_at(page, line + o->offset_lines);
        flush(triggers[i]); flush(targets[i]);
    }
    mfence();
}

static int parse_mode(const char *text, enum left_mode *mode)
{
    if (strcmp(text, "none") == 0) *mode = LEFT_NONE;
    else if (strcmp(text, "single_pc_stride") == 0) *mode = LEFT_SINGLE_PC_STRIDE;
    else if (strcmp(text, "multi_pc_stride") == 0) *mode = LEFT_MULTI_PC_STRIDE;
    else if (strcmp(text, "single_pc_page_pair") == 0) *mode = LEFT_SINGLE_PC_PAGE_PAIR;
    else if (strcmp(text, "multi_page_pair") == 0) *mode = LEFT_MULTI_PAGE_PAIR;
    else return -1;
    return 0;
}
static int parse_size(const char *text, size_t *value)
{
    char *end; unsigned long long v;
    errno = 0; v = strtoull(text, &end, 0);
    if (*text == '\0' || *end != '\0' || errno == ERANGE || v > SIZE_MAX || v == 0) return -1;
    *value = (size_t)v; return 0;
}
static void usage(const char *program)
{ fprintf(stderr, "usage: %s --mode MODE [--offset-lines N] [--train-pairs N] [--left-pairs N] [--rounds N] [--trigger-batch N] [--threshold-ns N] [--cpu N]\nMODE: none, single_pc_stride, multi_pc_stride, single_pc_page_pair, multi_page_pair\n", program); }

static int parse_options(int argc, char **argv, struct options *o)
{
    static const struct option long_options[] = {
        {"mode", required_argument, NULL, 'm'}, {"offset-lines", required_argument, NULL, 'o'},
        {"train-pairs", required_argument, NULL, 't'}, {"left-pairs", required_argument, NULL, 'l'},
        {"rounds", required_argument, NULL, 'r'}, {"trigger-batch", required_argument, NULL, 'b'},
        {"threshold-ns", required_argument, NULL, 'n'}, {"cpu", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0},
    };
    int option;
    while ((option = getopt_long(argc, argv, "m:o:t:l:r:b:n:c:h", long_options, NULL)) != -1) {
        size_t value;
        if (option == 'h') { usage(argv[0]); exit(0); }
        if (option == 'm') { if (parse_mode(optarg, &o->mode)) return -1; continue; }
        if (parse_size(optarg, &value)) return -1;
        if (option == 'o') o->offset_lines = value; else if (option == 't') o->train_pairs = value;
        else if (option == 'l') o->left_pairs = value; else if (option == 'r') o->rounds = value;
        else if (option == 'b') o->trigger_batch = value; else if (option == 'n') o->threshold_ns = value;
        else if (option == 'c') o->cpu = value; else return -1;
    }
    return optind == argc ? 0 : -1;
}

int main(int argc, char **argv)
{
    struct options o = { DEFAULT_ROUNDS, DEFAULT_THRESHOLD_NS, DEFAULT_TRAIN_PAIRS,
                         DEFAULT_LEFT_PAIRS, DEFAULT_TRIGGER_BATCH, 16, SIZE_MAX, LEFT_NONE };
    void **triggers, **targets; struct stats stats = {0}; int cpu;
    size_t delta;
    if (parse_options(argc, argv, &o) || o.offset_lines >= LINE_COUNT ||
        o.trigger_batch >= POOL_PAGES / 2 || o.train_pairs >= POOL_PAGES / 2 ||
        o.left_pairs == 0 || o.left_pairs >= POOL_PAGES / 2)
        { usage(argv[0]); return 1; }
    delta = o.offset_lines * LINE_SIZE;
    if (o.mode == LEFT_SINGLE_PC_STRIDE || o.mode == LEFT_MULTI_PC_STRIDE) {
        if (o.left_pairs > (POOL_PAGES / 2 * PAGE_SIZE) / delta) return 1;
    }
    if (o.cpu != SIZE_MAX) { cpu_set_t set; CPU_ZERO(&set); CPU_SET(o.cpu, &set); if (sched_setaffinity(0, sizeof(set), &set)) return 1; cpu = (int)o.cpu; }
    else cpu = sched_getcpu();
    timer_freq_hz = cntfrq(); if (timer_freq_hz == 0) return 1;
    mapping_size = (size_t)POOL_PAGES * PAGE_SIZE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mapping == MAP_FAILED) return 1;
    triggers = calloc(o.trigger_batch, sizeof(*triggers)); targets = calloc(o.trigger_batch, sizeof(*targets));
    if (!triggers || !targets) return 1;
    printf("# cpu=%d mode=%d offset_lines=%zu rounds=%zu train_pairs=%zu left_pairs=%zu trigger_batch=%zu threshold_ns=%zu\n", cpu, o.mode, o.offset_lines, o.rounds, o.train_pairs, o.left_pairs, o.trigger_batch, o.threshold_ns);
    for (size_t round = 0; round < o.rounds; round++) {
        cpp_rctx(); train_initial(&o, round); train_left_offset(&o, round);
        prepare_triggers(&o, round, triggers, targets);
        for (size_t i = 0; i < o.trigger_batch; i++) trigger_load(triggers[i]);
        for (size_t i = 0; i < 2000; i++) asm volatile("nop" ::: "memory");
        for (size_t i = 0; i < o.trigger_batch; i++) { uint64_t latency = reload_ns(targets[i]); stats.latency_ns += latency; stats.probes++; if (latency <= o.threshold_ns) stats.hits++; }
    }
    printf("mode\tprobes\thits\thits_per_1000\tavg_latency_ns\n");
    printf("%d\t%llu\t%llu\t%.2f\t%.2f\n", o.mode, (unsigned long long)stats.probes, (unsigned long long)stats.hits,
           1000.0 * (double)stats.hits / stats.probes, (double)stats.latency_ns / stats.probes);
    free(targets); free(triggers); munmap(mapping, mapping_size); return 0;
}
